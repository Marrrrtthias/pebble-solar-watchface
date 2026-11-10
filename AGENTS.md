# AGENTS.md

Notes for agents working on this repo. **When a new decision is made, add it here** (one line, with the reason if it isn't obvious).

## Project
- Recreation of the Apple Watch "Solar" face for Pebble Time 2 (`emery`, 200×228, 64 colors). Emery only; round (gabbro) support would be a separate pass.
- Native C (`src/c/main.c`). Phone JS (`src/pkjs/index.js`) only sends the location.
- Everything in the repo is in English.

## Design decisions
- The curve plots sin(elevation) instead of elevation: it looks the same and avoids `asin`. All math is fixed-point (`sin_lookup`/`cos_lookup`), with no floats.
- The glows are drawn per pixel into the framebuffer using 4×4 Bayer dithering over small palette ramps (day blue; twilight warm/cool). The day glow fades out under the horizon while the twilight glow is active.
- The face redraws on `MINUTE_UNIT` ticks only, with no animation timers (battery).
- Location: phone GPS → persisted on the watch, re-requested hourly. Fallback 50°N, 8°E.
- Time font is the system `BITHAM_42_LIGHT` (no bundled font for now).
- 12-hour mode shows no AM/PM, matching the original; the sun's position tells the two apart.
- Accepted: around midnight the sun is half cut off at the left/right screen edge. Do not fix unless asked.
- Watchfaces get no button events, so interactive mode is touch-only: tap toggles it, swipe left/right steps to the next/previous marker (chronological, clamped at the ends), 30 s without input exits. Raw `touch_service` (not recognizers); the touch sensor stays subscribed, a deliberate battery tradeoff.
- Interactive markers: sunrise, solar noon, now, sunset. Displayed sunrise/sunset times use the standard -0.833 deg threshold (matches published times); their markers sit on the horizon line. Solar noon is computed analytically (hour angle 0). Polar day/night drops sunrise/sunset.
- Interactive mode reuses the normal render path: the sun and glow are drawn at the selected marker's time. No animation when switching markers (battery).

## Debug / store assets
- `DEBUG_TOUCH_VIA_TAP` maps `pebble emu-tap` to touch gestures (the emulator cannot inject touch): `--direction z+` = tap, `x+` = swipe left (next), `x-` = swipe right (previous). Cannot be combined with `DEMO_MODE`.
- `DEBUG_FAKE_MINUTE` freezes the clock. `DEMO_MODE` fixes Berlin on 6 Sep (CEST) and advances 20 min per `pebble emu-tap`; it was used for the store GIF. **All debug flags (`DEBUG_FAKE_MINUTE`, `DEMO_MODE`, `DEBUG_TOUCH_VIA_TAP`) must be off (`-1` / `0`) in commits and releases.**
- Store assets live in `store/`: `make_icon.py` (icons 144/48) and `make_gif.py` (frames → GIF). They are plain Python with no dependencies (Pillow isn't installed). README screenshots are in `screenshots/`.
- Pebble SDK commands need to run outside the sandbox (they write to `~/Library/Application Support/Pebble SDK`).

## Git / releases
- Commit with the GitHub noreply email (already set in the local git config). GitHub rejects pushes that use the private address.
- Releases: tag `vX.Y.Z` on `main`, matching `version` in `package.json`. Attach a clean-built `solar-X.Y.Z.pbw`, since `build/` is gitignored.
