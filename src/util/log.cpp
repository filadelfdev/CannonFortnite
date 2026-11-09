#include "util/log.h"
#include <windows.h>
#include <chrono>
#include <ctime>

namespace cf::log {

namespace {
Level g_min = Level::Info;
std::mutex g_lock;
const char* LevelTag(Level l){
    switch(l){
        case Level::Trace: return "trace";
        case Level::Info:  return "info ";
        case Level::Warn:  return "warn ";
        case Level::Error: return "err  ";
    }
    return "?";
}
}

void SetLevel(Level l){ g_min = l; }

void Line(Level l, const char* fmt, ...){
    if((int)l < (int)g_min) return;
    std::scoped_lock _(g_lock);
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[2048];
    int n = std::snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d %s] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, LevelTag(l));
    va_list ap; va_start(ap, fmt);
    n += std::vsnprintf(buf+n, sizeof(buf)-n, fmt, ap);
    va_end(ap);
    if(n<(int)sizeof(buf)-2){ buf[n++]='\n'; buf[n]=0; }
    OutputDebugStringA(buf);
    // Only to debug output — nothing lands on disk.
}

} // namespace cf::log
