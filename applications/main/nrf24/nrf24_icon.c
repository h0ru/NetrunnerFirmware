#include <assets_icons.h>
#include <icon_i.h>

/* App menu glyph: the name "NRF" over "24" in a 3x5 micro-font (a 4-wide
 * diagonal N so it doesn't read as an H). 14x14, single frame, uncompressed
 * (leading 0x00 flag, then XBM rows, 2 bytes/row, LSB = leftmost pixel). */
static const uint8_t nrf24_icon_frame_0[] = {
    0x00, 0x00, 0x00, 0xd2, 0x1c, 0x56, 0x05, 0xda, 0x0c, 0x52,
    0x05, 0x52, 0x05, 0x00, 0x00, 0x00, 0x00, 0x98, 0x02, 0xa0,
    0x02, 0x90, 0x03, 0x08, 0x02, 0x38, 0x02, 0x00, 0x00,
};

static const uint8_t* const nrf24_icon_frames[] = {
    nrf24_icon_frame_0,
};

const Icon A_Nrf24_14 = {
    .width = 14,
    .height = 14,
    .frame_count = 1,
    .frame_rate = 0,
    .frames = nrf24_icon_frames,
};
