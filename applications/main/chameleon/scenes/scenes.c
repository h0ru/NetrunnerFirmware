#include "scenes.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const chameleon_app_on_enter_handlers[])(void*) = {
#include "scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const chameleon_app_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const chameleon_app_on_exit_handlers[])(void* context) = {
#include "scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers chameleon_app_scene_handlers = {
    .on_enter_handlers = chameleon_app_on_enter_handlers,
    .on_event_handlers = chameleon_app_on_event_handlers,
    .on_exit_handlers = chameleon_app_on_exit_handlers,
    .scene_num = ChameleonAppSceneNum,
};
