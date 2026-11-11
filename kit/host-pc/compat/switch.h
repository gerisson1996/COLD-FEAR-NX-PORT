#pragma once
// The PC build (host-pc): the libnx names the shared Switch sources use (ui.cpp, vulkan.cpp's debug keys), with
// libnx's values. input.cpp fills them from the keyboard and XInput.
#include <cstdint>

#ifndef BITL
#define BITL(n) (1ull << (n))
#endif

enum HidNpadButton : uint64_t {
    HidNpadButton_A = BITL(0),
    HidNpadButton_B = BITL(1),
    HidNpadButton_X = BITL(2),
    HidNpadButton_Y = BITL(3),
    HidNpadButton_StickL = BITL(4),
    HidNpadButton_StickR = BITL(5),
    HidNpadButton_L = BITL(6),
    HidNpadButton_R = BITL(7),
    HidNpadButton_ZL = BITL(8),
    HidNpadButton_ZR = BITL(9),
    HidNpadButton_Plus = BITL(10),
    HidNpadButton_Minus = BITL(11),
    HidNpadButton_Left = BITL(12),
    HidNpadButton_Up = BITL(13),
    HidNpadButton_Right = BITL(14),
    HidNpadButton_Down = BITL(15),
};
