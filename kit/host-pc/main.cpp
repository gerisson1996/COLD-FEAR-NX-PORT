// The PC host (Windows): the same game and renderer as the Switch build (host-switch/vulkan.cpp on the PC's Vulkan),
// in a Win32 window, to find and fix the Switch's bugs on the PC. The game is described by app::kGame.
//
// coldfear_pc.exe [game folder] [--docked]
//   game folder: the disc's files (default.xbe and its folders); without it <exe>/game or the env XBE_GAME_DIR
//   --docked: a 1920x1080 window, the Switch docked; 1280x720 (handheld) without it
// Files beside the exe while it runs (for scripted tests):
//   cmd   "BUTTONS [frames]" presses those buttons (input.cpp), then is deleted
//   shot  saves the next frame's back buffer as shots/NNNN.bmp, then is deleted
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

HWND g_hwnd = nullptr;
bool g_docked = false;
std::atomic<uint32_t> g_surface_epoch{0};
std::filesystem::path g_exe_dir;
std::filesystem::path g_game_arg;

app::Mode ScreenMode() { return g_docked ? app::Mode{1920, 1080} : app::Mode{1280, 720}; }

std::filesystem::path FindExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

}  // namespace

namespace host {

void* Window() { return g_hwnd; }
bool Foreground() { return true; }
uint32_t SurfaceEpoch() { return g_surface_epoch; }
void WaitForeground() {}

void WindowPixels(int& w, int& h) {
    app::Mode m = ScreenMode();
    w = int(m.width);
    h = int(m.height);
}

const std::filesystem::path& GameArg() { return g_game_arg; }

// "shot" beside the exe: the next frame is saved
bool TakeShot() {
    std::error_code ec;
    std::filesystem::path p = g_exe_dir / "shot";
    if (!std::filesystem::exists(p, ec)) return false;
    std::filesystem::remove(p, ec);
    return true;
}

void SaveShot(const uint8_t* bgra, uint32_t w, uint32_t h) {
    static uint32_t n = 0;
    std::error_code ec;
    std::filesystem::create_directories(g_exe_dir / "shots", ec);
    char name[32];
    std::snprintf(name, sizeof(name), "%04u.bmp", n++);
    std::ofstream f(g_exe_dir / "shots" / name, std::ios::binary);
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    uint32_t bytes = w * h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + bytes;
    ih.biSize = sizeof(ih);
    ih.biWidth = LONG(w);
    ih.biHeight = -LONG(h);  // top down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    // opaque: the guest's alpha is not the picture's
    std::vector<uint8_t> px(bgra, bgra + bytes);
    for (uint32_t i = 3; i < bytes; i += 4) px[i] = 0xFF;
    f.write(reinterpret_cast<const char*>(px.data()), bytes);
    LOGI("pc: shot %s (%ux%u)", name, w, h);
}

}  // namespace host

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// as the Switch: 30 frames a second, vsync on, the frame at the screen's height
Settings DefaultSettings() {
    Settings s;
    s.render_percent = 100;
    s.fps_cap = 30;
    s.vsync = true;
    return s;
}

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s, PC", kGame.title, IMAGE_XBE, date, PORT_BUILD);
    LOGR("Path: %s", ExeDir().string().c_str());
    Mode desk = Desktop();
    LOGR("Display: %ux%u (%s)", desk.width, desk.height, g_docked ? "as docked" : "as handheld");
    LogSettings();
}

void ApplyDisplay() {}

Mode Desktop() { return ScreenMode(); }

Mode Output() { return Desktop(); }

bool Widescreen() {
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

std::filesystem::path ExeDir() { return g_exe_dir; }

int Instance() { return 0; }

void* Window() { return g_hwnd; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

namespace {
std::atomic<uint32_t> g_hold_fps{0};
std::atomic<int64_t> g_hold_until{0};  // steady clock ticks
}  // namespace

void HoldFrameRate(uint32_t fps) {
    g_hold_fps = fps;
    g_hold_until = (std::chrono::steady_clock::now() + std::chrono::milliseconds(250)).time_since_epoch().count();
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (uint32_t hold = g_hold_fps; hold && now.time_since_epoch().count() < g_hold_until) cap = hold;
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

void InitUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "pc_win32";
}

// a crash: where on the host, and the guest's stack when it happened on a guest thread
LONG WINAPI Crash(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    HMODULE exe = GetModuleHandleW(nullptr);
    uintptr_t pc = uintptr_t(er->ExceptionAddress);
    uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    uint64_t fault = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    LOGE("crash: exception %08lX at %p (exe+%llX), address %016llX", er->ExceptionCode, er->ExceptionAddress,
         (unsigned long long)(pc - uintptr_t(exe)), (unsigned long long)fault);
    // the function's name from the PDB, when lld wrote one
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(proc, nullptr, TRUE)) {
        alignas(8) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, pc, &disp, si)) LOGE("crash: in %s+%llX", si->Name, (unsigned long long)disp);
    }
    if (base && fault >= base && fault < base + (1ull << 32)) LOGE("crash: guest address %08X", uint32_t(fault - base));
    LOGE("crash: rax %016llX rbx %016llX rcx %016llX rdx %016llX", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOGE("crash: rsi %016llX rdi %016llX rsp %016llX rbp %016llX", c->Rsi, c->Rdi, c->Rsp, c->Rbp);
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("crash: on guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 256);
    }
    logx::Flush();
    TerminateProcess(proc, 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        LOGI("pc: window closed");
        logx::Flush();
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_CHAR:
        if (ImGui::GetCurrentContext() && wp < 0x10000) {
            std::lock_guard lock(app::UiLock());
            ImGui::GetIO().AddInputCharacter(unsigned(wp));
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND MakeWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"XboxSwitchPortKitPC";
    RegisterClassW(&wc);
    app::Mode m = ScreenMode();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, LONG(m.width), LONG(m.height)};
    AdjustWindowRect(&r, style, FALSE);
    std::wstring title = std::wstring(app::kGame.title) + L" (PC test build " + std::wstring(PORT_BUILD, PORT_BUILD + std::strlen(PORT_BUILD)) + L")";
    HWND h = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style, 40, 40, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

void Fatal(const char* text) {
    LOGE("%s", text);
    logx::Flush();
    MessageBoxA(nullptr, text, PORT_TITLE, MB_OK | MB_ICONERROR);
}

}  // namespace

int main(int argc, char** argv) {
    g_exe_dir = FindExeDir();
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--docked")) g_docked = true;
        else g_game_arg = argv[i];
    }
    SetUnhandledExceptionFilter(Crash);
    std::set_terminate([] {
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
            LOGE("crash: std::terminate (abort, no exception)");
        } catch (const std::system_error& e) {
            LOGE("crash: uncaught std::system_error %d: %s", e.code().value(), e.what());
        } catch (const std::exception& e) {
            LOGE("crash: uncaught exception: %s", e.what());
        } catch (...) {
            LOGE("crash: uncaught exception of an unknown type");
        }
        logx::Flush();
        TerminateProcess(GetCurrentProcess(), 3);
    });
    timeBeginPeriod(1);
    std::error_code ec;
    std::filesystem::create_directories(g_exe_dir / "logs", ec);
    std::freopen((g_exe_dir / "logs" / "stderr.log").string().c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    logx::Init((g_exe_dir / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) {
        Fatal(PORT_TITLE ": the game files were not found. Pass the folder with default.xbe on the command line.");
        return 1;
    }

    g_hwnd = MakeWindow();
    LOGI("app: ImGui");
    InitUi();
    LOGI("app: controllers");
    input::Init();
    LOGI("app: game window hook");
    if (app::kGame.window) app::kGame.window(g_hwnd);
    LOGI("app: booting the image");
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        Fatal(PORT_TITLE ": the game failed to start, see the log.");
        return 1;
    }

    auto alive = std::chrono::steady_clock::now();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (std::chrono::steady_clock::now() - alive > std::chrono::seconds(5)) {
            alive = std::chrono::steady_clock::now();
            LOGI("app: alive");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
