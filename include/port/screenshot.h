// Captures a screenshot of this process's own game window (found by CGWindowID via its own PID),
// falling back to a whole-screen capture if the window-specific path fails. Implementation:
// src/port/screenshot.cpp. Used by src/port/vi.cpp's RE4_PORT_SCREENSHOT mechanism.
#ifndef TARGET_PC
#error "include/port/screenshot.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_SCREENSHOT_H
#define RE4_PORT_SCREENSHOT_H

namespace re4_port {

void CaptureOwnWindowScreenshot(const char* destPath);

} // namespace re4_port

#endif // RE4_PORT_SCREENSHOT_H
