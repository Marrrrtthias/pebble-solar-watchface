# AGENTS.md

Notes for agents working on this repo. **When a new decision is made, add it here** (one line, with the reason if it isn't obvious).

## Project
- Recreation of the Apple Watch "Solar" face for Pebble Time 2 (`emery`, 200×228, 64 colors). Emery only; round (gabbro) support would be a separate pass.
- Two Pebble projects in one repo, sharing one version number:
  - Watchface "Solar" at the repo root (`src/c/main.c`), non-interactive.
  - Watchapp "Solar Times" in `solar-times/` (`solar-times/src/c/main.c`), only the interactive marker view.
  - Shared sun model and rendering in `shared/c/solar.{c,h}`, compiled by both wscripts. The app's wscript also bundles the root `src/pkjs`.
- Native C. Phone JS (`src/pkjs/index.js`) only sends the location.
- Everything in the repo is in English.

## Design decisions
- The curve plots sin(elevation) instead of elevation: it looks the same and avoids `asin`. All math is fixed-point (`sin_lookup`/`cos_lookup`), with no floats.
- The glows are drawn per pixel into the framebuffer using 4×4 Bayer dithering over small palette ramps (day blue; twilight warm/cool). The day glow fades out under the horizon while the twilight glow is active.
- The face redraws on `MINUTE_UNIT` ticks only, with no animation timers (battery).
- Location: phone GPS → persisted on the watch, re-requested hourly. Fallback 50°N, 8°E. Each app persists its own copy (app storage is per UUID).
- Time font is the system `BITHAM_42_LIGHT` (no bundled font for now).
- 12-hour mode shows no AM/PM on the clock, matching the original; the sun's position tells the two apart. Marker times in Solar Times do show AM/PM, since they are away from now.
- Accepted: around midnight the sun is half cut off at the left/right screen edge. Do not fix unless asked.
- Interactive mode is a separate watchapp because watchfaces get neither button events nor touch input on real hardware (raw `touch_service` and gesture recognizers both delivered nothing), and a watchface can't launch another app. Users open Solar Times from the watchface via the system Quick Launch setting; Back or 30 s without input returns to the watchface.
- Solar Times controls: Up = earlier marker, Down = later (timeline convention), Select = back to now; markers are chronological and clamped at the ends.
- Markers: sunrise, solar noon, now, sunset. Displayed sunrise/sunset times use the standard -0.833 deg threshold (matches published times); their markers sit on the horizon line. Solar noon is computed analytically (hour angle 0). Polar day/night drops sunrise/sunset.
- Solar Times reuses the watchface render path (`solar_draw`): the sun and glow are drawn at the selected marker's time. No animation when switching markers (battery).

## Debug / store assets
- `DEBUG_FAKE_MINUTE` freezes the clock. `DEMO_MODE` fixes Berlin on 6 Sep (CEST) and advances 20 min per `pebble emu-tap`; it was used for the store GIF. Solar Times has its own `DEBUG_FAKE_MINUTE`. **All debug flags must be off (`-1` / `0`) in commits and releases.**
- Test Solar Times in the emulator with `pebble emu-button click up|down|select|back`. Reinstalling an unchanged build doesn't relaunch it; open it from the launcher.
- Store assets live in `store/`: `make_icon.py` (icons 144/48) and `make_gif.py` (frames → GIF). They are plain Python with no dependencies (Pillow isn't installed). README screenshots are in `screenshots/`.
- Pebble SDK commands need to run outside the sandbox (they write to `~/Library/Application Support/Pebble SDK`).

## Git / releases
- Commit with the GitHub noreply email (already set in the local git config). GitHub rejects pushes that use the private address.
- Releases: tag `vX.Y.Z` on `main`, matching `version` in both `package.json` and `solar-times/package.json`. Attach clean-built `solar-X.Y.Z.pbw` and `solar-times-X.Y.Z.pbw`, since `build/` is gitignored.
