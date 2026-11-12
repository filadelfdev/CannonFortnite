#include "marker.h"

#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <ctime>

namespace cf::spoofer::marker {

namespace {

constexpr uint32_t kMagic   = 0xCF50F000;
constexpr uint32_t kVersion = 1;

std::filesystem::path MarkerPath(){
    wchar_t p[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p);
    std::filesystem::path out = p;
    out /= L"Microsoft\\Windows\\INetCache\\.cf_spoof.marker";
    std::filesystem::create_directories(out.parent_path());
    return out;
}

} // namespace

bool Write(int32_t slot, uint64_t fpHash){
    Blob b{};
    b.magic       = kMagic;
    b.version     = kVersion;
    b.slot        = slot;
    b.appliedUnix = (int64_t)std::time(nullptr);
    b.fpHash      = fpHash;
    std::ofstream f(MarkerPath(), std::ios::binary|std::ios::trunc);
    if(!f) return false;
    f.write((const char*)&b, sizeof(b));
    return (bool)f;
}

bool Read(Blob& out){
    std::ifstream f(MarkerPath(), std::ios::binary);
    if(!f) return false;
    f.read((char*)&out, sizeof(out));
    if(!f) return false;
    return out.magic == kMagic && out.version == kVersion;
}

bool Delete(){
    std::error_code ec;
    return std::filesystem::remove(MarkerPath(), ec);
}

bool Exists(){
    Blob b{};
    return Read(b);
}

double AgeHours(){
    Blob b{};
    if(!Read(b)) return -1.0;
    int64_t now = (int64_t)std::time(nullptr);
    return (now - b.appliedUnix) / 3600.0;
}

} // namespace cf::spoofer::marker
