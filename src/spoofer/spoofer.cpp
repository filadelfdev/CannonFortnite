#include "spoofer/spoofer.h"
#include "antiban/hash.h"
#include "util/log.h"

#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "iphlpapi")
#pragma comment(lib, "setupapi")
#pragma comment(lib, "cfgmgr32")
#pragma comment(lib, "advapi32")

namespace cf::spoof {

namespace {

std::mt19937_64& Rng(){
    static std::mt19937_64 r{std::random_device{}()};
    return r;
}

std::string MakeGuid(){
    std::stringstream s;
    auto hex = [&](int n){
        s << std::hex << std::setfill('0');
        for(int i=0;i<n;++i) s << std::setw(2) << (Rng()&0xFF);
    };
    hex(4); s << '-'; hex(2); s << '-'; hex(2); s << '-'; hex(2); s << '-'; hex(6);
    return s.str();
}

std::string MakeHexSerial(int bytes){
    std::stringstream s;
    s << std::hex << std::uppercase << std::setfill('0');
    for(int i=0;i<bytes;++i) s << std::setw(2) << (Rng()&0xFF);
    return s.str();
}

std::string MakeMac(){
    // Local-admin OUI, random body.
    auto r=Rng();
    uint8_t m[6] = {
        0x02, (uint8_t)((r>>8)&0xFF), (uint8_t)((r>>16)&0xFF),
        (uint8_t)((r>>24)&0xFF), (uint8_t)((r>>32)&0xFF), (uint8_t)((r>>40)&0xFF)
    };
    std::stringstream s;
    s << std::hex << std::uppercase << std::setfill('0');
    for(int i=0;i<6;++i){ if(i) s<<'-'; s<<std::setw(2)<<(int)m[i]; }
    return s.str();
}

std::string RegRead(HKEY root, const char* path, const char* name){
    HKEY k=nullptr;
    if(RegOpenKeyExA(root, path, 0, KEY_READ|KEY_WOW64_64KEY, &k)) return {};
    char buf[512]{}; DWORD sz=sizeof(buf);
    RegQueryValueExA(k, name, nullptr, nullptr, (BYTE*)buf, &sz);
    RegCloseKey(k);
    return buf;
}

bool RegWrite(HKEY root, const char* path, const char* name, const std::string& value){
    HKEY k=nullptr;
    if(RegCreateKeyExA(root, path, 0, nullptr, 0,
                       KEY_WRITE|KEY_WOW64_64KEY, nullptr, &k, nullptr)) return false;
    bool ok = RegSetValueExA(k, name, 0, REG_SZ,
                             (const BYTE*)value.c_str(),
                             (DWORD)value.size()+1) == 0;
    RegCloseKey(k);
    return ok;
}

std::filesystem::path StashPath(){
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::filesystem::path p = tmp;
    // Scrambled name so a cursory look doesn't flag it.
    p /= L"svc_7f3a1c.bin";
    return p;
}

} // namespace

Fingerprint Read(){
    Fingerprint fp;
    fp.machineGuid   = RegRead(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid");
    fp.hwProfileGuid = RegRead(HKEY_LOCAL_MACHINE,
        "System\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001",
        "HwProfileGuid");
    fp.productId     = RegRead(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductId");
    fp.installDate   = RegRead(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "InstallDate");

    // Volume serial on C:
    DWORD vser=0;
    GetVolumeInformationW(L"C:\\", nullptr, 0, &vser, nullptr, nullptr, nullptr, 0);
    std::stringstream s; s<<std::hex<<std::uppercase<<vser;
    fp.volumeSerial = s.str();

    // Adapters
    ULONG sz=0; GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &sz);
    std::vector<BYTE> buf(sz);
    if(GetAdaptersAddresses(AF_UNSPEC, 0, nullptr,
        (IP_ADAPTER_ADDRESSES*)buf.data(), &sz)==NO_ERROR){
        for(auto a=(IP_ADAPTER_ADDRESSES*)buf.data(); a; a=a->Next){
            if(a->PhysicalAddressLength != 6) continue;
            std::stringstream ms;
            ms<<std::hex<<std::uppercase<<std::setfill('0');
            for(int i=0;i<6;++i){ if(i) ms<<'-'; ms<<std::setw(2)<<(int)a->PhysicalAddress[i]; }
            fp.macs.push_back(ms.str());
        }
    }

    return fp;
}

bool Randomize(Fingerprint& out){
    out.machineGuid   = MakeGuid();
    out.hwProfileGuid = "{" + MakeGuid() + "}";
    out.productId     = "00330-80000-00000-" + MakeHexSerial(2);
    out.installDate   = std::to_string((uint32_t)(Rng() & 0x7FFFFFFF));
    out.diskSerial    = MakeHexSerial(10);
    out.volumeSerial  = MakeHexSerial(4);
    out.macs.clear();
    out.macs.push_back(MakeMac());
    return true;
}

bool Apply(const Fingerprint& fp){
    bool ok = true;
    ok &= RegWrite(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
                   "MachineGuid", fp.machineGuid);
    ok &= RegWrite(HKEY_LOCAL_MACHINE,
                   "System\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001",
                   "HwProfileGuid", fp.hwProfileGuid);
    ok &= RegWrite(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                   "ProductId", fp.productId);
    ok &= RegWrite(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                   "InstallDate", fp.installDate);
    // MAC swap goes through SetupDiSetDeviceRegistryProperty — needs
    // admin + a NIC restart. The driver does the burned-in level; here
    // we only update the active address via the registry override.
    if(!fp.macs.empty()){
        HKEY k=nullptr;
        if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}",
            0, KEY_READ, &k)==0){
            for(DWORD i=0; ;++i){
                char sub[16]; DWORD sz=16;
                if(RegEnumKeyExA(k, i, sub, &sz, nullptr, nullptr, nullptr, nullptr)) break;
                std::string path = std::string(
                    "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\") + sub;
                std::string mac = fp.macs[0];
                std::string stripped;
                for(char c : mac) if(c!='-') stripped.push_back(c);
                RegWrite(HKEY_LOCAL_MACHINE, path.c_str(), "NetworkAddress", stripped);
            }
            RegCloseKey(k);
        }
    }
    return ok;
}

bool ApplyKernel(const Fingerprint& fp){
    HANDLE h = CreateFileW(L"\\\\.\\CfSpoof", GENERIC_WRITE,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if(h==INVALID_HANDLE_VALUE) return false;

    struct Payload {
        char smbiosUuid[64];
        char diskSerial [64];
        char mac[6];
    } p{};
    strncpy_s(p.smbiosUuid, fp.smbiosUuid.c_str(), sizeof(p.smbiosUuid)-1);
    strncpy_s(p.diskSerial, fp.diskSerial.c_str(), sizeof(p.diskSerial)-1);
    if(!fp.macs.empty()){
        auto& m = fp.macs[0];
        int o=0;
        for(size_t i=0;i<m.size() && o<6;++i){
            if(m[i]=='-' || m[i]==':') continue;
            char hx[3]{m[i], m[i+1], 0};
            p.mac[o++] = (char)std::strtoul(hx, nullptr, 16);
            ++i;
        }
    }

    DWORD ret=0;
    bool ok = DeviceIoControl(h, 0x800, &p, sizeof(p), nullptr, 0, &ret, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

bool StashReal(){
    Fingerprint real = Read();
    auto path = StashPath();
    std::ofstream f(path, std::ios::binary|std::ios::trunc);
    if(!f) return false;
    auto put = [&](const std::string& s){
        uint32_t n=(uint32_t)s.size(); f.write((char*)&n,4); f.write(s.data(),n);
    };
    put(real.machineGuid); put(real.hwProfileGuid); put(real.productId);
    put(real.installDate); put(real.volumeSerial);
    uint32_t mc=(uint32_t)real.macs.size(); f.write((char*)&mc,4);
    for(auto& m : real.macs) put(m);
    return true;
}

bool RestoreReal(){
    auto path = StashPath();
    std::ifstream f(path, std::ios::binary);
    if(!f) return false;
    auto get = [&](){ uint32_t n=0; f.read((char*)&n,4); std::string s(n,'\0'); f.read(s.data(),n); return s; };
    Fingerprint fp;
    fp.machineGuid=get(); fp.hwProfileGuid=get(); fp.productId=get();
    fp.installDate=get(); fp.volumeSerial=get();
    uint32_t mc=0; f.read((char*)&mc,4);
    for(uint32_t i=0;i<mc;++i) fp.macs.push_back(get());
    return Apply(fp);
}

} // namespace cf::spoof
