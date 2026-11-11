// Gyro aiming. sub_0017EBE0 (the player's controller on the stack) is the aim's update: it takes the right stick,
// already shaped and ramped by sub_0017FD60 (dead zone, half a second to full speed), and turns two angles of the
// controller in radians: +0xAD0 the turn asked of the body this tick, positive to the left, used up by the call, and
// +0xAD4 the aim's pitch, kept, positive up, eased back to its limits by the game. The gyro's turn is added to both
// before the call, so it moves the aim one to one with none of the stick's ramp. host-switch/gyro.cpp gives the turn
// as mouse counts while its mode's button is held (ZL, the aim, by port.cmake's PORT_GYRO_MODE) and has the
// settings: sensitivity, vertical share, axis, steadiness, drift correction, inverts.
#include <algorithm>
#include <cstring>

#include "common/mouse_keyboard.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr float kRadians = 0.05f * 0.0174532925f;  // a count is 0.05 degrees at sensitivity 1
constexpr uint32_t kTurn = 0xAD0;
constexpr uint32_t kPitch = 0xAD4;
constexpr uint32_t kCamera = 0xB74;    // -1: no camera, the update does nothing
constexpr float kPitchLimit = 0.407f;  // where the stick stops, up; the gyro pushes no further, either way

float F32(uint32_t a) {
    float f;
    std::memcpy(&f, HostPtr(a), 4);
    return f;
}
void WF(uint32_t a, float f) { std::memcpy(HostPtr(a), &f, 4); }
}  // namespace

X86_FUNC(__imp__sub_0017EBE0);
X86_FUNC(sub_0017EBE0) {
    uint32_t controller = R32(ctx.esp + 4);
    float dx, dy;
    if (controller && R32(controller + kCamera) != 0xFFFFFFFF && input::TakeMouse(dx, dy)) {
        WF(controller + kTurn, F32(controller + kTurn) - dx * kRadians);
        float pitch = F32(controller + kPitch);
        float wanted = pitch - dy * kRadians;
        // past the limit only back towards it
        if (wanted > kPitchLimit) wanted = std::max(pitch, kPitchLimit);
        if (wanted < -kPitchLimit) wanted = std::min(pitch, -kPitchLimit);
        WF(controller + kPitch, wanted);
    }
    __imp__sub_0017EBE0(ctx, base);
}
