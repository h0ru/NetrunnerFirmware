#pragma once

#include <gui/view.h>

typedef struct PowerMenu PowerMenu;

typedef enum {
    PowerMenuActionSettings,
    PowerMenuActionReboot,
    PowerMenuActionPowerOff,
} PowerMenuAction;

/* Fired when the user confirms an option (center/right on the confirm prompt). */
typedef void (*PowerMenuCallback)(PowerMenuAction action, void* context);

PowerMenu* power_menu_alloc(void);
void power_menu_free(PowerMenu* power_menu);
View* power_menu_get_view(PowerMenu* power_menu);
void power_menu_set_callback(PowerMenu* power_menu, PowerMenuCallback callback, void* context);
/* Reset to the icon row with the default option (Reboot, the middle one) selected. */
void power_menu_reset(PowerMenu* power_menu);
