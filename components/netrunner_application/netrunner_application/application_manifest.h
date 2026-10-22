/**
 * @file application_manifest.h
 * Netrunner application manifest
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "elf/elf_api_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAP_MANIFEST_MAGIC 0x52474448
#define FAP_MANIFEST_SUPPORTED_VERSION 1

#define FAP_MANIFEST_MAX_APP_NAME_LENGTH 32
#define FAP_MANIFEST_MAX_ICON_SIZE 32

#pragma pack(push, 1)

typedef struct {
    uint32_t manifest_magic;
    uint32_t manifest_version;
    union {
        struct {
            uint16_t minor;
            uint16_t major;
        };
        uint32_t version;
    } api_version;
    uint16_t hardware_target_id;
} NetrunnerApplicationManifestBase;

typedef struct {
    NetrunnerApplicationManifestBase base;
    uint16_t stack_size;
    uint32_t app_version;
    char name[FAP_MANIFEST_MAX_APP_NAME_LENGTH];
    char has_icon;
    char icon[FAP_MANIFEST_MAX_ICON_SIZE];
} NetrunnerApplicationManifestV1;

typedef NetrunnerApplicationManifestV1 NetrunnerApplicationManifest;

#pragma pack(pop)

bool netrunner_application_manifest_is_valid(const NetrunnerApplicationManifest* manifest);
bool netrunner_application_manifest_is_too_old(
    const NetrunnerApplicationManifest* manifest,
    const ElfApiInterface* api_interface);
bool netrunner_application_manifest_is_too_new(
    const NetrunnerApplicationManifest* manifest,
    const ElfApiInterface* api_interface);
bool netrunner_application_manifest_is_target_compatible(const NetrunnerApplicationManifest* manifest);

#ifdef __cplusplus
}
#endif
