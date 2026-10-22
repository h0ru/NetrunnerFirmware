#include <assets_icons.h>
#include <icon_i.h>

/* 14x14 menu icon: a small monitor with text lines (system-info). */
static const uint8_t netfetch_icon_frame_0[] = {
    0x00, 0x00, 0x00, 0xfe, 0x1f, 0x02, 0x10, 0xfa, 0x17, 0x02,
    0x10, 0xfa, 0x11, 0x02, 0x10, 0xfa, 0x17, 0x02, 0x10, 0xfe,
    0x1f, 0xc0, 0x00, 0xc0, 0x00, 0xf0, 0x03, 0x00, 0x00,
};

static const uint8_t* const netfetch_icon_frames[] = {
    netfetch_icon_frame_0,
};

const Icon A_Netfetch_14 = {
    .width = 14,
    .height = 14,
    .frame_count = 1,
    .frame_rate = 0,
    .frames = netfetch_icon_frames,
};
