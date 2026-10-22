#pragma once

#include "settings_core.h"

#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETRUNNER_SETTINGS_PATH INT_PATH(".momentum_settings.txt")

extern NetrunnerSettings netrunner_settings;
extern uint32_t netrunner_settings_revision;

void netrunner_settings_load(void);
bool netrunner_settings_save(void);

#ifdef __cplusplus
}
#endif
