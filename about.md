# Tick Assist (Click Auto-Correct)

A mobile-friendly **240 TPS Click Lookahead & Auto-Correct** mod for Geometry Dash built with the Geode SDK.

## How It Works
- **Early Click Window (`1–16` Ticks):** When you tap or click `1` to `16` ticks early (at `240` ticks per second), Tick Assist simulates your player trajectory forward.
- **480-Tick Lookahead Check (`2.0s` at `240` TPS):** If clicking on the current tick fails to trigger a jump/orb or results in a collision death within the next `480` ticks, the solver tests candidate ticks `+1` through `+16` and queues your click for the exact tick that passes the obstacle check and survives the lookahead horizon.
- **In-Game Mobile UI Popup:** Tap the **TA** button in the Pause Menu (or the optional top-screen mobile HUD button) to adjust your `1–16` tick window, change lookahead frames/ticks (`60–960`), and toggle specific correction checks (Ground Jumps, Orbs/Rings, Ship/Wave/UFO/Swing corridors, Early Releases, Dual Player 2, and Corner Rescue) without leaving the level.
