#pragma once
#include <netrunner_application/netrunner_application.h>

typedef struct ArmFapRuntime ArmFapRuntime;
NetrunnerApplicationPreloadStatus arm_fap_runtime_preload(
    Storage* storage, const char* path, NetrunnerApplicationManifest* manifest,
    bool full, ArmFapRuntime** runtime);
NetrunnerApplicationLoadStatus arm_fap_runtime_map(ArmFapRuntime* runtime);
const char* arm_fap_runtime_error(const ArmFapRuntime* runtime);
int32_t arm_fap_runtime_run(ArmFapRuntime* runtime);
void arm_fap_runtime_free(ArmFapRuntime* runtime);
