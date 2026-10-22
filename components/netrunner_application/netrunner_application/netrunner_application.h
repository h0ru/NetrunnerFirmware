/**
 * @file netrunner_application.h
 * Netrunner application
 */
#pragma once

#include "application_manifest.h"
#include "elf/elf_api_interface.h"

#include <furi.h>
#include <storage/storage.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NetrunnerApplicationPreloadStatusSuccess = 0,
    NetrunnerApplicationPreloadStatusInvalidFile,
    NetrunnerApplicationPreloadStatusNotEnoughMemory,
    NetrunnerApplicationPreloadStatusInvalidManifest,
    NetrunnerApplicationPreloadStatusApiTooOld,
    NetrunnerApplicationPreloadStatusApiTooNew,
    NetrunnerApplicationPreloadStatusTargetMismatch,
} NetrunnerApplicationPreloadStatus;

typedef enum {
    NetrunnerApplicationLoadStatusSuccess = 0,
    NetrunnerApplicationLoadStatusUnspecifiedError,
    NetrunnerApplicationLoadStatusMissingImports,
} NetrunnerApplicationLoadStatus;

const char* netrunner_application_preload_status_to_string(NetrunnerApplicationPreloadStatus status);
const char* netrunner_application_load_status_to_string(NetrunnerApplicationLoadStatus status);
typedef struct NetrunnerApplication NetrunnerApplication;

/* Optional ARM load diagnostic, valid until the application is freed. */
const char* netrunner_application_get_load_error(const NetrunnerApplication* app);

typedef struct {
    const char* name;
    uint32_t address;
} NetrunnerApplicationMemoryMapEntry;

typedef struct {
    uint32_t mmap_entry_count;
    NetrunnerApplicationMemoryMapEntry* mmap_entries;
    uint32_t debug_link_size;
    uint8_t* debug_link;
} NetrunnerApplicationState;

NetrunnerApplication*
    netrunner_application_alloc(Storage* storage, const ElfApiInterface* api_interface);
void netrunner_application_free(NetrunnerApplication* app);

NetrunnerApplicationPreloadStatus
    netrunner_application_preload(NetrunnerApplication* app, const char* path);
NetrunnerApplicationPreloadStatus
    netrunner_application_preload_manifest(NetrunnerApplication* app, const char* path);
const NetrunnerApplicationManifest* netrunner_application_get_manifest(NetrunnerApplication* app);
NetrunnerApplicationLoadStatus netrunner_application_map_to_memory(NetrunnerApplication* app);
FuriThread* netrunner_application_alloc_thread(NetrunnerApplication* app, const char* args);
bool netrunner_application_is_plugin(NetrunnerApplication* app);

typedef int32_t (*NetrunnerApplicationEntryPoint)(void*);

typedef struct {
    const char* appid;
    const uint32_t ep_api_version;
    const void* entry_point;
} NetrunnerAppPluginDescriptor;

typedef const NetrunnerAppPluginDescriptor* (*NetrunnerApplicationPluginEntryPoint)(void);

const NetrunnerAppPluginDescriptor*
    netrunner_application_plugin_get_descriptor(NetrunnerApplication* app);

bool netrunner_application_load_name_and_icon(
    FuriString* path,
    Storage* storage,
    uint8_t** icon_ptr,
    FuriString* item_name);

#ifdef __cplusplus
}
#endif
