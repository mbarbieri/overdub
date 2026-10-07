# Building and testing Overdub

Requirements: macOS, Apple clang with C++23 (Command Line Tools are enough), CMake 3.25+, Ninja. v1 builds `x86_64` only (ADR 0001, issue #11).

## First checkout

```sh
git submodule update --init --recursive   # external/JUCE (9.0.3), external/Catch2 (v3), external/json
```

## Build and test

```sh
cmake --preset debug            # configure into build/debug; applies the JUCE patch
cmake --build --preset debug    # builds Overdub.app, overdub_core and overdub_tests
ctest --preset debug            # runs every Catch2 test case
open build/debug/src/app/Overdub_artefacts/Debug/Overdub.app
```

Use `release` in place of `debug` for an optimised build. The first build compiles JUCE once into `overdub_juce` (about 1.5 minutes on an i7-9750H); later builds only recompile what changed.

## Layout

- `src/<area>/`: one folder per feature area from ADR 0006 (`engine`, `plugin`, `song`, `capture`, `video`, `export`, `ui`, `app`), created when a slice needs it. Includes are rooted at `src/`: `#include "core/BuildInfo.h"`.
- `src/core/` → `overdub_core`: shared infrastructure every area links (logging, errors, RT event ring). It is not an event area.
- `src/app/` → `Overdub` (`Overdub.app`): entry point and main window.
- `external/` → `overdub_juce`: the JUCE modules, compiled once. Our targets get JUCE through `overdub_core`, never by linking `juce::juce_*` modules directly (that would compile JUCE again and duplicate symbols).
- `tests/<area>/` → `overdub_tests`: one Catch2 executable. Each `TEST_CASE` becomes one ctest test.
- `tests/support/` → `overdub_rt_guard` (test-only dylib, never linked into `Overdub.app`): the allocation and lock guard from ADR 0005. Wrap real-time code in `REQUIRE_REALTIME_SAFE (...)` from `support/RealtimeGuardCatch.h`; it fails the test if the code allocates, frees or takes a blocking lock on this thread, including inside JUCE and libc++. It is a dylib because it interposes `malloc`-family and lock functions through dyld's `__interpose` section, which dyld ignores in the main executable. `RealtimeGuard.h` lists what it catches and what it does not.

## Logs

Each launch writes `~/Library/Logs/Overdub/overdub-<UTC timestamp>-<pid>.jsonl` (ADR 0006); the last 20 launches are kept. Debug builds log `debug` and up and echo every line to stderr; release builds log `info` and up. Override at launch with `OVERDUB_LOG=debug` (or `info`, `warn`, `error`):

```sh
OVERDUB_LOG=debug build/release/src/app/Overdub_artefacts/Release/Overdub.app/Contents/MacOS/Overdub
jq -c 'select(.lvl == "error")' "$(ls -1 ~/Library/Logs/Overdub/overdub-*.jsonl | tail -1)"
```

In code: `core::logEvent` (`core/Log.h`) off the audio thread; `core::RtEventRing` + an `RtEventSpec` catalog (`core/RtEventRing.h`) on it; `core::Expected<T>`, `core::logError` and `OVERDUB_CHECK` (`core/Error.h`) for failures. Tests use `core::ScopedLogCapture` and never read the log file.

## The JUCE patch

`patches/juce-force-auv2-oop.patch` makes JUCE load AUv2 plugins out-of-process through `AUHostingService`, as it already does for AUv3 (ADR 0002). Configuring applies it to `external/JUCE` idempotently: if it is already applied nothing happens, if it applies cleanly it is applied, otherwise configure stops with an error. `external/JUCE` shows as modified inside the submodule; `.gitmodules` sets `ignore = dirty` so the superproject stays clean.

### Re-applying the JUCE patch (after bumping JUCE)

1. `git -C external/JUCE checkout -- . && git -C external/JUCE checkout <new-tag>`
2. `cmake --preset debug`. If the patch still applies, you are done.
3. If configure fails, edit `createAudioUnit` in `external/JUCE/modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h` by hand so the `AudioComponentInstantiate(..., kAudioComponentInstantiation_LoadOutOfProcess, ...)` branch runs for every AU, then regenerate the patch: `git -C external/JUCE diff > patches/juce-force-auv2-oop.patch`. JUCE sources use CRLF line endings; `.gitattributes` keeps the patch bytes exact.
4. Commit the new submodule commit and the patch together.

## Code signing

Overdub.app is signed after every build with the keychain identity named by the `OVERDUB_CODESIGN_IDENTITY` cache variable (default `Overdub Local Development`), without the hardened runtime. A stable identity keeps camera and microphone permissions across rebuilds (ADR 0001). If the identity is missing, configure warns and the app is signed ad-hoc: it still runs, but macOS asks for permissions again after each build.

To create the identity once: Keychain Access → Certificate Assistant → Create a Certificate… → Name `Overdub Local Development`, Identity Type `Self Signed Root`, Certificate Type `Code Signing`. Then re-run `cmake --preset debug`. Check with `security find-identity -p codesigning` (a self-signed certificate may be listed as not trusted; codesign still uses it).
