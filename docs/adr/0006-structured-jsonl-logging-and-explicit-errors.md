# One JSON Lines log per launch, an RT event ring, and `std::expected` errors

Overdub logs to its own JSON Lines file, one per app launch, in `~/Library/Logs/Overdub/overdub-<timestamp>-<pid>.jsonl`. We keep the last 20 launches. The writer flushes every line, and it is installed as the `juce::Logger`, so JUCE's own messages land there too. Debug builds also echo to stderr. We chose a plain file over `os_log` because every line is AI-written and AI-debugged. A session can `jq` or `grep` the file and attach it to an issue, but `log show` is slow and noisy. The logger is one process-wide object reached through a free function. It is a deliberate exception to the "pass state explicitly" rule, because threading a `Logger&` through every constructor adds noise and no safety. The audio thread never touches it. Real-time code pushes fixed-size POD events into a lock-free SPSC ring owned by the engine and passed explicitly. A logger thread drains the ring every ~50 ms and turns each event into JSON. Failures travel as `std::expected<T, Error>`, which is why the language standard moves from C++20 to C++23 (ADR 0001). The AU spike, JUCE 9.0.3 included, builds cleanly under C++23.

## Considered Options

- **JUCE `FileLogger`**: no code to write, but plain text lines aren't structured.
- **`os_log` / unified logging (alone or mirrored)**: native and cheap, but awkward for an agent to read or attach.
- **Atomic counters polled from the UI thread, without a ring**: simpler, but loses event order and the sample position.
- **A `Logger&` passed to every feature object**: obeys the no-globals rule, but it touches every constructor for no gain.
- **Exceptions, `juce::Result`, or `tl::expected` on C++20**: exceptions hide control flow, `juce::Result` carries no value, and `tl::expected` is a vendored copy of what C++23 ships.

## Consequences

- **Event shape.** Each line is one flat JSON object with reserved keys `t` (UTC wall time, millisecond precision), `host_ns` (`mach_absolute_time` in ns), `lvl`, `thread` (`audio | ui | video | io | logger | worker`) and `event`, plus an optional `msg`. Every other key is the event's own field. Nesting is allowed only for arrays. RT events carry the `host_ns` and `sample` from when the audio thread captured them, not from when they were drained.
- **Naming.** Event names are `area.snake_case`, where `area` is the `src/` feature folder (`engine`, `plugin`, `song`, `capture`, `video`, `export`, `ui`, `control`, `app`). `Error.code` uses the same namespace. Names are string literals at the call site and stay stable once they ship. The RT `enum` and its name table are the only catalog.
- **Levels.** `debug | info | warn | error`. Debug builds log `debug` and up, release builds `info` and up, and `OVERDUB_LOG=debug` overrides either at launch. There's no `trace` level and no per-area filter.
- **RT path.** RT events are `{code, sample, hostTimeNs, 2–4 numeric args}`. When the ring is full, an atomic `dropped` counter goes up and is logged on the next drain. Very frequent values (callback load, peak callback time) are atomics, summarised once a second as `engine.load` at `debug`, and only while playing or recording. Video and AVFoundation callbacks run on dispatch queues and use the normal API directly.
- **Required boundary events** (the minimum; slices may add more):
  - `app.started`: version, git SHA, macOS version, CPU architecture, log path.
  - `engine.device_opened` / `engine.device_closed`: devices, rate, buffer, reported latencies, calibration offset.
  - `capture.pass_started` / `capture.pass_stopped`: Pass id, Armed Tracks and inputs, camera, start sample, length, dropped and overrun counts.
  - `plugin.loaded` / `plugin.unloaded` / `plugin.invalidated`: AU id, version, latency, tail, in- or out-of-process.
  - `video.*` start, stop and dropped frames.
  - `export.started` / `export.finished`.
  - `song.opened`, `song.saved` (`debug`), `song.migrated`, `song.history_dropped`.
- **Errors.** `Error` is `{code, message, fields}` and turns straight into a log event. Third-party exceptions (nlohmann/json, file I/O) are caught at the call that can throw and converted to an `Error`. No exception crosses a feature boundary, and no exception or `expected` is used in RT code. An `Error` is logged once, where it is handled, not again at each layer it passes through.
- **Invariants.** `OVERDUB_CHECK(cond, "area.event", fields…)` logs an `error` with `file:line` in every build and lets the caller recover. In debug it also calls `jassertfalse`, which stops only when a debugger is attached, so a failed check never aborts a debug run or `ctest`. On the audio thread an RT event code replaces it.
- **What the user sees.** There are three tiers:
  - *Log only*: recovered or invisible.
  - *Notice*: a non-modal one-liner that disappears on its own, for degraded but undecided situations such as a crashed plugin slot now bypassed, or dropped camera frames.
  - *Alert*: a modal dialog, only when the user must decide or data was lost.

  No modal ever appears during a Pass. Alerts queue until stop, and recording continues whenever it can. User-facing wording comes from one `Error.code` → sentence table in `ui/`, which falls back to a generic sentence that includes the code.
- **Tests.**
  - Tests swap in an in-memory sink and never read the log file.
  - Every Notice- or Alert-tier path has a test asserting its event and its tier.
  - Every RT event code has a test for its `sample` and `host_ns`, plus a ring-overflow test.
  - Boundary events are asserted where a test already drives that boundary.
  - Assertions check `event` and fields, never `msg`.

## Amendment: the `control` area (issue #166)

- `src/control/` is the control socket (ADR 0008). Its events and its protocol Error codes use the `control` area. An Error keeps the area of the folder that raises it, so a refusal returned over the socket is still `ui.*`.
- An Intent that does nothing (no Song open, a Clip key outside Edit mode, nothing to undo) returns an Error that is logged at `debug` and never presented, because the UI already shows the gesture as unavailable.
