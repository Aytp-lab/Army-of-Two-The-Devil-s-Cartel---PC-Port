// v0.10.0 unload lookup compares an absolute VFS path with a relative manifest
// key. Finish dispatch cleanup only after the real last-reference unload.
// Original guest routine, kernel unload and return status are retained.
#include "generated/default/ao4_launcher_init.h"
#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_module.h>
#include <rex/system/user_module.h>
#include <rex/system/xtypes.h>

REX_HOOK_RAW(sub_82018A80) {
    auto* kernel = rex::system::KernelState::shared();
    const uint32_t handle = ctx.r3.u32;
    auto module = rex::system::XModule::GetFromHModule(
        kernel, kernel->memory()->TranslateVirtual(handle));
    const bool splash = module &&
        (module->name() == "AO4.Splash" || module->name() == "AO4.Splash.xex");
    __imp__sub_82018A80(ctx, base);
    if (!splash || ctx.r3.u32 != 1) return;
    const auto* loader = kernel->memory()->TranslateVirtual<rex::system::X_LDR_DATA_TABLE_ENTRY*>(handle);
    if (uint16_t(loader->load_count) != 0) return;
    auto range = kernel->function_dispatcher()->UnregisterModule("ao4.splash.xex");
    if (!range) return; // Normal SDK cleanup may already have completed.
    for (const char* name : {"xboxkrnl.exe", "xam.xex"}) {
        auto imported = kernel->GetModule(name);
        if (imported && imported->module_type() == rex::system::XModule::ModuleType::kKernelModule)
            static_cast<rex::system::KernelModule*>(imported.get())->InvalidateThunkCacheInRange(
                range->first, range->second);
    }
    REXLOG_INFO("AO4 unload: cleared Splash AOT dispatch after final guest unload");
}
