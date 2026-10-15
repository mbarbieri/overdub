# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Overdub: a macOS app (JUCE 9.0.3, C++23, CMake + Ninja) for recording songs one part at a time over backing tracks, with optional camera video. Read `docs/building.md` for layout and build details, and the ADR in `docs/adr/` that governs any area you touch.

## Build and test

```sh
git submodule update --init --recursive   # first checkout; configure fails on empty external/*
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
ctest --preset debug -R "<test case name regex>"    # a subset by name
build/debug/tests/overdub_tests "[engine][pdc]"      # a subset by Catch2 tag
```

- `make app` builds the release, signed `dist/Overdub.app` (and verifies it); `make install` copies it to `/Applications`; `make test` is the debug build plus full `ctest`. Details in `docs/building.md`.
- Always use the presets: they set `x86_64`, which `CMakeLists.txt` requires (v1 is Intel-only). Keep code arm64-clean anyway: no SSE/AVX intrinsics or `x86_64` assumptions.
- Configure applies `patches/*.patch` to `external/JUCE`; the submodule shows dirty on purpose. Never edit JUCE sources without regenerating the patch (see "Re-applying the JUCE patch" in `docs/building.md`). Patches keep CRLF bytes. If configure fails with `patch failed` after switching branches, the old patch is still applied: run `git -C external/JUCE checkout -- .` and configure again.
- Link JUCE only through `overdub_core`, never `juce::juce_*` modules directly (duplicate symbols, second JUCE compile).
- ONNX Runtime and pocketfft link only into `overdub-separate` (`src/separation/`), never `overdub_core` or the app; only `make app`'s bundle check catches a violation.
- New test files must be added to `target_sources` in `tests/CMakeLists.txt`, under `tests/<area>/` mirroring `src/`. Tag `TEST_CASE`s `[area][subarea]`.
- The full suite is the gate for every change.

## Testing rules (ADR 0005)

- Assert computed expectations from synthetic stimuli (impulses, null tests, sines). Never add golden WAV/MP4/screenshot files.
- Tolerances in ADR 0005 are spec: loosening one is a spec change, not a test fix.
- Drive the engine through `Engine::process(...)` with fake buffers/host times; no test opens real audio hardware (`tests/engine/FakeAudioIODevice.h`).
- No test runs the real Separation model: use `tests/song/FakeSeparationHelper.h` or the stand-in graph in `tests/separation/OnnxTestModel.h`.
- Wrap audio-thread code in `REQUIRE_REALTIME_SAFE (...)` (`tests/support/RealtimeGuardCatch.h`).
- Tests capture logs with `core::ScopedLogCapture` and assert `event` and fields, never `msg`.

## Code conventions

- Realtime code: no allocation, locks, logging, exceptions or `Expected`. Push an `RtEvent` into the engine's `RtEventRing` instead. Free replaced plans off the audio thread.
- Errors: `core::Expected<T>`; log an `Error` once where it is handled (`core::logError`). Catch third-party exceptions (nlohmann/json, file I/O) at the call site; no exception crosses a feature boundary. Invariants use `OVERDUB_CHECK`.
- Logging: `core::logEvent` off the audio thread. Event names and `Error.code` are `area.snake_case` where area is the `src/` folder; field keys are snake_case.
- `core` has no `core.*` events; it logs under its caller's area.
- Formatting is enforced by `.clang-format` (JUCE style: `foo (x)`, `! cond`, Allman braces, unindented namespaces, the header under test included first). Every C++/ObjC++ file in `src/` and `tests/` must be clean before committing:
  ```sh
  git ls-files -z 'src/*.cpp' 'src/*.h' 'src/*.mm' 'tests/*.cpp' 'tests/*.h' | xargs -0 xcrun clang-format -i            # fix
  git ls-files -z 'src/*.cpp' 'src/*.h' 'src/*.mm' 'tests/*.cpp' 'tests/*.h' | xargs -0 xcrun clang-format --dry-run -Werror  # check
  ```
- Includes are rooted at `src/` (`#include "core/Log.h"`).
- Use `GLOSSARY.md` terms (Song, Track, Pass, Take, Clip, Backing Track…) in code, event names, tests and commits, and avoid the synonyms it lists.

## Workflow

- Work arrives as GitHub issues. Before implementing, read the issue with `gh issue view <N>` and cover every Acceptance bullet with a test.
- Branches: `<issue>-<name>`, `spike/…`, `research/…`, `prototype/…`. Commit subjects describe the observable behavior and end with `(issue #N)`.
- PR title is a short summary of the change; body starts `Closes #N.`, ends with the test plan and pass count.
- PRs need no GitHub review: reviews are done by a local agent. Merging without a GitHub approval is fine; don't request or wait for one.
