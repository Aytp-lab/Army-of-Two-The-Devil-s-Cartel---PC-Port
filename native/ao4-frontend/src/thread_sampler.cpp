// ao4_thread_sampler <pid> <tid> <seconds> [top]
// Samples one thread's instruction pointer (~1 kHz) and prints the hottest functions
// (dbghelp symbols from the modules' PDBs). Diagnosis tool, not part of the package.
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) { std::fwprintf(stderr, L"usage: ao4_thread_sampler <pid> <tid> <seconds> [top]\n"); return 2; }
  const DWORD pid = wcstoul(argv[1], nullptr, 10), tid = wcstoul(argv[2], nullptr, 10);
  const int seconds = _wtoi(argv[3]), top = argc > 4 ? _wtoi(argv[4]) : 40;
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!process || !thread) { std::fwprintf(stderr, L"open failed %lu\n", GetLastError()); return 1; }
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  if (!SymInitialize(process, nullptr, TRUE)) { std::fwprintf(stderr, L"SymInitialize failed\n"); return 1; }
  timeBeginPeriod(1);
  std::map<DWORD64, unsigned> hits;
  std::map<std::string, unsigned> callers;  // first frame outside ntdll/kernel for ntdll samples
  auto name_of = [&](DWORD64 a) {
    alignas(SYMBOL_INFO) char b[sizeof(SYMBOL_INFO) + 256];
    auto* sy = reinterpret_cast<SYMBOL_INFO*>(b);
    sy->SizeOfStruct = sizeof(SYMBOL_INFO);
    sy->MaxNameLen = 255;
    DWORD64 d = 0;
    IMAGEHLP_MODULE64 mo{sizeof(mo)};
    std::string mod = SymGetModuleInfo64(process, a, &mo) ? mo.ModuleName : "?";
    return SymFromAddr(process, a, &d, sy) ? mod + "!" + sy->Name : mod + "!?";
  };
  unsigned samples = 0, running = 0;
  const DWORD end = GetTickCount() + DWORD(seconds) * 1000;
  while (GetTickCount() < end) {
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &c)) {
        ++hits[c.Rip];
        ++samples;
        IMAGEHLP_MODULE64 mo{sizeof(mo)};
        if (SymGetModuleInfo64(process, c.Rip, &mo) && _stricmp(mo.ModuleName, "ntdll") == 0) {
          STACKFRAME64 f{};
          f.AddrPC.Offset = c.Rip; f.AddrPC.Mode = AddrModeFlat;
          f.AddrFrame.Offset = c.Rbp; f.AddrFrame.Mode = AddrModeFlat;
          f.AddrStack.Offset = c.Rsp; f.AddrStack.Mode = AddrModeFlat;
          CONTEXT w = c;
          std::string chain;
          for (int depth = 0; depth < 24; ++depth) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &f, &w, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) || !f.AddrPC.Offset) break;
            IMAGEHLP_MODULE64 m2{sizeof(m2)};
            if (!SymGetModuleInfo64(process, f.AddrPC.Offset, &m2)) continue;
            if (_stricmp(m2.ModuleName, "ao4_main_native") == 0 || _stricmp(m2.ModuleName, "rexruntime") == 0) {
              chain += name_of(f.AddrPC.Offset);
              if (chain.size() > 300 || chain.find("__imp__") != std::string::npos) break;
              chain += " <- ";
            }
          }
          ++callers[chain.empty() ? std::string("(no main frame)") : chain];
        }
      }
      ResumeThread(thread);
    }
    Sleep(1);
  }
  timeEndPeriod(1);
  // Aggregate by function (symbol start) and by module.
  std::map<std::string, unsigned> by_func, by_module;
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
  for (const auto& [rip, n] : hits) {
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::string name;
    IMAGEHLP_MODULE64 mod{sizeof(mod)};
    const std::string module = SymGetModuleInfo64(process, rip, &mod) ? mod.ModuleName : "?";
    if (SymFromAddr(process, rip, &disp, sym)) name = module + "!" + sym->Name;
    else name = module + "!?";
    by_func[name] += n;
    by_module[module] += n;
  }
  std::vector<std::pair<unsigned, std::string>> f, m;
  for (auto& [k, v] : by_func) f.push_back({v, k});
  for (auto& [k, v] : by_module) m.push_back({v, k});
  std::sort(f.rbegin(), f.rend());
  std::sort(m.rbegin(), m.rend());
  std::printf("samples %u\n-- modules\n", samples);
  for (auto& [v, k] : m) std::printf("%6.1f%%  %s\n", 100.0 * v / std::max(1u, samples), k.c_str());
  std::printf("-- functions\n");
  for (int i = 0; i < top && i < int(f.size()); ++i)
    std::printf("%6.1f%%  %s\n", 100.0 * f[i].first / std::max(1u, samples), f[i].second.c_str());
  std::vector<std::pair<unsigned, std::string>> cv;
  for (auto& [k, v] : callers) cv.push_back({v, k});
  std::sort(cv.rbegin(), cv.rend());
  std::printf("-- ntdll samples by caller chain\n");
  for (int i = 0; i < 25 && i < int(cv.size()); ++i)
    std::printf("%6.1f%%  %s\n", 100.0 * cv[i].first / std::max(1u, samples), cv[i].second.c_str());
  (void)running;
  return 0;
}
