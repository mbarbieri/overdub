# PROTOTYPE: AU monitoring + dry, latency-compensated Takes (issue #6)

Throwaway spike. It lives only on branch `spike/au-monitoring` and is never merged to main.

**Question:** does JUCE 9 deliver the core recording loop well enough to commit to? That loop is: two Armed inputs, each monitored live through an AU, both recorded dry while a Backing Track plays, and each Take placed with latency compensation.

## Run

```sh
spikes/au-monitoring/build.sh            # applies juce-force-auv2-oop.patch, builds Release
open -n "build/spike-au-monitoring-Release/OverdubAuSpike_artefacts/Release/Overdub AU Spike.app"
```

Scripted measurement (writes `~/Library/Logs/OverdubAuSpike/results-*.txt` and `spike.log`):

```sh
open -n "<app>" --args --auto [--oop] --plugin1 NeuralAmpModeler --plugin2 AUDistortion \
    --buffers 32,64,128,256 --cpu-seconds 5 --editors --quit
```

- `--oop` sets `OVERDUB_SPIKE_AU_OOP=1` during instantiation. The JUCE patch then loads AUv2 through `AudioComponentInstantiate(kAudioComponentInstantiation_LoadOutOfProcess)`.
- **Loopback test.** It plays seven 10 ms noise bursts as the Backing Track and records both tracks dry. It then cross-correlates take 0 against the source. `measured_rt` is the real round trip, `reported_rt` is in + out + graph latency, and `align_error` is measured minus reported. Patch an output into an input for an electrical loopback. With the built-in devices the loop is acoustic, through the speakers into the mic.
- **CPU phase.** Plugins process and both tracks record while monitoring is muted (speakers plus mic would feed back).

## Layout

- `SpikeEngine`: device callback → one `AudioProcessorGraph`. Its nodes are `input → [plugin] → MonitorGain → output`, `BackingPlayer → output` and `input → DryRecorder`. The recorder is fed straight from the input node, so PDC never delays it.
- `DryRecorder`: `ThreadedWriter` per track. It stamps the transport position of the first dry block. Placement = first block − (input + output + graph latency).
- `LoopbackTest`: signal generation and cross-correlation analysis.
- `HalDump`: raw CoreAudio latency properties for every device, including JUCE's private aggregate.
- `results/`: the measurement runs from 2026-10-07 on an Intel i7-9750H MacBook Pro (macOS 26.6) using the built-in mic and speakers.
