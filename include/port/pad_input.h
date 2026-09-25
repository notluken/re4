// Coordinator follow-up (docs/port-boot.md section 38): real keyboard input (via Aurora's own
// PADSetKeyboardActive()/PADSetKeyButtonBindings() -- Aurora ships that API but with every key
// left PAD_KEY_INVALID by default, docs/port-boot.md section 38's own trace) for interactive runs,
// plus a scripted, deterministic input source for non-interactive/automated runs.
#ifndef TARGET_PC
#error "include/port/pad_input.h is TARGET_PC-only"
#endif

#include "types.h"

namespace re4_port {

// Installs a minimal GameCube-shaped keyboard mapping on PAD channel 0 and activates it (Aurora's
// own real PADRead() then ORs keyboard state into channel 0's PADStatus alongside any connected
// real gamepad -- see pad.cpp's merge_virtual_status()-adjacent logic, docs/port-boot.md section
// 38). Call once, after PADInit() (src/game/pad.cpp's real PadInit(), TARGET_PC branch) --
// PADSetKeyboardActive()/PADSetKeyButtonBindings() are real Aurora entry points, not stubs.
void InitKeyboardInput();

// Scripted input for automated/non-interactive runs (docs/port-boot.md section 38): parses
// $RE4_PORT_INPUT once, lazily, on first call -- format "frame:BUTTON,frame:BUTTON,..."
// (e.g. "120:A,240:START"), BUTTON one of A/B/X/Y/START/Z/L/R/UP/DOWN/LEFT/RIGHT (case-sensitive,
// matching the real PAD_BUTTON_* names). Each entry is held for RE4_PORT_INPUT_HOLD_FRAMES frames
// (default 10) starting at its listed frame. Returns the bitwise-OR of every button bit "held" on
// the frame this call represents (advances its own internal frame counter by one per call -- call
// exactly once per real frame, matching PadRead()'s own once-per-frame contract). Returns 0 (no
// bits) when $RE4_PORT_INPUT is unset -- a complete no-op for interactive/default runs.
u32 PollScriptedInput();

} // namespace re4_port
