#include "ui/DoubleClickInterval.h"

#import <AppKit/AppKit.h>

namespace overdub::ui
{
std::chrono::milliseconds systemDoubleClickInterval()
{
    return std::chrono::milliseconds ((long long) (NSEvent.doubleClickInterval * 1000.0));
}
} // namespace overdub::ui
