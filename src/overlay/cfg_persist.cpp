#include "overlay/cfg_persist.h"

#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <cstdint>

namespace cf::ov::persist {

namespace {

constexpr uint32_t kMagic   = 0xCF4B4559;   // "CFKE" in little-endian
constexpr uint32_t kVersion = 1;

std::filesystem::path PrefsPath(){
    wchar_t p[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p);
    std::filesystem::path out = p;
    out /= L"Microsoft\\Windows\\INetCache\\.svcprefs.bin";
    std::filesystem::create_directories(out.parent_path());
    return out;
}

struct Blob {
    uint32_t magic;
    uint32_t version;
    int32_t  menu;
    int32_t  panic;
    int32_t  cannon;
    int32_t  speed;
    int32_t  esp;
};

} // namespace

void LoadHotkeys(OverlayCfg& cfg){
    std::ifstream f(PrefsPath(), std::ios::binary);
    if(!f) return;
    Blob b{};
    f.read((char*)&b, sizeof(b));
    if(!f || b.magic != kMagic || b.version != kVersion) return;
    // Sanity — reject nonsense scancodes.
    auto ok = [](int k){ return k > 0 && k <= 0xFF; };
    if(!ok(b.menu) || !ok(b.panic) || !ok(b.cannon) || !ok(b.speed) || !ok(b.esp))
        return;
    cfg.menuKey.store(b.menu);
    cfg.panicKey.store(b.panic);
    cfg.toggleCannonKey.store(b.cannon);
    cfg.toggleSpeedKey.store(b.speed);
    cfg.toggleEspKey.store(b.esp);
}

void SaveHotkeys(const OverlayCfg& cfg){
    std::ofstream f(PrefsPath(), std::ios::binary|std::ios::trunc);
    if(!f) return;
    Blob b{
        kMagic, kVersion,
        cfg.menuKey.load(),
        cfg.panicKey.load(),
        cfg.toggleCannonKey.load(),
        cfg.toggleSpeedKey.load(),
        cfg.toggleEspKey.load(),
    };
    f.write((const char*)&b, sizeof(b));
}

} // namespace cf::ov::persist
