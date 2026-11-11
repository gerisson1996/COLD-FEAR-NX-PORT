// The front end's black bands. Each menu page has a letterbox amount in its data (0.08 for the main menu), copied to
// [menu+0x50300]; sub_0022E8C0 (the menu update, thiscall) draws two black rectangles from the TV safe area edge
// (36 px) plus that share of the safe height, 68 px on a 480 line screen. On a CRT the overscan hid most of it; here the
// whole band shows. The menu is drawn without them; the gameplay letterbox (sub_0018E100, the camera's) is not this.
#include "xdk/hle.h"

using namespace kernel;

X86_FUNC(__imp__sub_0022E8C0);
X86_FUNC(sub_0022E8C0) {
    uint32_t amount = ctx.ecx + 0x50300;
    uint32_t saved = R32(amount);
    W32(amount, 0);
    __imp__sub_0022E8C0(ctx, base);
    W32(amount, saved);
}
