# ClickAssist

ClickAssist is a Geometry Dash practice assist that predicts a short input
trajectory and can delay a press or apply a conservative release correction
when that path is safer. It is for learning, testing, and casual play.

## Run validity

When an assist stage is active, every gameplay session is **assisted and
invalid** for leaderboard, record, verification, or top-play submission. The
HUD shows a small dim red `ClickAssist` marker with the active tick limit. The
marker cannot be hidden through the mod's settings while assistance is active.
Stage 5 is true raw pass-through input, so it intentionally hides both HUD
labels.

The mod is deliberately transparent: it appears in Geode's normal mod list,
uses normal Geode settings, and does not present assisted gameplay as a valid
achievement.

## Features

- Short-horizon trajectory simulation for buffered presses.
- Conservative emergency release assistance for wave and robot.
- Continuous, frame-synchronised threat probing with bounded work.
- Collision handling for common hazards, slopes, pads, and angled geometry.
- Optional detailed diagnostics written only to a local telemetry file.

## Settings

Geode shows ClickAssist in its regular mod list. It provides these release
settings:

- **Show Live Console** — off by default; opens the diagnostic terminal after
  restart.
- **Write Detailed Telemetry** — off by default; writes diagnostics after
  restart.
- **HUD Position** — places the dimmed marker and tick label at any screen
  corner or horizontal center.
- **Cycle Assist Stage** — an editable gameplay keybind; defaults to `F6`.

Detailed telemetry is written to:

`%LOCALAPPDATA%\\GeometryDash\\geode\\mods\\ai.predictivebuffer\\prediction-telemetry.log`

Disabling diagnostics does not remove the assisted/invalid HUD marker or make
an assist-stage run valid.

## Build

Set `GEODE_SDK` to a compatible Geode SDK, configure with CMake, then build:

```powershell
cmake -B build
cmake --build build
```

The packaged `.geode` is emitted in `build/`.

## Release policy

Do not submit runs made with ClickAssist to leaderboards, record lists,
verification teams, or top-play showcases. If you share footage, disclose that
ClickAssist was active.

## License

ClickAssist is licensed under the [GNU General Public License v3.0](LICENSE).
