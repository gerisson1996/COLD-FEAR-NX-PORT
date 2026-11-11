// Cold Fear: what src/common needs from the game. No hooks on the game's functions yet.
#include <string>

#include "app/app.h"
#include "common/options.h"

namespace game {

// the game picks its language from the console's settings: the menu's row offers English only
bool HasLanguages() { return false; }
bool LanguageAvailable(const std::string& code) { return code == "int"; }
bool HudShown() { return true; }

}  // namespace game

// The menu draws black bands top and bottom when the video flags say 480p; the old port reported widescreen alone
// and its menu filled the screen
namespace {
const bool g_no_480p = (app::g_video_480p = false, true);
}
