# App startup and audio device

On launch, Overdub asks for microphone access the first time, opens the saved audio device, and falls back to the default device when the saved one is missing. It then shows the main window, reopens the Last Song (the Song most recently opened successfully on this Mac) or shows the no-Song page when there is none, and raises Notices such as the uncalibrated-latency Notice.

## Sub-features

- `startup-ready`: the window comes up and an audio device opens.
- `startup-device-fallback`: a saved device that is missing falls back to the default device instead of leaving the app silent.
- `startup-notices`: Notices for the state at startup (for example `ui.latency_not_calibrated`).
- `startup-last-song`: the Song that last finished opening (New Song…, Open…, Duplicate…) is reopened at the next launch through the `reopen_last_song` Intent, logging `ui.last_song_reopened` (`path`).
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
- **Last Song.** Run `.claude/skills/verify/scripts/journey-last-song.sh` on a fresh launch (or `journeys-all.sh last-song`). It creates a Song, quits, launches again with `LAST_SONG_FILE` naming the first launch's `last-song.json`, and checks the Song is open in Record mode and `ui.last_song_reopened` was logged. The journey quits both launches itself.
- **Quit.** Run `.claude/skills/verify/scripts/cleanup.sh`. It prints `cleanup: stopped Overdub pid N`, and the last line of `$RUN_DIR/overdub.jsonl` has `"event":"app.stopped"`.

## Gotchas

- A launch with `OVERDUB_CONTROL_SOCKET` set neither reads nor records the Last Song unless `OVERDUB_LAST_SONG_FILE` names a file. `launch.sh` always passes `$RUN_DIR/last-song.json` (or `$LAST_SONG_FILE`), so a journey never touches `~/Library/Application Support/Overdub/last-song.json`.
- The window itself (its layout, the Notice banner) cannot be seen. `overdub-ctl state parts='["messages"]'` shows the Notice the window shows (`/messages/notice/code`), and the log shows that it was raised.
- On this Mac, `audio-device.json` names a NUX NGA-3BT interface that is usually unplugged, so the fallback path runs on most launches.
- Launching the bare binary instead of using `open` makes the quit Apple Event fail with error -600, and cleanup has to SIGKILL. Use `launch.sh`.
