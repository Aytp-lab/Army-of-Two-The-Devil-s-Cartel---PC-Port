#pragma once
// Observer-only control, shared by the Main and GPU diagnostic builds.
// Touch the specified file to enable a bounded 30-second observation window.
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
namespace ao4_query_trace {
inline uint64_t Tick() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
inline uint64_t Frequency() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }
inline bool Active(uint64_t& window) {
  static const auto path = [] {
    struct Path { wchar_t value[4096]{}; } p;
    if (GetEnvironmentVariableW(L"AO4_QUERY_TRACE_CONTROL",p.value,4096)>=4096) p.value[0]=0;
    return p;
  }();
  if (!path.value[0]) return false;
  struct Cache { uint64_t check=0,window=0,count=0; bool active=false; };
  thread_local Cache c;
  uint64_t now=Tick();
  if (now>=c.check) {
    c.check=now+Frequency()/4;
    WIN32_FILE_ATTRIBUTE_DATA data{}; FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER utc{},modified{}; utc.LowPart=time.dwLowDateTime; utc.HighPart=time.dwHighDateTime;
    c.active=false;
    if (GetFileAttributesExW(path.value,GetFileExInfoStandard,&data)) {
      modified.LowPart=data.ftLastWriteTime.dwLowDateTime;
      modified.HighPart=data.ftLastWriteTime.dwHighDateTime;
      if(modified.QuadPart!=c.window) {c.window=modified.QuadPart;c.count=0;}
      c.active=utc.QuadPart>=modified.QuadPart && utc.QuadPart-modified.QuadPart<300000000ull;
    }
  }
  window=c.window;
  // At most 100,000 observed events per thread/window; not a gameplay limit.
  return c.active && ++c.count<=100000;
}
}
#else
namespace ao4_query_trace {
inline uint64_t Tick(){return 0;}
inline uint64_t Frequency(){return 0;}
inline bool Active(uint64_t&){return false;}
}
#endif
