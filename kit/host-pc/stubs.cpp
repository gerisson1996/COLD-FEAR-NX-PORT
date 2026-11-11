// What the PC build has not: touch controls, the gyro, the keyboard binds of the PC ports, the update check. The
// same neutral answers as host-switch/stubs.cpp and the Switch's gyro off.
#include <string>

#include "common/mouse_keyboard.h"
#include "core/input.h"
#include "host.h"

namespace host {
void CheckForUpdates() {}
}  // namespace host

namespace touch {
void Apply(int, input::Pad&) {}
void Draw() {}
bool Shown() { return false; }
void AddLook(float, float) {}
bool LookActive() { return false; }
bool Firing() { return false; }
void PadUsed() {}
const char* LayoutName() { return "-"; }
void StepLayout(int) {}
uint32_t ControlsOpacity() { return 0; }
void StepOpacity(int) {}
uint32_t ControlsSize() { return 0; }
void StepSize(int) {}
void EditLayout() {}
}  // namespace touch

namespace gyro {
uint32_t CurrentMode() { return kOff; }
void SetMode(uint32_t) {}
const char* ModeName() { return "OFF"; }
void NextMode() {}
float Sensitivity() { return 1.0f; }
void SetSensitivity(float) {}
bool InvertX() { return false; }
bool InvertY() { return false; }
void SetInvert(bool, bool) {}
float Vertical() { return 1.0f; }
void SetVertical(float) {}
uint32_t Axis() { return kTurn; }
void SetAxis(uint32_t) {}
float Steadiness() { return 0; }
void SetSteadiness(float) {}
bool DriftFix() { return false; }
void SetDriftFix(bool) {}
uint32_t AimAssist() { return 0; }
void SetAimAssist(uint32_t) {}
}  // namespace gyro

namespace input {
void ReleaseCursor() {}
void Keyboard(Pad&) {}
const Action* Actions(size_t* count) {
    if (count) *count = 0;
    return nullptr;
}
std::string Binding(size_t) { return {}; }
void Bind(size_t, int, int) {}
void Unbind(size_t) {}
bool TakeDeletePress() { return false; }
void ResetBindings() {}
void LoadBindings() {}
int PressedKey() { return 0; }
bool PanelKey(int) { return false; }
bool HudKey(int) { return false; }
std::string ButtonKey(int) { return {}; }
bool TouchActive() { return false; }
std::string TouchName(int) { return {}; }
void SeenPad(const Pad&) {}
void SetMenu(bool) {}
bool KeyboardActive() { return false; }
bool CursorShown() { return false; }
bool Cursor(float&, float&, bool&) { return false; }
int TakeWheel() { return 0; }
bool LeftButton() { return false; }
void TapConfirm() {}
void SetTextEntry(bool) {}
void OnChar(unsigned) {}
std::u16string TakeChars() { return {}; }
}  // namespace input

namespace input {
bool GyroAiming() { return false; }
bool TakeMouse(float&, float&) { return false; }
}  // namespace input

// the handheld screen's shadows (host-switch/screen.cpp): the PC's monitor is left alone
namespace screen {
float HandheldGamma() { return 1.0f; }
void SetHandheldGamma(float) {}
float Gamma() { return 1.0f; }
}  // namespace screen
