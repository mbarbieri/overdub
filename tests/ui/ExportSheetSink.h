#pragma once

#include "ui/ExportSheet.h"
#include "ui/Intent.h"

#include <variant>
#include <vector>

namespace overdub::ui::testing
{
// An Export sheet's Intents handled as SongView handles them, without a Song: the format, the length
// and the Video Take call the sheet back, and Export takes the sheet's request. What each one asked
// for is kept for the test to check.
struct ExportSheetSink
{
    std::vector<exporting::ExportRequest> requests;
    std::vector<song::Id> videoTakesChosen;
    int closes = 0; // show_export { false }: Cancel

    void connect (ExportSheet& sheet)
    {
        sheet.onIntent = [this, &sheet] (const Intent& intent) {
            const auto* songIntent = std::get_if<SongIntent> (&intent);
            if (songIntent == nullptr)
                return;
            if (const auto* format = std::get_if<intent::SetExportFormat> (songIntent))
                sheet.chooseFormat (format->format);
            if (const auto* length = std::get_if<intent::SetExportLength> (songIntent))
                sheet.chooseLength (length->length);
            if (const auto* videoTake = std::get_if<intent::SetExportVideoTake> (songIntent))
            {
                sheet.chooseVideoTake (videoTake->videoTakeId);
                videoTakesChosen.push_back (videoTake->videoTakeId);
            }
            if (std::holds_alternative<intent::Export> (*songIntent))
                requests.push_back (sheet.request());
            if (const auto* show = std::get_if<intent::ShowExport> (songIntent); show != nullptr && ! show->shown)
                ++closes;
        };
    }
};
} // namespace overdub::ui::testing
