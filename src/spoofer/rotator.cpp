#include "spoofer/rotator.h"
#include "antiban/jitter.h"
#include "util/log.h"

#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <ctime>

#pragma comment(lib, "bcrypt")

namespace cf::spoof::rot {

namespace {

std::filesystem::path LedgerPath(){
    wchar_t p[MAX_PATH]{};
    SHGetFolderPathW(nullptr, 0x001c /*CSIDL_LOCAL_APPDATA*/, nullptr, 0, p);
    std::filesystem::path out = p;
    out /= L"Microsoft\\Windows\\INetCache\\.svcledger.bin";
    std::filesystem::create_directories(out.parent_path());
    return out;
}

// AES-GCM key from (MachineGuid ⊕ ProductId ⊕ InstallDate) — a wiped
// machine can't decrypt, which is on purpose. Nothing recoverable off-box.
void DeriveKey(uint8_t key[32]){
    auto read = [](const char* path, const char* name){
        HKEY k=nullptr; char buf[256]{}; DWORD sz=sizeof(buf);
        if(RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ|KEY_WOW64_64KEY, &k)==0){
            RegQueryValueExA(k, name, nullptr, nullptr, (BYTE*)buf, &sz);
            RegCloseKey(k);
        }
        return std::string(buf);
    };
    std::string seed =
        read("SOFTWARE\\Microsoft\\Cryptography", "MachineGuid") +
        read("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductId") +
        read("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "InstallDate");

    BCRYPT_ALG_HANDLE alg=nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCRYPT_HASH_HANDLE h=nullptr;
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(h, (PUCHAR)seed.data(), (ULONG)seed.size(), 0);
    BCryptFinishHash(h, key, 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
}

bool AesGcmSeal(const std::vector<uint8_t>& in, std::vector<uint8_t>& out){
    uint8_t key[32]; DeriveKey(key);
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_KEY_HANDLE k=nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, key, 32, 0);

    uint8_t iv[12], tag[16];
    BCryptGenRandom(nullptr, iv, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce     = iv;  info.cbNonce = 12;
    info.pbTag       = tag; info.cbTag   = 16;

    out.assign(12 + in.size() + 16, 0);
    memcpy(out.data(), iv, 12);
    ULONG written = 0;
    NTSTATUS s = BCryptEncrypt(k, (PUCHAR)in.data(), (ULONG)in.size(), &info,
                               nullptr, 0, out.data()+12, (ULONG)in.size(), &written, 0);
    memcpy(out.data()+12+in.size(), tag, 16);

    BCryptDestroyKey(k); BCryptCloseAlgorithmProvider(alg, 0);
    return s == 0;
}

bool AesGcmOpen(const std::vector<uint8_t>& in, std::vector<uint8_t>& out){
    if(in.size() < 28) return false;
    uint8_t key[32]; DeriveKey(key);
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_KEY_HANDLE k=nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, key, 32, 0);

    size_t ctLen = in.size() - 12 - 16;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)in.data();          info.cbNonce = 12;
    info.pbTag   = (PUCHAR)(in.data()+12+ctLen); info.cbTag = 16;

    out.assign(ctLen, 0);
    ULONG written = 0;
    NTSTATUS s = BCryptDecrypt(k, (PUCHAR)(in.data()+12), (ULONG)ctLen, &info,
                               nullptr, 0, out.data(), (ULONG)ctLen, &written, 0);
    BCryptDestroyKey(k); BCryptCloseAlgorithmProvider(alg, 0);
    return s == 0;
}

void WriteStr(std::vector<uint8_t>& b, const std::string& s){
    uint32_t n = (uint32_t)s.size();
    b.insert(b.end(), (uint8_t*)&n, (uint8_t*)&n + 4);
    b.insert(b.end(), s.begin(), s.end());
}
std::string ReadStr(const uint8_t*& p, const uint8_t* end){
    if(p+4 > end) return {};
    uint32_t n; memcpy(&n, p, 4); p += 4;
    if(p+n > end) return {};
    std::string s((const char*)p, n); p += n;
    return s;
}

void Serialize(const std::vector<Slot>& ledger, std::vector<uint8_t>& out){
    uint32_t n = (uint32_t)ledger.size();
    out.insert(out.end(), (uint8_t*)&n, (uint8_t*)&n + 4);
    for(auto& s : ledger){
        out.insert(out.end(), (uint8_t*)&s.index, (uint8_t*)&s.index + 4);
        out.insert(out.end(), (uint8_t*)&s.lastUsedUnix, (uint8_t*)&s.lastUsedUnix + 8);
        uint8_t b = s.burned ? 1 : 0;
        out.push_back(b);
        WriteStr(out, s.fp.machineGuid);
        WriteStr(out, s.fp.hwProfileGuid);
        WriteStr(out, s.fp.productId);
        WriteStr(out, s.fp.installDate);
        WriteStr(out, s.fp.diskSerial);
        WriteStr(out, s.fp.volumeSerial);
        WriteStr(out, s.fp.smbiosUuid);
        uint32_t mc = (uint32_t)s.fp.macs.size();
        out.insert(out.end(), (uint8_t*)&mc, (uint8_t*)&mc + 4);
        for(auto& m : s.fp.macs) WriteStr(out, m);
    }
}

bool Deserialize(const std::vector<uint8_t>& in, std::vector<Slot>& out){
    if(in.size() < 4) return false;
    const uint8_t* p = in.data(); const uint8_t* e = p + in.size();
    uint32_t n; memcpy(&n, p, 4); p += 4;
    if(n > 1024) return false;
    out.clear(); out.reserve(n);
    for(uint32_t i=0;i<n;++i){
        Slot s{};
        if(p+13 > e) return false;
        memcpy(&s.index, p, 4);         p += 4;
        memcpy(&s.lastUsedUnix, p, 8);  p += 8;
        s.burned = (*p++) != 0;
        s.fp.machineGuid   = ReadStr(p, e);
        s.fp.hwProfileGuid = ReadStr(p, e);
        s.fp.productId     = ReadStr(p, e);
        s.fp.installDate   = ReadStr(p, e);
        s.fp.diskSerial    = ReadStr(p, e);
        s.fp.volumeSerial  = ReadStr(p, e);
        s.fp.smbiosUuid    = ReadStr(p, e);
        if(p+4 > e) return false;
        uint32_t mc; memcpy(&mc, p, 4); p += 4;
        for(uint32_t j=0;j<mc;++j) s.fp.macs.push_back(ReadStr(p, e));
        out.push_back(std::move(s));
    }
    return true;
}

thread_local int32_t g_currentSlotIndex = -1;

} // namespace

bool LoadLedger(std::vector<Slot>& out){
    auto path = LedgerPath();
    std::ifstream f(path, std::ios::binary);
    if(!f) return false;
    std::vector<uint8_t> enc((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint8_t> plain;
    if(!AesGcmOpen(enc, plain)) return false;
    return Deserialize(plain, out);
}

bool SaveLedger(const std::vector<Slot>& ledger){
    std::vector<uint8_t> plain, enc;
    Serialize(ledger, plain);
    if(!AesGcmSeal(plain, enc)) return false;
    auto path = LedgerPath();
    std::ofstream f(path, std::ios::binary|std::ios::trunc);
    if(!f) return false;
    f.write((const char*)enc.data(), enc.size());
    return true;
}

void Seed(std::vector<Slot>& ledger, int count){
    ledger.clear();
    ledger.reserve(count);
    for(int i=0;i<count;++i){
        Slot s{};
        s.index = (uint32_t)i;
        Randomize(s.fp);
        ledger.push_back(std::move(s));
    }
}

bool PickNext(std::vector<Slot>& ledger, Fingerprint& out){
    int bestIdx = -1;
    uint64_t oldest = ~0ULL;
    for(size_t i=0;i<ledger.size();++i){
        if(ledger[i].burned) continue;
        if(ledger[i].lastUsedUnix < oldest){
            oldest = ledger[i].lastUsedUnix;
            bestIdx = (int)i;
        }
    }
    if(bestIdx < 0) return false;
    g_currentSlotIndex = bestIdx;
    ledger[bestIdx].lastUsedUnix = (uint64_t)std::time(nullptr);
    out = ledger[bestIdx].fp;
    return true;
}

bool BurnCurrent(std::vector<Slot>& ledger){
    if(g_currentSlotIndex < 0 || g_currentSlotIndex >= (int)ledger.size()) return false;
    ledger[g_currentSlotIndex].burned = true;
    CF_WARN("slot %d burned", g_currentSlotIndex);
    return true;
}

int RemainingClean(const std::vector<Slot>& ledger){
    int n = 0;
    for(auto& s : ledger) if(!s.burned) ++n;
    return n;
}

} // namespace cf::spoof::rot
