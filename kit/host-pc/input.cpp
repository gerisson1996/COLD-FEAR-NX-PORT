// The PC build's controllers: XInput pads, the keyboard as player 1, and the "cmd" file beside the exe for scripted
// presses. Everything goes through the Switch's button bits (HidNpadButton_*, host-pc/compat/switch.h), so the
// mapping to the Xbox pad, the options panel's navigation and Minus + R3 are the Switch build's.
//
// keyboard: arrows = D-pad, WASD = left stick, IJKL = right stick, Enter = A, Backspace = B, Q = X, E = Y,
// Tab = Plus (Start), Esc = Minus (Back), 1 = L (white), 2 = R (black), Ctrl = ZL, Shift = ZR, F/G = L3/R3,
// F10 = Minus + R3 (the options panel)
// cmd file: "A", "PLUS", "DOWN+A 10", ... buttons joined by +, held for that many frames (8 without)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"

namespace input {

namespace {

std::mutex g_lock;

struct Raw {
    bool connected = false;
    uint64_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }

void Shape(int16_t& x, int16_t& y, uint32_t deadzone, float sensitivity) {
    if (!deadzone && sensitivity == 1.0f) return;
    constexpr float kFull = 32767.0f;
    float fx = float(x) / kFull, fy = float(y) / kFull;
    float m = std::sqrt(fx * fx + fy * fy);
    float dz = std::min(float(deadzone), 90.0f) / 100.0f;
    if (m <= dz) {
        x = y = 0;
        return;
    }
    float k = (m - dz) / (1.0f - dz) * sensitivity / m;
    x = Axis(fx * k);
    y = Axis(fy * k);
}

// the scripted press from the cmd file, counted in polls of port 0
struct Script {
    uint64_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int left = 0;
} g_script;

uint64_t ButtonByName(const std::string& n) {
    static const struct {
        const char* name;
        uint64_t bit;
    } kNames[] = {{"A", HidNpadButton_A},         {"B", HidNpadButton_B},         {"X", HidNpadButton_X},
                  {"Y", HidNpadButton_Y},         {"L3", HidNpadButton_StickL},   {"R3", HidNpadButton_StickR},
                  {"L", HidNpadButton_L},         {"R", HidNpadButton_R},         {"ZL", HidNpadButton_ZL},
                  {"ZR", HidNpadButton_ZR},       {"PLUS", HidNpadButton_Plus},   {"START", HidNpadButton_Plus},
                  {"MINUS", HidNpadButton_Minus}, {"BACK", HidNpadButton_Minus},  {"LEFT", HidNpadButton_Left},
                  {"UP", HidNpadButton_Up},       {"RIGHT", HidNpadButton_Right}, {"DOWN", HidNpadButton_Down}};
    for (auto& k : kNames)
        if (n == k.name) return k.bit;
    return 0;
}

void ReadCmd() {
    std::filesystem::path p = app::ExeDir() / "cmd";
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return;
    FILE* f = _wfopen(p.c_str(), L"r");
    if (!f) return;
    char b[128] = {};
    int frames = 8;
    int got = std::fscanf(f, "%127s %d", b, &frames);
    std::fclose(f);
    std::filesystem::remove(p, ec);
    if (got < 1) return;
    Script s;
    std::string all = b;
    size_t at = 0;
    while (at <= all.size()) {
        size_t e = all.find('+', at);
        if (e == std::string::npos) e = all.size();
        std::string n = all.substr(at, e - at);
        at = e + 1;
        if (n == "LS") s.ly = 32767;
        else if (n == "LSD") s.ly = -32767;
        else if (n == "LSL") s.lx = -32767;
        else if (n == "LSR") s.lx = 32767;
        else if (n == "RS") s.ry = 32767;
        else if (n == "RSD") s.ry = -32767;
        else if (n == "RSL") s.rx = -32767;
        else if (n == "RSR") s.rx = 32767;
        else s.b |= ButtonByName(n);
    }
    s.left = std::max(frames, 1);
    g_script = s;
    LOGI("pc: cmd %s for %d polls", b, frames);
}

bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool Focused() { return GetForegroundWindow() == static_cast<HWND>(app::Window()); }

Raw ReadKeyboard() {
    Raw r;
    if (!Focused()) return r;
    struct {
        int vk;
        uint64_t bit;
    } const kKeys[] = {{VK_UP, HidNpadButton_Up},        {VK_DOWN, HidNpadButton_Down},   {VK_LEFT, HidNpadButton_Left},
                       {VK_RIGHT, HidNpadButton_Right},  {VK_RETURN, HidNpadButton_A},    {VK_BACK, HidNpadButton_B},
                       {'Q', HidNpadButton_X},           {'E', HidNpadButton_Y},          {VK_TAB, HidNpadButton_Plus},
                       {VK_ESCAPE, HidNpadButton_Minus}, {'1', HidNpadButton_L},          {'2', HidNpadButton_R},
                       {VK_CONTROL, HidNpadButton_ZL},   {VK_SHIFT, HidNpadButton_ZR},    {'F', HidNpadButton_StickL},
                       {'G', HidNpadButton_StickR},      {VK_F10, HidNpadButton_Minus | HidNpadButton_StickR}};
    for (auto& k : kKeys)
        if (Key(k.vk)) r.b |= k.bit;
    if (Key('A')) r.lx = -32767;
    if (Key('D')) r.lx = 32767;
    if (Key('W')) r.ly = 32767;
    if (Key('S')) r.ly = -32767;
    if (Key('J')) r.rx = -32767;
    if (Key('L')) r.rx = 32767;
    if (Key('I')) r.ry = 32767;
    if (Key('K')) r.ry = -32767;
    r.connected = r.b || r.lx || r.ly || r.rx || r.ry;
    return r;
}

Raw ReadXInput(int port) {
    Raw r;
    XINPUT_STATE st{};
    if (XInputGetState(DWORD(port), &st) != ERROR_SUCCESS) return r;
    r.connected = true;
    const XINPUT_GAMEPAD& g = st.Gamepad;
    // positions as on the Switch: A is the Xbox A (bottom), B the right one
    struct {
        WORD x;
        uint64_t bit;
    } const kMap[] = {{XINPUT_GAMEPAD_DPAD_UP, HidNpadButton_Up},       {XINPUT_GAMEPAD_DPAD_DOWN, HidNpadButton_Down},
                      {XINPUT_GAMEPAD_DPAD_LEFT, HidNpadButton_Left},   {XINPUT_GAMEPAD_DPAD_RIGHT, HidNpadButton_Right},
                      {XINPUT_GAMEPAD_START, HidNpadButton_Plus},       {XINPUT_GAMEPAD_BACK, HidNpadButton_Minus},
                      {XINPUT_GAMEPAD_LEFT_THUMB, HidNpadButton_StickL}, {XINPUT_GAMEPAD_RIGHT_THUMB, HidNpadButton_StickR},
                      {XINPUT_GAMEPAD_LEFT_SHOULDER, HidNpadButton_L},  {XINPUT_GAMEPAD_RIGHT_SHOULDER, HidNpadButton_R},
                      {XINPUT_GAMEPAD_A, HidNpadButton_A},              {XINPUT_GAMEPAD_B, HidNpadButton_B},
                      {XINPUT_GAMEPAD_X, HidNpadButton_X},              {XINPUT_GAMEPAD_Y, HidNpadButton_Y}};
    for (auto& m : kMap)
        if (g.wButtons & m.x) r.b |= m.bit;
    if (g.bLeftTrigger > 64) r.b |= HidNpadButton_ZL;
    if (g.bRightTrigger > 64) r.b |= HidNpadButton_ZR;
    auto dz = [](SHORT v) { return int16_t(std::abs(v) < 7000 ? 0 : std::max<int>(v, -32767)); };
    r.lx = dz(g.sThumbLX);
    r.ly = dz(g.sThumbLY);
    r.rx = dz(g.sThumbRX);
    r.ry = dz(g.sThumbRY);
    return r;
}

Raw Read(int port) {
    Raw r = ReadXInput(port);
    if (port == 0) {
        Raw k = ReadKeyboard();
        r.connected = true;  // player 1 always has the keyboard
        r.b |= k.b;
        if (k.lx) r.lx = k.lx;
        if (k.ly) r.ly = k.ly;
        if (k.rx) r.rx = k.rx;
        if (k.ry) r.ry = k.ry;
        if (g_script.left > 0) {
            r.b |= g_script.b;
            if (g_script.lx) r.lx = g_script.lx;
            if (g_script.ly) r.ly = g_script.ly;
            if (g_script.rx) r.rx = g_script.rx;
            if (g_script.ry) r.ry = g_script.ry;
        }
    }
    return r;
}

Raw g_last0;

}  // namespace

void Init() { LOGI("input: XInput and the keyboard (PC build), cmd file for scripted presses"); }

Pad Poll(int port) {
    Pad p;
    if (port < 0 || port > 3) return p;
    Raw r;
    {
        std::lock_guard lock(g_lock);
        if (port == 0) {
            if (g_script.left > 0) g_script.left--;
            else ReadCmd();
        }
        r = Read(port);
        if (port == 0) g_last0 = r;
    }
    if (!r.connected) return p;
    p.connected = true;
    constexpr uint64_t kButtons[8] = {HidNpadButton_Up,   HidNpadButton_Down,  HidNpadButton_Left,     HidNpadButton_Right,
                                      HidNpadButton_Plus, HidNpadButton_Minus, HidNpadButton_StickL, HidNpadButton_StickR};
    for (int i = 0; i < 8; i++)
        if (r.b & kButtons[i]) p.buttons |= uint16_t(1u << i);
    constexpr uint64_t kAnalog[8] = {HidNpadButton_A, HidNpadButton_B, HidNpadButton_X,  HidNpadButton_Y,
                                     HidNpadButton_R, HidNpadButton_L, HidNpadButton_ZL, HidNpadButton_ZR};
    for (int i = 0; i < 8; i++) p.analog[i] = (r.b & kAnalog[i]) ? 0xFF : 0;
    p.lx = r.lx;
    p.ly = r.ly;
    p.rx = r.rx;
    p.ry = r.ry;
    constexpr uint16_t kThumbs = (1u << 6) | (1u << 7);
    static bool held = false;
    if (port == 0) held = (p.buttons & kThumbs) == kThumbs || (held && (p.buttons & kThumbs));
    if (app::MenuOpen() || held) {
        bool connected = p.connected;
        p = Pad{};
        p.connected = connected;
    }
    const game::Options& o = game::Opt();
    Shape(p.lx, p.ly, o.left_deadzone, o.left_sensitivity);
    Shape(p.rx, p.ry, o.right_deadzone, o.right_sensitivity);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    if (port < 0 || port > 3) return;
    XINPUT_VIBRATION v{left, right};
    XInputSetState(DWORD(port), &v);
}

uint64_t NavButtons() {
    std::lock_guard lock(g_lock);
    return Read(0).b;
}

void NavStick(float& x, float& y) {
    std::lock_guard lock(g_lock);
    Raw r = Read(0);
    x = r.lx / 32767.0f;
    y = r.ly / 32767.0f;
}

}  // namespace input
