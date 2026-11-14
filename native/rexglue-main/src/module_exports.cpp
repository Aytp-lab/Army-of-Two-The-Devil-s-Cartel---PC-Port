// SDK module ABI adapter for the separately analysed Main entrypoint.
// Function table and layout are the real generated data, not substitute hooks.
#include "generated/ao4.main/ao4_main_init.h"
#include <rex/system/function_dispatcher.h>

void ao4_main_RegisterFunctions(rex::runtime::IModuleRegistrar*);

extern "C" __declspec(dllexport)
void ReXModule_Register(rex::runtime::IModuleRegistrar* registrar) {
    ao4_main_RegisterFunctions(registrar);
}

extern "C" __declspec(dllexport)
const rex::PPCImageInfo* ReXModule_GetImageInfo() { return &PPCImageConfig; }
