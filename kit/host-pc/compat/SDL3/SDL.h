#pragma once
// The PC build: the few SDL3 names the game's sources use, as host-switch/compat/SDL3/SDL.h answers them
#include <cstdint>

struct SDL_Window;
typedef uint32_t SDL_DisplayID;

struct SDL_DisplayMode {
    int w, h;
    float pixel_density;
    float refresh_rate;
};

inline SDL_DisplayID SDL_GetPrimaryDisplay() { return 1; }
inline SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window*) { return 1; }
inline const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID) {
    static SDL_DisplayMode m{1280, 720, 1.0f, 60.0f};
    return &m;
}

enum : uint32_t { SDL_EVENT_QUIT = 0x100 };

union SDL_Event {
    uint32_t type;
    uint8_t padding[128];
};

namespace kernel {
[[noreturn]] void Quit();
}

inline bool SDL_PushEvent(SDL_Event* e) {
    if (e && e->type == SDL_EVENT_QUIT) kernel::Quit();
    return true;
}
