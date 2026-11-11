// The OLED model's screen. Its blacks are black and its darkest steps sit close together, so Cold Fear's night decks
// lose what an LCD's backlight showed. game::OledBoost says when the brightness ramp (src/d3d/d3d8.cpp, SendGamma)
// lifts the darks further: the option on, or on AUTO an OLED console out of the dock (docked, the TV shows the frame).
#include "common/options.h"
#include "core/log.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace game {

bool OledBoost() {
    uint32_t mode = Opt().oled;
    if (mode) return mode == 2;
#ifdef __SWITCH__
    static const bool oled = [] {
        SetSysProductModel model = SetSysProductModel_Invalid;
        if (R_SUCCEEDED(setsysInitialize())) {
            setsysGetProductModel(&model);
            setsysExit();
        }
        LOGI("oled: product model %d", int(model));
        return model == SetSysProductModel_Aula;
    }();
    return oled && appletGetOperationMode() == AppletOperationMode_Handheld;
#else
    return false;
#endif
}

}  // namespace game
