// Cold Fear's Direct3D calls the kit's d3d8.cpp does not take over: their console code programs the NV2A itself
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

// D3DDevice_Reset(D3DPRESENT_PARAMETERS*), 0x262CB0: the game calls it before each film with the boot's parameters
// and another presentation interval. The host's buffers and swap chain stay as they are
X86_FUNC(sub_00262CB0) {
    uint32_t pp = R32(ctx.esp + 4);
    LOG_FIRST(4, "d3d: Reset %ux%u interval %X kept as it is", R32(pp), R32(pp + 4), R32(pp + 0x34));
    Return(ctx, 0, 1);
}
