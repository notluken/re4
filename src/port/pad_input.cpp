// See include/port/pad_input.h.
#ifndef TARGET_PC
#error "src/port/pad_input.cpp is host-only (TARGET_PC)"
#endif

#include "port/pad_input.h"

#include <dolphin/pad.h> // our own copy: real PAD_BUTTON_*/PAD_TRIGGER_* bit values (hardware-
                          // shaped, safe to include -- see below for why the *_KEYBOARD_* API
                          // itself needs a separate, local extern "C" declaration instead)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL_scancode.h>

// Aurora's own real, TARGET_PC-only keyboard-binding API (../aurora/lib/dolphin/pad/pad.cpp) --
// not declared in this repo's own include/dolphin/pad.h (that header mirrors real hardware, which
// has no keyboard concept at all), and RE4_GAME_INCLUDES is searched before Aurora's own include
// dir even for src/port/ targets (docs/port-boot.md section 37's src/port/card.cpp precedent),
// so `#include <dolphin/pad.h>` above already resolved to this repo's own copy. Local extern "C"
// declarations (matching Aurora's real, ordinary-linkage symbols) sidestep that, same pattern.
extern "C" {
struct PADKeyButtonBinding {
    int scancode;
    unsigned short padButton;
};
void PADSetKeyboardActive(unsigned int port, int active);
int PADSetKeyButtonBindings(unsigned int port, PADKeyButtonBinding bindings[12]); // PAD_BUTTON_COUNT
}

namespace re4_port {

void InitKeyboardInput()
{
    // Coordinator's minimal mapping (docs/port-boot.md section 38): Enter/Return = Start, Z = A,
    // X = B, C = X, V = Y, arrow keys = D-pad. Order must match Aurora's own PAD_BUTTON_* array
    // order (A, B, X, Y, START, Z, L, R, UP, DOWN, LEFT, RIGHT -- ../aurora/lib/dolphin/pad/pad.cpp
    // g_defaultKeys, confirmed by reading it directly, not guessed) -- PADSetKeyButtonBindings()
    // takes a full PAD_BUTTON_COUNT-sized array, one entry per button in that fixed order.
    PADKeyButtonBinding bindings[12] = {
        {SDL_SCANCODE_Z, PAD_BUTTON_A},
        {SDL_SCANCODE_X, PAD_BUTTON_B},
        {SDL_SCANCODE_C, PAD_BUTTON_X},
        {SDL_SCANCODE_V, PAD_BUTTON_Y},
        {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
        {SDL_SCANCODE_A, PAD_TRIGGER_Z}, // Z trigger -- 'A' key, arbitrary but out of the way of
                                          // the WASD/arrow movement keys below
        {-1, PAD_TRIGGER_L},              // no default key -- rarely needed to reach the title
        {-1, PAD_TRIGGER_R},               // screen, left unbound rather than guessing one
        {SDL_SCANCODE_UP, PAD_BUTTON_UP},
        {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN},
        {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT},
        {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT},
    };
    PADSetKeyButtonBindings(0, bindings);
    PADSetKeyboardActive(0, 1);
    std::fprintf(stderr, "re4_port: keyboard input active on PAD channel 0 -- Return=Start, "
                          "Z=A, X=B, C=X, V=Y, A=Z-trigger, arrows=D-pad\n");
}

namespace {

struct ScriptedEntry {
    long frame;
    u32 bit;
};

u32 ButtonNameToBit(const std::string& name)
{
    if (name == "A") return PAD_BUTTON_A;
    if (name == "B") return PAD_BUTTON_B;
    if (name == "X") return PAD_BUTTON_X;
    if (name == "Y") return PAD_BUTTON_Y;
    if (name == "START") return PAD_BUTTON_START;
    if (name == "Z") return PAD_TRIGGER_Z;
    if (name == "L") return PAD_TRIGGER_L;
    if (name == "R") return PAD_TRIGGER_R;
    if (name == "UP") return PAD_BUTTON_UP;
    if (name == "DOWN") return PAD_BUTTON_DOWN;
    if (name == "LEFT") return PAD_BUTTON_LEFT;
    if (name == "RIGHT") return PAD_BUTTON_RIGHT;
    std::fprintf(stderr, "re4_port: RE4_PORT_INPUT: unknown button name '%s', ignored\n",
                 name.c_str());
    return 0;
}

std::vector<ScriptedEntry>& Schedule()
{
    static std::vector<ScriptedEntry> schedule = [] {
        std::vector<ScriptedEntry> out;
        const char* env = std::getenv("RE4_PORT_INPUT");
        if (env == nullptr || env[0] == '\0') {
            return out;
        }
        std::string s(env);
        std::size_t pos = 0;
        while (pos < s.size()) {
            std::size_t comma = s.find(',', pos);
            std::string entry = s.substr(pos, comma == std::string::npos ? comma : comma - pos);
            std::size_t colon = entry.find(':');
            if (colon != std::string::npos) {
                long frame = std::strtol(entry.substr(0, colon).c_str(), nullptr, 10);
                u32 bit = ButtonNameToBit(entry.substr(colon + 1));
                if (bit != 0) {
                    out.push_back({frame, bit});
                    std::fprintf(stderr, "re4_port: RE4_PORT_INPUT: frame %ld -> bit 0x%04x\n",
                                 frame, bit);
                }
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
        return out;
    }();
    return schedule;
}

} // namespace

u32 PollScriptedInput()
{
    static long frameCounter = 0;
    static const long kHoldFrames = [] {
        if (const char* env = std::getenv("RE4_PORT_INPUT_HOLD_FRAMES")) {
            long v = std::strtol(env, nullptr, 10);
            if (v > 0) {
                return v;
            }
        }
        return 10L;
    }();

    long frame = frameCounter++;
    u32 bits = 0;
    for (const ScriptedEntry& e : Schedule()) {
        if (frame >= e.frame && frame < e.frame + kHoldFrames) {
            bits |= e.bit;
        }
    }
    return bits;
}

} // namespace re4_port
