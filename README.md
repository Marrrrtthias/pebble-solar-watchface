# Solar

A Pebble Time 2 (emery) watchface inspired by the Apple Watch Solar face. It plots the sun's elevation over the day for your location and shows a glowing sun at the current time.

| Day | Sunset | Dusk | Night |
|:---:|:---:|:---:|:---:|
| ![Day](screenshots/t1200.png) | ![Sunset](screenshots/t1525.png) | ![Dusk](screenshots/t1630.png) | ![Night](screenshots/t2207.png) |

- The curve runs from midnight (left) to midnight (right). The grey line is the horizon.
- The glow changes with the sun's elevation: blue during the day, red and pink at the horizon, and a hollow ring when the sun is below the horizon.
- Your location comes from the phone and is cached on the watch. It falls back to 50°N, 8°E until the phone reports a location.
- The face redraws once per minute.

### Solar Times app

| Now | Sunrise | Noon | Sunset |
|:---:|:---:|:---:|:---:|
| ![Now](screenshots/interactive_now.png) | ![Sunrise](screenshots/interactive_sunrise.png) | ![Noon](screenshots/interactive_noon.png) | ![Sunset](screenshots/interactive_sunset.png) |

A companion watchapp that shows markers for sunrise, solar noon, now and sunset on the sun path. Watchfaces can't react to buttons or touch, so it is a separate app.

- Up and Down move the sun to the previous or next marker. The top left shows that marker's time and its offset from now (e.g. `+ 01:20 h`). The current time is at the top right. Select jumps back to now.
- Back returns to the watchface. So does 30 seconds without input.
- To open it straight from the watchface, assign Solar Times to a button in the watch's Settings → Quick Launch.

## Build and install

Requires the [Pebble SDK](https://developer.repebble.com/).

The watchface is the project at the repo root, the Solar Times app is in `solar-times/`. Build and install each from its folder:

```bash
pebble build
pebble install --emulator emery   # emulator
pebble install --cloudpebble      # watch
```

To preview a specific time, set `DEBUG_FAKE_MINUTE` in `src/c/main.c` (or `solar-times/src/c/main.c`) to a minute of the day (e.g. `990` for 16:30).
