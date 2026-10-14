#pragma once

#include <ostream>
#include <string>
#include <vector>

// The overdub-separate command (ADR 0007, issue #113):
//
//     overdub-separate <model.onnx> <input audio file> <output folder>
//
// decodes the input at 44.1 kHz stereo (a mono file is copied to both channels), runs the
// Separation and writes one 32-bit float, 44.1 kHz, stereo WAV per part into the output folder
// (created if missing), named after the part in the model's `sources` metadata (bass.wav,
// drums.wav, ...), each exactly as long as the decoded input.
//
// Protocol for the parent process. Every line is one JSON object, flushed as it is written.
//   stdout: {"event":"separation.started","chunks":N,"samples":N,"parts":[...]}
//           {"event":"separation.chunk_done","chunk":i,"chunks":N}   (i = 1..N)
//           {"event":"separation.part_written","part":"bass","path":"..."}   (once per part)
//           {"event":"separation.finished","seconds":s,"peak_memory_mb":m}
//   stderr: on failure, exactly one line {"error":"<code>","message":"...", <the Error's fields>}
//   exit:   0 only when every part is fully written; 1 on any failure; 2 on wrong usage.
// Error codes: separation.usage, separation.model_missing, separation.model_unloadable,
// separation.model_mismatch, separation.input_unreadable, separation.output_unwritable,
// separation.inference_failed, separation.part_write_failed, separation.internal_error.
namespace overdub::separation
{
inline constexpr int exitSuccess = 0;
inline constexpr int exitFailure = 1;
inline constexpr int exitUsage = 2;

// `arguments` excludes the program name. Relative paths resolve against the working directory.
int runSeparateCommand (const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err);
} // namespace overdub::separation
