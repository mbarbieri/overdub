#pragma once

#include "ui/ExportPage.h"
#include "ui/Intent.h"

#include <variant>
#include <vector>

namespace overdub::ui::testing
{
// An Export page's Intents handled as SongView handles them, without a Song: the format, the length
// and the Video Take call the page back, and Export takes the page's request. What each one asked
// for is kept for the test to check.
struct ExportPageSink
{
    std::vector<exporting::ExportRequest> requests;
    std::vector<song::Id> videoTakesChosen;
    int reveals = 0;

    void connect (ExportPage& page)
    {
        page.onIntent = [this, &page] (const Intent& intent) {
            const auto* songIntent = std::get_if<SongIntent> (&intent);
            if (songIntent == nullptr)
                return;
            if (const auto* format = std::get_if<intent::SetExportFormat> (songIntent))
                page.chooseFormat (format->format);
            if (const auto* length = std::get_if<intent::SetExportLength> (songIntent))
                page.chooseLength (length->length);
            if (const auto* videoTake = std::get_if<intent::SetExportVideoTake> (songIntent))
            {
                page.chooseVideoTake (videoTake->videoTakeId);
                videoTakesChosen.push_back (videoTake->videoTakeId);
            }
            if (std::holds_alternative<intent::Export> (*songIntent))
                requests.push_back (page.request());
            if (std::holds_alternative<intent::RevealExport> (*songIntent))
                ++reveals;
        };
    }
};
} // namespace overdub::ui::testing
