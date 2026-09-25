// See include/port/screenshot.h. Finds this process's own game window by CGWindowID (via its own
// PID, not by matching a name string) so the screenshot mechanism (src/port/vi.cpp) can capture
// only that window instead of the whole screen.
#ifndef TARGET_PC
#error "src/port/screenshot.cpp is host-only (TARGET_PC)"
#endif

#include "port/screenshot.h"

#include <ApplicationServices/ApplicationServices.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>

namespace re4_port {

namespace {

// CGWindowListCopyWindowInfo returns every on-screen window across all apps; picks the first one
// whose kCGWindowOwnerPID matches our own process -- robust against title/owner-name changes,
// unlike matching on a string like "re4_boot".
std::optional<CGWindowID> FindOwnWindowId()
{
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (list == nullptr) {
        return std::nullopt;
    }
    const pid_t myPid = getpid();
    std::optional<CGWindowID> found;
    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; ++i) {
        auto* info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        CFNumberRef ownerPidRef =
            static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowOwnerPID));
        if (ownerPidRef == nullptr) {
            continue;
        }
        int ownerPid = 0;
        CFNumberGetValue(ownerPidRef, kCFNumberIntType, &ownerPid);
        if (ownerPid != static_cast<int>(myPid)) {
            continue;
        }
        CFNumberRef windowNumberRef =
            static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowNumber));
        if (windowNumberRef == nullptr) {
            continue;
        }
        int windowNumber = 0;
        CFNumberGetValue(windowNumberRef, kCFNumberIntType, &windowNumber);
        found = static_cast<CGWindowID>(windowNumber);
        break;
    }
    CFRelease(list);
    return found;
}

} // namespace

void CaptureOwnWindowScreenshot(const char* destPath)
{
    std::optional<CGWindowID> windowId = FindOwnWindowId();
    if (windowId.has_value()) {
        std::fprintf(stderr, "re4_boot: screenshot window id=%u\n", *windowId);
        std::string cmd = "/usr/sbin/screencapture -x -l " + std::to_string(*windowId) + " '" +
                           destPath + "'";
        int rc = std::system(cmd.c_str());
        // Window-only on purpose: a whole-screen fallback captures whatever else is on the user's
        // desktop (other apps, private messages). If the per-window capture fails, no file is made.
        if (rc == 0) {
            std::fprintf(stderr, "re4_boot: screenshot attempted -> %s (window-specific)\n", destPath);
            return;
        }
        std::fprintf(stderr, "re4_boot: window-specific screenshot failed (rc=%d), none taken\n", rc);
    } else {
        std::fprintf(stderr, "re4_boot: no own window found for screenshot, none taken\n");
    }
}

} // namespace re4_port
