// The PC build's game files: the folder on the command line, the env XBE_GAME_DIR or <exe>/game, holding the
// disc's files with default.xbe. The xbe's SHA-1 is checked against the one recompiled; another is logged, then used.
#include <cstdlib>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"

extern const char kImageSha1[];

namespace host {
const std::filesystem::path& GameArg();
}

namespace app {

namespace {

namespace fs = std::filesystem;

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

}  // namespace

bool FindGame(GameFiles& game) {
    std::vector<fs::path> dirs;
    if (!host::GameArg().empty()) dirs.push_back(host::GameArg());
    if (const char* e = std::getenv("XBE_GAME_DIR")) dirs.push_back(e);
    dirs.push_back(ExeDir() / "game");
    for (const fs::path& d : dirs) {
        std::ifstream f(d / IMAGE_XBE, std::ios::binary);
        if (!f) continue;
        std::vector<uint8_t> xbe((std::istreambuf_iterator<char>(f)), {});
        std::string sha = Sha1(xbe);
        bool match = sha == kImageSha1;
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, d.string().c_str(), sha.c_str(), match ? "" : ", another release");
        game = {d, std::move(xbe)};
        return true;
    }
    LOGE("game: no %s found", IMAGE_XBE);
    return false;
}

}  // namespace app
