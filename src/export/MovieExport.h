#pragma once

#include "core/Error.h"
#include "export/MixExport.h"
#include "export/MovieSpan.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

// Export mode's MP4 (issue #38, ADR 0003): the offline mix and one Video Take in one movie.
//
// The mix is rendered (MixRender) into a hidden WAV beside the destination, then an
// AVMutableComposition holds the mix at time 0 and the Video Take at its sample-exact start (a
// CMTime in Song samples), its movie scaled by the drift ratio onto the Song clock.
// AVAssetExportSession writes H.264 + AAC into a hidden partial file that is renamed over the
// destination once complete; the mix file is always deleted. The range (export/MovieSpan.h) is
// the Video Take's span, or for the full song [0, max (mix end, Video Take end)) with an
// AVMutableVideoComposition's black background where there is no video. The full song re-times
// frames onto an output grid of the Video Take's mean frame duration on the Song clock; the video
// enters the composition half a grid step early so each grid point shows the nearest source
// frame: up to ±½ frame per frame (a placement's phase on the grid), no systematic offset.
// The mix file is padded with silence to the range's end, so a Song with no Clips (an empty mix)
// still exports its Video Take, over silence (issue #67).
//
// Logs export.started (format mp4, path, video_take_id, length, start_sample, end_sample,
// sample_rate) and export.finished (format, path, samples, elapsed_ms).
//
// Errors: export.video_take_outside_song (video_take_id, video_take_end_sample: the range is
// empty because the Video Take ends at or before Song sample 0), export.cancelled,
// export.video_take_unreadable (path, video_take_id, reason), export.movie_failed (step:
// compose_audio | compose_video | encode, detail), export.write_failed (as exportMix's).
namespace overdub::exporting
{
// The Video Take an MP4 shows, and its movie file.
struct MovieSource
{
    juce::File movie;
    song::VideoTake videoTake;
};

// Writes request.destination at request.videoLength (the request's videoTakeId is the caller's:
// `source` is what is shown). Progress: the mix render is the first half, the encode the second.
// ExportResult::samples is the length of the exported range.
core::Expected<ExportResult> exportMovie (MixRender& render, const ExportRequest& request, const MovieSource& source, const ProgressFn& progress = {}, const ShouldStopFn& shouldStop = {});
} // namespace overdub::exporting
