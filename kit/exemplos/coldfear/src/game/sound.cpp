// Cold Fear's DirectSound calls the kit's sound code does not take over: their console code would run on the host's
// objects, which have another layout
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

// IDirectSoundStream::SetAllParameters(stream, DS3DBUFFER*, apply), .XTLID B0046: the 3D settings of a stream. The
// host mixes streams without them
X86_FUNC(sub_002CB243) {
    LOG_FIRST(1, "sound: stream SetAllParameters ignored");
    Return(ctx, 0, 3);
}

// IDirectSound_GetCaps(ds, DSCAPS*), .XTLID B003E: the console code reads the library's voice counts, which the host
// never fills, and Cold Fear sized its voice pool from them (no free voice, no sound). DSCAPS: free 2D voices, free 3D
// voices, free buffer SGEs, memory allocated; the console has 256 voices, 64 of them 3D
X86_FUNC(sub_002C95B7) {
    uint32_t caps = R32(ctx.esp + 8);
    if (caps) {
        W32(caps, 192);
        W32(caps + 4, 64);
        W32(caps + 8, 2047);
        W32(caps + 12, 0);
    }
    LOG_FIRST(1, "sound: GetCaps 192 2D and 64 3D voices");
    Return(ctx, 0, 2);
}

// .XTLID B0061 (buffer, 0, 0, 0) before a buffer plays: console code on the host's buffer, the host has nothing to set
X86_FUNC(sub_002CA2D0) {
    LOG_FIRST(1, "sound: buffer export B0061 ignored");
    Return(ctx, 0, 4);
}
