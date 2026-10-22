#include "../fatfs.h"

#include <string.h>

FATFS fatfs_object;

void fatfs_init(void) {
    memset(&fatfs_object, 0, sizeof(fatfs_object));
}

/* Volume-label stubs, only for builds where FatFs itself does not provide them
 * (FF_USE_LABEL == 0). With exFAT enabled (tools/patch_fatfs_exfat.py) the
 * project turns on CONFIG_FATFS_USE_LABEL, so ff.c defines the real functions
 * and these stubs must step aside to avoid a multiple-definition link error. */
#if !FF_USE_LABEL
FRESULT f_getlabel(const TCHAR* path, TCHAR* label, DWORD* vsn) {
    (void)path;
    if(label != NULL) {
        label[0] = '\0';
    }
    if(vsn != NULL) {
        *vsn = 0;
    }
    return FR_OK;
}

FRESULT f_setlabel(const TCHAR* label) {
    (void)label;
    return FR_OK;
}
#endif
