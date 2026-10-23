#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {

// ============================================================================
// Geometry Dash GameObjectType Integer Constants (Cross-Version Safe)
// ============================================================================
constexpr int GD_OBJ_SOLID                = 1;
constexpr int GD_OBJ_HAZARD               = 2;
constexpr int GD_OBJ_INVERSE_GRAV_PORTAL  = 3;
constexpr int GD_OBJ_NORMAL_GRAV_PORTAL   = 4;
constexpr int GD_OBJ_YELLOW_PAD           = 8;
constexpr int GD_OBJ_PINK_PAD             = 9;
constexpr int GD_OBJ_GRAVITY_PAD          = 10;
constexpr int GD_OBJ_YELLOW_ORB           = 11;
constexpr int GD_OBJ_PINK_ORB             = 12;
constexpr int GD_OBJ_BLUE_GRAV_ORB        = 13;
constexpr int GD_OBJ_BREAKABLE            = 21;
constexpr int GD_OBJ_SLOPE                = 25;
constexpr int GD_OBJ_GREEN_ORB            = 29;
constexpr int GD_OBJ_BLACK_DROP_ORB       = 32;
constexpr int GD_OBJ_RED_PAD              = 34;
constexpr int GD_OBJ_RED_ORB              = 35;
constexpr int GD_OBJ_CUSTOM_ORB           = 36;
constexpr int GD_OBJ_DASH_ORB             = 37;
constexpr int GD_OBJ_GRAVITY_DASH_ORB     = 38;
constexpr int GD_OBJ_SPIDER_ORB           = 43;
constexpr int GD_OBJ_SPIDER_PAD           = 44;
constexpr int GD_OBJ_TELEPORT_ORB         = 46;
constexpr int GD_OBJ_ANIMATED_HAZARD      = 47;

// ============================================================================
// Configuration Helper (Synced with Geode Settings & In-Game Custom Popup UI)
// ============================================================================
struct ModConfig {
    bool enabled = true;
    int maxEarlyTicks = 8;        // 1 to 16 ticks
    int lookaheadTicks = 480;     // Default 480 ticks on 240 TPS (2.0s ahead)
    int baseTPS = 240;            // Default 240 ticks per second
    bool correctGroundJumps = true;
    bool correctOrbs = true;
    bool correctAirModes = true;
    bool correctReleases = true;
    bool preserveHoldLength = true;
    bool correctPlayer2 = true;
    bool cornerRescue = true;
    bool showMobileButton = true;
    bool showHud = true;

    static ModConfig load() {
        ModConfig cfg;
        auto* mod = Mod::get();
        cfg.enabled = mod->getSettingValue<bool>("enabled");
        cfg.maxEarlyTicks = static_cast<int>(std::clamp<int64_t>(
            mod->getSettingValue<int64_t>("max-early-ticks"), 1, 16
        ));
        cfg.lookaheadTicks = static_cast<int>(std::clamp<int64_t>(
            mod->getSettingValue<int64_t>("lookahead-ticks"), 60, 960
        ));
        cfg.baseTPS = static_cast<int>(std::clamp<int64_t>(
            mod->getSettingValue<int64_t>("base-tps"), 60, 480
        ));
        cfg.correctGroundJumps = mod->getSettingValue<bool>("correct-ground-jumps");
        cfg.correctOrbs = mod->getSettingValue<bool>("correct-orbs");
        cfg.correctAirModes = mod->getSettingValue<bool>("correct-air-modes");
        cfg.correctReleases = mod->getSettingValue<bool>("correct-releases");
        cfg.preserveHoldLength = mod->getSettingValue<bool>("preserve-hold-length");
        cfg.correctPlayer2 = mod->getSettingValue<bool>("correct-player2");
        cfg.cornerRescue = mod->getSettingValue<bool>("corner-rescue");
        cfg.showMobileButton = mod->getSettingValue<bool>("show-mobile-button");
        cfg.showHud = mod->getSettingValue<bool>("show-hud");
        return cfg;
    }
};

// ============================================================================
// Spatial Level Object Cache for Fast 240 TPS Lookahead Simulation
// ============================================================================
enum class CachedObjKind {
    Hazard,
    Solid,
    Slope,
    OrbOrRing,
    Pad,
    GravityPortal
};

struct CachedObject {
    GameObject* raw = nullptr;
    CachedObjKind kind = CachedObjKind::Hazard;
    int objectID = 0;
    int rawType = 0;
    CCRect hitbox;
};

class LevelSpatialIndex {
public:
    static constexpr int BUCKET_SIZE = 120; // 4 grid blocks wide (120 units)
    std::unordered_map<int, std::vector<CachedObject>> buckets;
    int totalIndexed = 0;

    void clear() {
        buckets.clear();
        totalIndexed = 0;
    }

    static CCRect computeApproxHitbox(GameObject* obj, CachedObjKind kind) {
        CCPoint pos = obj->getPosition();
        float sx = std::abs(obj->getScaleX());
        float sy = std::abs(obj->getScaleY());
        if (sx < 0.05f) sx = 1.0f;
        if (sy < 0.05f) sy = 1.0f;

        float w = 30.0f * sx;
        float h = 30.0f * sy;

        if (kind == CachedObjKind::Hazard) {
            // Standard GD spike internal hitbox is much smaller than its 30x30 sprite
            w = 7.0f * sx;
            h = 13.0f * sy;
        } else if (kind == CachedObjKind::OrbOrRing) {
            w = 34.0f * sx;
            h = 34.0f * sy;
        } else if (kind == CachedObjKind::Pad) {
            w = 26.0f * sx;
            h = 10.0f * sy;
        } else if (kind == CachedObjKind::GravityPortal) {
            w = 28.0f * sx;
            h = 56.0f * sy;
        }
        return CCRect(pos.x - w * 0.5f, pos.y - h * 0.5f, w, h);
    }

    void insertObject(GameObject* obj) {
        if (!obj) return;
        int typeInt = static_cast<int>(obj->m_objectType);
        CachedObjKind kind;

        switch (typeInt) {
            case GD_OBJ_HAZARD:
            case GD_OBJ_ANIMATED_HAZARD:
                kind = CachedObjKind::Hazard;
                break;
            case GD_OBJ_SOLID:
            case GD_OBJ_BREAKABLE:
                kind = CachedObjKind::Solid;
                break;
            case GD_OBJ_SLOPE:
                kind = CachedObjKind::Slope;
                break;
            case GD_OBJ_YELLOW_ORB:
            case GD_OBJ_PINK_ORB:
            case GD_OBJ_RED_ORB:
            case GD_OBJ_BLUE_GRAV_ORB:
            case GD_OBJ_GREEN_ORB:
            case GD_OBJ_BLACK_DROP_ORB:
            case GD_OBJ_DASH_ORB:
            case GD_OBJ_GRAVITY_DASH_ORB:
            case GD_OBJ_SPIDER_ORB:
            case GD_OBJ_TELEPORT_ORB:
            case GD_OBJ_CUSTOM_ORB:
                kind = CachedObjKind::OrbOrRing;
                break;
            case GD_OBJ_YELLOW_PAD:
            case GD_OBJ_PINK_PAD:
            case GD_OBJ_RED_PAD:
            case GD_OBJ_GRAVITY_PAD:
            case GD_OBJ_SPIDER_PAD:
                kind = CachedObjKind::Pad;
                break;
            case GD_OBJ_INVERSE_GRAV_PORTAL:
            case GD_OBJ_NORMAL_GRAV_PORTAL:
                kind = CachedObjKind::GravityPortal;
                break;
            default:
                return;
        }

        CachedObject entry;
        entry.raw = obj;
        entry.kind = kind;
        entry.objectID = obj->m_objectID;
        entry.rawType = typeInt;
        entry.hitbox = computeApproxHitbox(obj, kind);

        int minB = static_cast<int>(std::floor(entry.hitbox.getMinX() / BUCKET_SIZE));
        int maxB = static_cast<int>(std::floor(entry.hitbox.getMaxX() / BUCKET_SIZE));
        for (int b = minB; b <= maxB; ++b) {
            buckets[b].push_back(entry);
        }
        ++totalIndexed;
    }
};

static LevelSpatialIndex g_spatialIndex;

// ============================================================================
// 240 TPS Forward Physics & Click-Check Solver
// ============================================================================
struct SimState {
    double x = 0.0;
    double y = 0.0;
    double yVel = 0.0;
    double gravity = 0.958199;
    float playerSpeed = 0.9f;
    float vehicleSize = 1.0f;
    bool onGround = false;
    bool upsideDown = false;
    bool isShip = false;
    bool isBird = false;
    bool isBall = false;
    bool isDart = false;
    bool isRobot = false;
    bool isSpider = false;
    bool isSwing = false;
    bool isDashing = false;
    bool goingLeft = false;
    bool holding = false;
    bool dead = false;
    bool actionRegistered = false;
    double minHazardClearance = 9999.0;

    static SimState fromPlayer(PlayerObject* p, bool currentlyHolding) {
        SimState s;
        CCPoint pos = p->getRealPosition();
        s.x = pos.x;
        s.y = pos.y;
        s.yVel = p->m_yVelocity;
        s.gravity = (p->m_gravity > 0.01) ? p->m_gravity : 0.958199;
        s.playerSpeed = (p->m_playerSpeed > 0.1f) ? p->m_playerSpeed : 0.9f;
        s.vehicleSize = (p->m_vehicleSize > 0.1f) ? p->m_vehicleSize : 1.0f;
        s.onGround = p->m_isOnGround;
        s.upsideDown = p->m_isUpsideDown;
        s.isShip = p->m_isShip;
        s.isBird = p->m_isBird;
        s.isBall = p->m_isBall;
        s.isDart = p->m_isDart;
        s.isRobot = p->m_isRobot;
        s.isSpider = p->m_isSpider;
        s.isSwing = p->m_isSwing;
        s.isDashing = p->m_isDashing;
        s.goingLeft = p->m_isGoingLeft;
        s.holding = currentlyHolding;
        return s;
    }
};

struct LookaheadResult {
    bool survived = false;
    bool actionTriggered = false;
    int survivedTicks = 0;
    double minClearance = 0.0;
};

static double getHorizontalSpeedUnitsPerSec(float speedMult) {
    // Standard Geometry Dash horizontal speed constants (units/second):
    // 0.7x = 251.16, 0.9x (1x) = 311.58, 1.1x (2x) = 387.42, 1.3x (3x) = 468.0, 1.6x (4x) = 576.0
    if (speedMult < 0.8f) return 251.16;
    if (speedMult < 1.0f) return 311.58;
    if (speedMult < 1.2f) return 387.42;
    if (speedMult < 1.45f) return 468.00;
    return 576.00;
}

static bool tryTriggerOrbAtState(SimState& st, ModConfig const& cfg) {
    if (!cfg.correctOrbs) return false;
    int bucket = static_cast<int>(std::floor(st.x / LevelSpatialIndex::BUCKET_SIZE));
    float pHalf = 14.0f * st.vehicleSize;
    CCRect pRect(
        static_cast<float>(st.x - pHalf),
        static_cast<float>(st.y - pHalf),
        pHalf * 2.0f,
        pHalf * 2.0f
    );

    double gravSign = st.upsideDown ? -1.0 : 1.0;

    for (int b = bucket - 1; b <= bucket + 1; ++b) {
        auto it = g_spatialIndex.buckets.find(b);
        if (it == g_spatialIndex.buckets.end()) continue;
        for (auto const& obj : it->second) {
            if (obj.kind != CachedObjKind::OrbOrRing) continue;
            if (!pRect.intersectsRect(obj.hitbox)) continue;

            switch (obj.rawType) {
                case GD_OBJ_YELLOW_ORB:
                case GD_OBJ_CUSTOM_ORB:
                    st.yVel = 11.18 * gravSign;
                    st.onGround = false;
                    return true;
                case GD_OBJ_PINK_ORB:
                    st.yVel = 8.05 * gravSign;
                    st.onGround = false;
                    return true;
                case GD_OBJ_RED_ORB:
                    st.yVel = 15.10 * gravSign;
                    st.onGround = false;
                    return true;
                case GD_OBJ_BLUE_GRAV_ORB:
                    st.upsideDown = !st.upsideDown;
                    st.yVel = -3.8 * gravSign;
                    st.onGround = false;
                    return true;
                case GD_OBJ_GREEN_ORB:
                    st.upsideDown = !st.upsideDown;
                    st.yVel = -11.18 * gravSign;
                    st.onGround = false;
                    return true;
                case GD_OBJ_BLACK_DROP_ORB:
                    st.yVel = -14.5 * gravSign;
                    st.onGround = false;
                    return true;
                case GD_OBJ_DASH_ORB:
                    st.isDashing = true;
                    st.yVel = 0.0;
                    st.onGround = false;
                    return true;
                case GD_OBJ_GRAVITY_DASH_ORB:
                    st.upsideDown = !st.upsideDown;
                    st.isDashing = true;
                    st.yVel = 0.0;
                    st.onGround = false;
                    return true;
                case GD_OBJ_SPIDER_ORB:
                case GD_OBJ_TELEPORT_ORB:
                    st.upsideDown = !st.upsideDown;
                    st.yVel = 0.0;
                    st.onGround = true;
                    return true;
                default:
                    break;
            }
        }
    }
    return false;
}

static bool applyButtonActionAtTick(SimState& st, bool isPress, ModConfig const& cfg) {
    double gravSign = st.upsideDown ? -1.0 : 1.0;
    double sizeScale = (st.vehicleSize < 0.85f) ? 0.8f : 1.0;

    if (isPress) {
        st.holding = true;

        // 1. Check Orb / Ring activation first (applies to all modes)
        if (tryTriggerOrbAtState(st, cfg)) {
            st.actionRegistered = true;
            return true;
        }

        // 2. Air / Flying modes (Ship, UFO/Bird, Wave/Dart, Swing)
        if (st.isShip || st.isBird || st.isDart || st.isSwing) {
            if (!cfg.correctAirModes) return false;
            if (st.isBird) {
                st.yVel = 7.45 * gravSign * sizeScale;
            } else if (st.isSwing) {
                st.upsideDown = !st.upsideDown;
            }
            st.onGround = false;
            st.actionRegistered = true;
            return true;
        }

        // 3. Ground-based modes (Cube, Robot, Ball, Spider)
        if (!cfg.correctGroundJumps) return false;
        if (st.onGround) {
            if (st.isBall) {
                st.upsideDown = !st.upsideDown;
                st.yVel = -2.5 * gravSign;
                st.onGround = false;
            } else if (st.isSpider) {
                st.upsideDown = !st.upsideDown;
                st.yVel = 0.0;
                st.y += 90.0 * gravSign;
                st.onGround = true;
            } else {
                // Cube / Robot jump impulse at 240 TPS
                st.yVel = 11.18 * gravSign * sizeScale;
                st.onGround = false;
            }
            st.actionRegistered = true;
            return true;
        }

        return false;
    } else {
        // Button Release
        st.holding = false;
        if (st.isDashing) {
            st.isDashing = false;
            st.actionRegistered = true;
            return true;
        }
        if (st.isShip || st.isDart || st.isRobot) {
            st.actionRegistered = true;
            return true;
        }
        return false;
    }
}

static void stepPhysicsSingleTick(SimState& st, double dt) {
    double dirX = st.goingLeft ? -1.0 : 1.0;
    double xSpeed = getHorizontalSpeedUnitsPerSec(st.playerSpeed);
    st.x += dirX * xSpeed * dt;

    double gravSign = st.upsideDown ? -1.0 : 1.0;
    double stepScale = dt * 60.0; // Normalized to 60Hz base physics step (0.25 at 240 TPS)

    if (st.isDashing) {
        st.yVel = 0.0;
    } else if (st.isDart) {
        // Wave moves at constant diagonal slope (+/- xSpeed)
        double waveSlope = (st.vehicleSize < 0.85f) ? 2.0 : 1.0;
        double waveDir = (st.holding ? 1.0 : -1.0) * gravSign;
        st.y += xSpeed * dt * waveSlope * waveDir;
        st.yVel = xSpeed * waveSlope * waveDir / 60.0;
    } else if (st.isShip) {
        double accel = st.holding ? (0.42 * gravSign) : (-0.34 * gravSign);
        st.yVel += accel * stepScale;
        st.yVel = std::clamp(st.yVel, -12.0, 12.0);
        st.y += st.yVel * stepScale * 2.7;
    } else {
        if (!st.onGround) {
            st.yVel -= (st.gravity * 0.92 * gravSign) * stepScale;
            st.yVel = std::clamp(st.yVel, -15.5, 15.5);
            st.y += st.yVel * stepScale * 2.7;
        }
    }

    // Ground plane floor check (Y = 105.0 is standard GD ground top)
    if (!st.upsideDown && st.y <= 105.0) {
        st.y = 105.0;
        if (st.yVel <= 0.0) {
            st.yVel = 0.0;
            st.onGround = true;
        }
    }

    // Spatial collision & pad/portal check in nearby buckets
    int bucket = static_cast<int>(std::floor(st.x / LevelSpatialIndex::BUCKET_SIZE));
    float pHalf = 11.5f * st.vehicleSize;
    CCRect pRect(
        static_cast<float>(st.x - pHalf),
        static_cast<float>(st.y - pHalf),
        pHalf * 2.0f,
        pHalf * 2.0f
    );

    bool supportedBySolid = (!st.upsideDown && st.y <= 105.5);

    for (int b = bucket - 1; b <= bucket + 1; ++b) {
        auto it = g_spatialIndex.buckets.find(b);
        if (it == g_spatialIndex.buckets.end()) continue;

        for (auto const& obj : it->second) {
            if (obj.kind == CachedObjKind::Hazard) {
                double dx = std::max({
                    0.0,
                    static_cast<double>(obj.hitbox.getMinX() - pRect.getMaxX()),
                    static_cast<double>(pRect.getMinX() - obj.hitbox.getMaxX())
                });
                double dy = std::max({
                    0.0,
                    static_cast<double>(obj.hitbox.getMinY() - pRect.getMaxY()),
                    static_cast<double>(pRect.getMinY() - obj.hitbox.getMaxY())
                });
                double dist = std::hypot(dx, dy);
                st.minHazardClearance = std::min(st.minHazardClearance, dist);

                if (pRect.intersectsRect(obj.hitbox)) {
                    st.dead = true;
                    return;
                }
            } else if (obj.kind == CachedObjKind::Solid || obj.kind == CachedObjKind::Slope) {
                if (!pRect.intersectsRect(obj.hitbox)) continue;

                double blockTop = obj.hitbox.getMaxY();
                double blockBot = obj.hitbox.getMinY();

                if (!st.upsideDown) {
                    if (st.y - pHalf >= blockTop - 9.5 && st.yVel <= 0.5) {
                        st.y = blockTop + pHalf;
                        st.yVel = 0.0;
                        st.onGround = true;
                        supportedBySolid = true;
                    } else if (st.isDart || (st.x + pHalf > obj.hitbox.getMinX() + 3.0 && st.y < blockTop - 4.0 && st.y > blockBot + 4.0)) {
                        st.dead = true;
                        return;
                    }
                } else {
                    if (st.y + pHalf <= blockBot + 9.5 && st.yVel >= -0.5) {
                        st.y = blockBot - pHalf;
                        st.yVel = 0.0;
                        st.onGround = true;
                        supportedBySolid = true;
                    } else if (st.isDart || (st.x + pHalf > obj.hitbox.getMinX() + 3.0 && st.y < blockTop - 4.0 && st.y > blockBot + 4.0)) {
                        st.dead = true;
                        return;
                    }
                }
            } else if (obj.kind == CachedObjKind::Pad) {
                if (pRect.intersectsRect(obj.hitbox)) {
                    if (obj.rawType == GD_OBJ_GRAVITY_PAD) {
                        st.upsideDown = !st.upsideDown;
                        st.yVel = -4.0 * gravSign;
                        st.onGround = false;
                    } else {
                        st.yVel = 13.5 * gravSign;
                        st.onGround = false;
                    }
                }
            } else if (obj.kind == CachedObjKind::GravityPortal) {
                if (pRect.intersectsRect(obj.hitbox)) {
                    st.upsideDown = (obj.rawType == GD_OBJ_INVERSE_GRAV_PORTAL);
                }
            }
        }
    }

    if (!supportedBySolid) {
        st.onGround = false;
    }
}

static LookaheadResult simulateCandidateTick(
    SimState initial,
    int delayTicks,
    bool isPress,
    ModConfig const& cfg
) {
    LookaheadResult res;
    SimState st = initial;
    st.minHazardClearance = 9999.0;

    double dt = 1.0 / static_cast<double>(std::max(60, cfg.baseTPS));

    // Phase 1: Step `delayTicks` before applying the click/release
    for (int t = 0; t < delayTicks; ++t) {
        stepPhysicsSingleTick(st, dt);
        if (st.dead) {
            res.survived = false;
            res.survivedTicks = t;
            res.minClearance = st.minHazardClearance;
            return res;
        }
    }

    // Phase 2: Apply the click or release on tick `delayTicks`
    res.actionTriggered = applyButtonActionAtTick(st, isPress, cfg);
    if (isPress && !res.actionTriggered) {
        res.survived = false;
        res.survivedTicks = delayTicks;
        res.minClearance = st.minHazardClearance;
        return res;
    }

    // Phase 3: Simulate forward for `cfg.lookaheadTicks` (e.g. 480 ticks on 240 TPS)
    for (int t = 1; t <= cfg.lookaheadTicks; ++t) {
        stepPhysicsSingleTick(st, dt);
        if (st.dead) {
            res.survived = false;
            res.survivedTicks = delayTicks + t;
            res.minClearance = st.minHazardClearance;
            return res;
        }
    }

    res.survived = true;
    res.survivedTicks = delayTicks + cfg.lookaheadTicks;
    res.minClearance = st.minHazardClearance;
    return res;
}

} // namespace

// ============================================================================
// Custom In-Game UI Popup ("Tick Assist Config") for Mobile & Desktop
// ============================================================================
class TickAssistPopup : public geode::Popup {
protected:
    CCLabelBMFont* m_earlyWindowLabel = nullptr;
    CCLabelBMFont* m_lookaheadLabel = nullptr;
    CCLabelBMFont* m_tpsLabel = nullptr;

    bool init() {
        if (!Popup::init(380.f, 270.f)) return false;

        this->setTitle("Tick Assist (240 TPS Auto-Correct)");

        auto winSize = m_mainLayer->getContentSize();

        auto subLabel = CCLabelBMFont::create(
            "Auto-corrects 1-16t early clicks using 480t lookahead",
            "chatFont.fnt"
        );
        subLabel->setScale(0.62f);
        subLabel->setColor({180, 230, 255});
        subLabel->setPosition({winSize.width * 0.5f, winSize.height - 38.f});
        m_mainLayer->addChild(subLabel);

        auto controlMenu = CCMenu::create();
        controlMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(controlMenu);

        // 1. Early Click Window (1 - 16 ticks)
        m_earlyWindowLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_earlyWindowLabel->setScale(0.55f);
        m_earlyWindowLabel->setPosition({winSize.width * 0.5f, winSize.height - 68.f});
        m_mainLayer->addChild(m_earlyWindowLabel);

        addStepperRow(
            controlMenu,
            winSize.height - 68.f,
            menu_selector(TickAssistPopup::onEarlyMinus),
            menu_selector(TickAssistPopup::onEarlyPlus)
        );

        // 2. Lookahead Horizon (60 - 960 ticks, default 480)
        m_lookaheadLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_lookaheadLabel->setScale(0.52f);
        m_lookaheadLabel->setPosition({winSize.width * 0.5f, winSize.height - 98.f});
        m_mainLayer->addChild(m_lookaheadLabel);

        addStepperRow(
            controlMenu,
            winSize.height - 98.f,
            menu_selector(TickAssistPopup::onLookaheadMinus),
            menu_selector(TickAssistPopup::onLookaheadPlus)
        );

        // 3. Base Simulation TPS (60 - 480, default 240)
        m_tpsLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_tpsLabel->setScale(0.52f);
        m_tpsLabel->setPosition({winSize.width * 0.5f, winSize.height - 126.f});
        m_mainLayer->addChild(m_tpsLabel);

        addStepperRow(
            controlMenu,
            winSize.height - 126.f,
            menu_selector(TickAssistPopup::onTpsMinus),
            menu_selector(TickAssistPopup::onTpsPlus)
        );

        // Feature Toggles Grid (2 Columns for Mobile Touch Accessibility)
        float colLeftX = 42.f;
        float colRightX = 205.f;
        float row1Y = winSize.height - 160.f;
        float row2Y = winSize.height - 186.f;
        float row3Y = winSize.height - 212.f;

        addSettingToggle(controlMenu, "Mod Enabled", "enabled", {colLeftX, row1Y});
        addSettingToggle(controlMenu, "Ground Jumps", "correct-ground-jumps", {colRightX, row1Y});
        addSettingToggle(controlMenu, "Orbs & Rings", "correct-orbs", {colLeftX, row2Y});
        addSettingToggle(controlMenu, "Ship/Wave/UFO", "correct-air-modes", {colRightX, row2Y});
        addSettingToggle(controlMenu, "Early Releases", "correct-releases", {colLeftX, row3Y});
        addSettingToggle(controlMenu, "Corner Rescue", "corner-rescue", {colRightX, row3Y});

        // Bottom Button to open full Geode Mod Settings Dialog
        auto fullSettingsSpr = ButtonSprite::create(
            "All Settings & Sliders", "goldFont.fnt", "GJ_button_04.png", 0.7f
        );
        fullSettingsSpr->setScale(0.75f);
        auto fullSettingsBtn = CCMenuItemSpriteExtra::create(
            fullSettingsSpr, this, menu_selector(TickAssistPopup::onOpenAllSettings)
        );
        fullSettingsBtn->setPosition({winSize.width * 0.5f, 25.f});
        controlMenu->addChild(fullSettingsBtn);

        refreshLabels();
        return true;
    }

    void addStepperRow(CCMenu* menu, float yPos, SEL_MenuHandler onMinus, SEL_MenuHandler onPlus) {
        auto winSize = m_mainLayer->getContentSize();
        auto leftSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        leftSpr->setScale(0.62f);
        auto leftBtn = CCMenuItemSpriteExtra::create(leftSpr, this, onMinus);
        leftBtn->setPosition({winSize.width * 0.5f - 125.f, yPos});
        menu->addChild(leftBtn);

        auto rightSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        rightSpr->setFlipX(true);
        rightSpr->setScale(0.62f);
        auto rightBtn = CCMenuItemSpriteExtra::create(rightSpr, this, onPlus);
        rightBtn->setPosition({winSize.width * 0.5f + 125.f, yPos});
        menu->addChild(rightBtn);
    }

    void addSettingToggle(CCMenu* menu, char const* labelText, std::string const& key, CCPoint pos) {
        bool current = Mod::get()->getSettingValue<bool>(key);
        auto offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        offSpr->setScale(0.62f);
        onSpr->setScale(0.62f);

        auto toggler = CCMenuItemToggler::create(
            offSpr, onSpr, this, menu_selector(TickAssistPopup::onToggleSetting)
        );
        toggler->toggle(current);
        toggler->setPosition(pos);
        toggler->setID(key);
        menu->addChild(toggler);

        auto lbl = CCLabelBMFont::create(labelText, "bigFont.fnt");
        lbl->setScale(0.34f);
        lbl->setAnchorPoint({0.f, 0.5f});
        lbl->setPosition({pos.x + 18.f, pos.y});
        m_mainLayer->addChild(lbl);
    }

    void refreshLabels() {
        auto cfg = ModConfig::load();
        m_earlyWindowLabel->setString(
            fmt::format("Early Window: {} ticks (1-16)", cfg.maxEarlyTicks).c_str()
        );
        double lookaheadSec = static_cast<double>(cfg.lookaheadTicks) / std::max(1, cfg.baseTPS);
        m_lookaheadLabel->setString(
            fmt::format("Lookahead: {} ticks ({:.2f}s)", cfg.lookaheadTicks, lookaheadSec).c_str()
        );
        m_tpsLabel->setString(
            fmt::format("Simulation Rate: {} TPS", cfg.baseTPS).c_str()
        );
    }

    void onEarlyMinus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("max-early-ticks");
        Mod::get()->setSettingValue<int64_t>("max-early-ticks", std::clamp<int64_t>(val - 1, 1, 16));
        refreshLabels();
    }

    void onEarlyPlus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("max-early-ticks");
        Mod::get()->setSettingValue<int64_t>("max-early-ticks", std::clamp<int64_t>(val + 1, 1, 16));
        refreshLabels();
    }

    void onLookaheadMinus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("lookahead-ticks");
        Mod::get()->setSettingValue<int64_t>("lookahead-ticks", std::clamp<int64_t>(val - 30, 60, 960));
        refreshLabels();
    }

    void onLookaheadPlus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("lookahead-ticks");
        Mod::get()->setSettingValue<int64_t>("lookahead-ticks", std::clamp<int64_t>(val + 30, 60, 960));
        refreshLabels();
    }

    void onTpsMinus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("base-tps");
        Mod::get()->setSettingValue<int64_t>("base-tps", std::clamp<int64_t>(val - 60, 60, 480));
        refreshLabels();
    }

    void onTpsPlus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("base-tps");
        Mod::get()->setSettingValue<int64_t>("base-tps", std::clamp<int64_t>(val + 60, 60, 480));
        refreshLabels();
    }

    void onToggleSetting(CCObject* sender) {
        auto* toggler = static_cast<CCMenuItemToggler*>(sender);
        std::string key = toggler->getID();
        if (!key.empty()) {
            bool current = Mod::get()->getSettingValue<bool>(key);
            Mod::get()->setSettingValue<bool>(key, !current);
        }
    }

    void onOpenAllSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }

public:
    static TickAssistPopup* create() {
        auto ret = new TickAssistPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ============================================================================
// PlayerObject Hook: Intercepts Early Clicks & Releases + Executes Delayed Ticks
// ============================================================================
class $modify(TickAssistPlayerObject, PlayerObject) {
public:
    struct Fields {
        bool m_bypassHook = false;
        bool m_hasQueuedPress = false;
        bool m_hasQueuedRelease = false;
        int m_queuedPressTicks = 0;
        int m_queuedReleaseTicks = 0;
        int m_lastAppliedDelayTicks = 0;
        double m_tickAccumulator = 0.0;
        PlayerButton m_queuedButton = PlayerButton::Jump;
        int m_totalCorrections = 0;
        int m_lastDelayAmount = 0;
        int m_lastSafeHorizon = 0;
    };

    bool shouldAffectThisPlayer(ModConfig const& cfg) {
        auto* pl = PlayLayer::get();
        if (!pl) return false;
        if (this == pl->m_player1) return true;
        if (this == pl->m_player2 && cfg.correctPlayer2) return true;
        return false;
    }

    bool pushButton(PlayerButton button) {
        if (m_fields->m_bypassHook || button != PlayerButton::Jump) {
            return PlayerObject::pushButton(button);
        }

        auto cfg = ModConfig::load();
        if (!cfg.enabled || !shouldAffectThisPlayer(cfg)) {
            return PlayerObject::pushButton(button);
        }

        // Capture current player state and test Tick 0 (immediate click)
        SimState current = SimState::fromPlayer(this, false);
        LookaheadResult tick0 = simulateCandidateTick(current, 0, true, cfg);

        if (tick0.actionTriggered && tick0.survived) {
            // Immediate click is already valid and survives the 480-tick lookahead!
            m_fields->m_lastAppliedDelayTicks = 0;
            return PlayerObject::pushButton(button);
        }

        // Search candidate delay ticks 1 .. maxEarlyTicks (1 to 16 ticks)
        int bestDelay = -1;
        int bestSurvivedTicks = tick0.survivedTicks;
        double bestClearance = tick0.minClearance;

        for (int delay = 1; delay <= cfg.maxEarlyTicks; ++delay) {
            LookaheadResult cand = simulateCandidateTick(current, delay, true, cfg);
            if (!cand.actionTriggered) continue;

            if (cand.survived) {
                bestDelay = delay;
                bestSurvivedTicks = cand.survivedTicks;
                bestClearance = cand.minClearance;
                break;
            }

            if (cand.survivedTicks > bestSurvivedTicks + 8 ||
                (cand.survivedTicks == bestSurvivedTicks && cand.minClearance > bestClearance)) {
                bestDelay = delay;
                bestSurvivedTicks = cand.survivedTicks;
                bestClearance = cand.minClearance;
            }
        }

        if (bestDelay >= 1 && bestDelay <= cfg.maxEarlyTicks) {
            m_fields->m_hasQueuedPress = true;
            m_fields->m_queuedPressTicks = bestDelay;
            m_fields->m_lastAppliedDelayTicks = bestDelay;
            m_fields->m_queuedButton = button;
            m_fields->m_totalCorrections++;
            m_fields->m_lastDelayAmount = bestDelay;
            m_fields->m_lastSafeHorizon = bestSurvivedTicks - bestDelay;
            return true;
        }

        m_fields->m_lastAppliedDelayTicks = 0;
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        if (m_fields->m_bypassHook || button != PlayerButton::Jump) {
            return PlayerObject::releaseButton(button);
        }

        auto cfg = ModConfig::load();
        if (!cfg.enabled || !shouldAffectThisPlayer(cfg)) {
            return PlayerObject::releaseButton(button);
        }

        // If a delayed press is still waiting to fire, queue the release after the press!
        if (m_fields->m_hasQueuedPress) {
            int extraHold = cfg.preserveHoldLength ? std::max(2, m_fields->m_lastAppliedDelayTicks) : 1;
            m_fields->m_hasQueuedRelease = true;
            m_fields->m_queuedReleaseTicks = m_fields->m_queuedPressTicks + extraHold;
            return true;
        }

        // Optional early-release auto-correction (1 - 16 ticks)
        if (cfg.correctReleases) {
            SimState current = SimState::fromPlayer(this, true);
            LookaheadResult rel0 = simulateCandidateTick(current, 0, false, cfg);

            if (!rel0.survived) {
                for (int delay = 1; delay <= cfg.maxEarlyTicks; ++delay) {
                    LookaheadResult cand = simulateCandidateTick(current, delay, false, cfg);
                    if (cand.survived) {
                        m_fields->m_hasQueuedRelease = true;
                        m_fields->m_queuedReleaseTicks = delay;
                        m_fields->m_totalCorrections++;
                        m_fields->m_lastDelayAmount = delay;
                        m_fields->m_lastSafeHorizon = cfg.lookaheadTicks;
                        return true;
                    }
                }
            }
        }

        return PlayerObject::releaseButton(button);
    }

    void executeQueuedPressNow() {
        if (!m_fields->m_hasQueuedPress) return;
        m_fields->m_hasQueuedPress = false;
        m_fields->m_queuedPressTicks = 0;
        m_fields->m_bypassHook = true;
        this->pushButton(m_fields->m_queuedButton);
        m_fields->m_bypassHook = false;
    }

    void executeQueuedReleaseNow() {
        if (!m_fields->m_hasQueuedRelease) return;
        m_fields->m_hasQueuedRelease = false;
        m_fields->m_queuedReleaseTicks = 0;
        m_fields->m_bypassHook = true;
        this->releaseButton(m_fields->m_queuedButton);
        m_fields->m_bypassHook = false;
    }

    void update(float dt) {
        PlayerObject::update(dt);

        if (!m_fields->m_hasQueuedPress && !m_fields->m_hasQueuedRelease) {
            return;
        }

        auto cfg = ModConfig::load();
        if (!cfg.enabled) {
            m_fields->m_hasQueuedPress = false;
            m_fields->m_hasQueuedRelease = false;
            return;
        }

        // Convert frame delta to 240 TPS ticks (in GD, dt is scaled by 60, so 1 sec = 60.0 units of dt)
        double realSeconds = static_cast<double>(dt) / 60.0;
        double ticksElapsed = std::max(1.0, realSeconds * static_cast<double>(cfg.baseTPS));
        m_fields->m_tickAccumulator += ticksElapsed;

        int wholeTicks = static_cast<int>(m_fields->m_tickAccumulator);
        if (wholeTicks <= 0) wholeTicks = 1;
        m_fields->m_tickAccumulator -= static_cast<double>(wholeTicks);

        if (m_fields->m_hasQueuedPress) {
            m_fields->m_queuedPressTicks -= wholeTicks;
            if (m_fields->m_queuedPressTicks <= 0 ||
                (this->m_isOnGround && m_fields->m_queuedPressTicks <= 2)) {
                executeQueuedPressNow();
            }
        }

        if (m_fields->m_hasQueuedRelease) {
            m_fields->m_queuedReleaseTicks -= wholeTicks;
            if (m_fields->m_queuedReleaseTicks <= 0) {
                executeQueuedReleaseNow();
            }
        }
    }
};

// ============================================================================
// PlayLayer Hook: Spatial Object Indexing, Corner Rescue & Mobile Status HUD
// ============================================================================
class $modify(TickAssistPlayLayer, PlayLayer) {
public:
    struct Fields {
        CCLabelBMFont* m_hudLabel = nullptr;
        CCMenuItemSpriteExtra* m_mobileUiBtn = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_spatialIndex.clear();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Create live status HUD label
        auto hud = CCLabelBMFont::create("Tick Assist: Ready (240 TPS)", "chatFont.fnt");
        hud->setScale(0.5f);
        hud->setAnchorPoint({0.f, 1.f});
        hud->setPosition({12.f, winSize.height - 26.f});
        hud->setOpacity(195);
        hud->setZOrder(999);
        this->addChild(hud);
        m_fields->m_hudLabel = hud;

        // Create optional in-game mobile floating button ("TA") at top-right near pause button
        auto uiMenu = CCMenu::create();
        uiMenu->setPosition({0.f, 0.f});
        uiMenu->setZOrder(1000);
        this->addChild(uiMenu);

        auto btnSpr = ButtonSprite::create("TA", "goldFont.fnt", "GJ_button_01.png", 0.6f);
        btnSpr->setScale(0.55f);
        btnSpr->setOpacity(185);
        auto btn = CCMenuItemSpriteExtra::create(
            btnSpr, this, menu_selector(TickAssistPlayLayer::onOpenTickAssistUI)
        );
        btn->setPosition({winSize.width - 52.f, winSize.height - 22.f});
        uiMenu->addChild(btn);
        m_fields->m_mobileUiBtn = btn;

        updateHudVisibility();
        return true;
    }

    void onOpenTickAssistUI(CCObject*) {
        this->pauseGame(false);
        TickAssistPopup::create()->show();
    }

    void updateHudVisibility() {
        auto cfg = ModConfig::load();
        if (m_fields->m_hudLabel) {
            m_fields->m_hudLabel->setVisible(cfg.enabled && cfg.showHud);
        }
        if (m_fields->m_mobileUiBtn) {
            m_fields->m_mobileUiBtn->setVisible(cfg.showMobileButton);
        }
    }

    void addObject(GameObject* object) {
        PlayLayer::addObject(object);
        g_spatialIndex.insertObject(object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (this->m_player1) {
            auto* p1 = static_cast<TickAssistPlayerObject*>(this->m_player1);
            p1->m_fields->m_hasQueuedPress = false;
            p1->m_fields->m_hasQueuedRelease = false;
            p1->m_fields->m_queuedPressTicks = 0;
            p1->m_fields->m_queuedReleaseTicks = 0;
        }
        if (this->m_player2) {
            auto* p2 = static_cast<TickAssistPlayerObject*>(this->m_player2);
            p2->m_fields->m_hasQueuedPress = false;
            p2->m_fields->m_hasQueuedRelease = false;
            p2->m_fields->m_queuedPressTicks = 0;
            p2->m_fields->m_queuedReleaseTicks = 0;
        }
        updateHudVisibility();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto cfg = ModConfig::load();
        updateHudVisibility();

        if (m_fields->m_hudLabel && cfg.enabled && cfg.showHud && this->m_player1) {
            auto* p1 = static_cast<TickAssistPlayerObject*>(this->m_player1);
            if (p1->m_fields->m_totalCorrections > 0) {
                m_fields->m_hudLabel->setString(fmt::format(
                    "TA [1-{}t | {}t@{}{}]: +{}t (Safe {}t) | Saved: {}",
                    cfg.maxEarlyTicks,
                    cfg.lookaheadTicks,
                    cfg.baseTPS,
                    p1->m_fields->m_hasQueuedPress ? " Q" : "",
                    p1->m_fields->m_lastDelayAmount,
                    p1->m_fields->m_lastSafeHorizon,
                    p1->m_fields->m_totalCorrections
                ).c_str());
            } else {
                m_fields->m_hudLabel->setString(fmt::format(
                    "TA Active: 1-{}t Early | {}t Lookahead @ {} TPS",
                    cfg.maxEarlyTicks,
                    cfg.lookaheadTicks,
                    cfg.baseTPS
                ).c_str());
            }
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto cfg = ModConfig::load();
        if (cfg.enabled && cfg.cornerRescue && player && object) {
            int typeInt = static_cast<int>(object->m_objectType);
            bool isLevelObstacle = (
                typeInt == GD_OBJ_HAZARD ||
                typeInt == GD_OBJ_ANIMATED_HAZARD ||
                typeInt == GD_OBJ_SOLID ||
                typeInt == GD_OBJ_SLOPE
            );
            auto* tap = static_cast<TickAssistPlayerObject*>(player);
            if (isLevelObstacle && tap->m_fields->m_hasQueuedPress &&
                tap->m_fields->m_queuedPressTicks <= cfg.maxEarlyTicks) {
                // Execute the queued passing click on the exact boundary tick and rescue the player
                tap->executeQueuedPressNow();
                return;
            }
        }

        PlayLayer::destroyPlayer(player, object);
    }
};

// ============================================================================
// PauseLayer Hook: Adds a Button in the Pause Menu to Open the Tick Assist UI
// ============================================================================
class $modify(TickAssistPauseLayer, PauseLayer) {
public:
    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto btnSpr = ButtonSprite::create("Tick Assist", "goldFont.fnt", "GJ_button_01.png", 0.65f);
        btnSpr->setScale(0.65f);

        auto btn = CCMenuItemSpriteExtra::create(
            btnSpr, this, menu_selector(TickAssistPauseLayer::onTickAssistPopup)
        );

        if (auto* rightMenu = this->getChildByID("right-button-menu")) {
            rightMenu->addChild(btn);
            rightMenu->updateLayout();
        } else {
            auto fallbackMenu = CCMenu::create();
            fallbackMenu->setPosition({winSize.width - 68.f, 42.f});
            fallbackMenu->addChild(btn);
            this->addChild(fallbackMenu, 100);
        }
    }

    void onTickAssistPopup(CCObject*) {
        TickAssistPopup::create()->show();
    }
};