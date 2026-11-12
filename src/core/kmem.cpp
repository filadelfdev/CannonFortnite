#include "core/kmem.h"
#include "antiban/hash.h"
#include "util/log.h"

#include <windows.h>
#include <mutex>
#include <vector>

namespace cf::kmem {

namespace {

// IOCTLs match driver/kread.c via the shared header — zero drift.
#include "../../driver/cf_ioctl.h"
constexpr uint32_t IOCTL_CF_ARM   = CF_ARM;
constexpr uint32_t IOCTL_CF_READ  = CF_READ;
constexpr uint32_t IOCTL_CF_WRITE = CF_WRITE;
constexpr uint32_t IOCTL_CF_BATCH = CF_BATCH;

HANDLE          g_dev = INVALID_HANDLE_VALUE;
std::mutex      g_lock;
bool            g_armed = false;

struct ReqWire {
    uint64_t src;
    uint64_t dst;        // user-mode dest, driver MDL-maps it
    uint32_t size;
    uint32_t pad;
};

} // namespace

bool Open(){
    std::scoped_lock _(g_lock);
    if(g_dev != INVALID_HANDLE_VALUE) return true;
    // Driver publishes \\.\CfRead; symlink only, no kernel-mode device
    // namespace exposure. If the driver is not loaded this stays closed
    // and the user-mode memory.cpp path is used instead.
    g_dev = CreateFileW(L"\\\\.\\CfRead",
                        GENERIC_READ|GENERIC_WRITE,
                        FILE_SHARE_READ|FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    return g_dev != INVALID_HANDLE_VALUE;
}

void Close(){
    std::scoped_lock _(g_lock);
    if(g_dev != INVALID_HANDLE_VALUE){ CloseHandle(g_dev); g_dev = INVALID_HANDLE_VALUE; }
    g_armed = false;
}

bool Online(){ return g_dev != INVALID_HANDLE_VALUE && g_armed; }

bool Arm(uint64_t imageHashFnv1a){
    if(g_dev == INVALID_HANDLE_VALUE) return false;
    DWORD ret=0;
    bool ok = DeviceIoControl(g_dev, IOCTL_CF_ARM,
                              &imageHashFnv1a, sizeof(imageHashFnv1a),
                              nullptr, 0, &ret, nullptr) != 0;
    g_armed = ok;
    return ok;
}

bool Read(uintptr_t addr, void* out, size_t n){
    if(!Online()) return false;
    ReqWire r{(uint64_t)addr, (uint64_t)out, (uint32_t)n, 0};
    DWORD ret=0;
    return DeviceIoControl(g_dev, IOCTL_CF_READ, &r, sizeof(r),
                           nullptr, 0, &ret, nullptr) != 0;
}

bool Write(uintptr_t addr, const void* in, size_t n){
    if(!Online()) return false;
    ReqWire r{(uint64_t)addr, (uint64_t)in, (uint32_t)n, 0};
    DWORD ret=0;
    return DeviceIoControl(g_dev, IOCTL_CF_WRITE, &r, sizeof(r),
                           nullptr, 0, &ret, nullptr) != 0;
}

bool Batch(const Req* r, size_t count){
    if(!Online() || count==0) return false;
    std::vector<ReqWire> wire(count);
    for(size_t i=0;i<count;++i){
        wire[i] = {(uint64_t)r[i].src, (uint64_t)r[i].dst, (uint32_t)r[i].size, 0};
    }
    DWORD ret=0;
    return DeviceIoControl(g_dev, IOCTL_CF_BATCH,
                           wire.data(), (DWORD)(wire.size()*sizeof(ReqWire)),
                           nullptr, 0, &ret, nullptr) != 0;
}

} // namespace cf::kmem
