#pragma once

#include <furi.h>
#include <gui/icon.h>

typedef enum {
    NetrunnerInternalApplicationFlagDefault = 0,
    NetrunnerInternalApplicationFlagInsomniaSafe = (1 << 0),
} NetrunnerInternalApplicationFlag;

typedef struct {
    const FuriThreadCallback app;
    const char* name;
    const char* appid;
    const size_t stack_size;
    const Icon* icon;
    const NetrunnerInternalApplicationFlag flags;
} NetrunnerInternalApplication;

typedef struct {
    const char* name;
    const Icon* icon;
    const char* path;
} NetrunnerExternalApplication;

typedef void (*NetrunnerInternalOnStartHook)(void);

extern const char* NETRUNNER_AUTORUN_APP_NAME;

/* Services list
 * Spawned on startup
 */
extern const NetrunnerInternalApplication NETRUNNER_SERVICES[];
extern const size_t NETRUNNER_SERVICES_COUNT;

/* Apps list
 * Spawned by loader
 */
extern const NetrunnerInternalApplication NETRUNNER_APPS[];
extern const size_t NETRUNNER_APPS_COUNT;

/* On system start hooks
 * Called by loader, after OS initialization complete
 */
extern const NetrunnerInternalOnStartHook NETRUNNER_ON_SYSTEM_START[];
extern const size_t NETRUNNER_ON_SYSTEM_START_COUNT;

/* System apps
 * Can only be spawned by loader by name
 */
extern const NetrunnerInternalApplication NETRUNNER_SYSTEM_APPS[];
extern const size_t NETRUNNER_SYSTEM_APPS_COUNT;

/* Debug apps 
 * Can only be spawned by loader by name
 */
extern const NetrunnerInternalApplication NETRUNNER_DEBUG_APPS[];
extern const size_t NETRUNNER_DEBUG_APPS_COUNT;

extern const NetrunnerInternalApplication NETRUNNER_ARCHIVE;

/* Settings list
 * Spawned by loader
 */
extern const NetrunnerInternalApplication NETRUNNER_SETTINGS_APPS[];
extern const size_t NETRUNNER_SETTINGS_APPS_COUNT;

/* External Settings list
 * Spawned by loader
 */
extern const NetrunnerExternalApplication NETRUNNER_EXTSETTINGS_APPS[];
extern const size_t NETRUNNER_EXTSETTINGS_APPS_COUNT;

/* External Menu Apps list
 * Spawned by loader
 */
extern const NetrunnerExternalApplication NETRUNNER_EXTERNAL_APPS[];
extern const size_t NETRUNNER_EXTERNAL_APPS_COUNT;
