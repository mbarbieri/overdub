# App startup and audio device

On launch, Overdub asks for microphone access the first time, opens the saved audio device, and falls back to the default device when the saved one is missing. It then shows the main window with no Song open and raises Notices such as the uncalibrated-latency Notice.

## Sub-features

- `startup-ready`: the window comes up and an audio device opens.
- `startup-device-fallback`: a saved device that is missing falls back to the default device instead of leaving the app silent.
- `startup-notices`: Notices for the state at startup (for example `ui.latency_not_calibrated`).
- `startup-quit`: a normal quit logs `app.stopped`.

## How to get to it (user POV)

- Open Overdub.app from Finder or the Dock.
- Quit with ⌘Q or Overdub → Quit.

## Driving it with launch.sh / doctor.sh / jq

Preconditions:

- No Overdub is running.
- The debug preset is configured.

- **Launch.** Run `.claude/skills/verify/scripts/launch.sh startup`. It prints `READY="event":"engine.device_opened"` (or `app.device_unavailable` on a Mac with no device).
- **Health.** Run `.claude/skills/verify/scripts/doctor.sh`. Every line reads `ok`. The trailing event list shows the opened device's name and sample rate.
- **Fallback.** Run `jq -c 'select(.event|test("^app.device_|^engine.device_opened"))' "$LOG"`. When the saved device is unplugged, you see `app.device_open_failed` with `"reason":"No such device: …"`, then `engine.device_opened` with the built-in device.
- **Notices.** Run `jq -c 'select(.event=="ui.notice_shown")' "$LOG"`. Each Notice code is listed. Its wording is in `src/ui/UserMessages.h`.
- **Quit.** Run `.claude/skills/verify/scripts/cleanup.sh`. It prints `cleanup: stopped Overdub pid N`, and the last line of `$RUN_DIR/overdub.jsonl` has `"event":"app.stopped"`.

## Gotchas

- The window itself (its layout, the Notice banner) cannot be seen. `overdub-ctl state parts='["messages"]'` shows the Notice the window shows (`/messages/notice/code`), and the log shows that it was raised.
- On this Mac, `audio-device.json` names a NUX NGA-3BT interface that is usually unplugged, so the fallback path runs on most launches.
- Launching the bare binary instead of using `open` makes the quit Apple Event fail with error -600, and cleanup has to SIGKILL. Use `launch.sh`.
