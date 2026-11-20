#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/board/board_preview_probe.h"

static uint8_t packed[240 * 480];
static uint8_t padded[240 * 512];

int main(void)
{
    for (size_t y = 0; y < 240; y++) {
        for (size_t x = 0; x < 240; x++) {
            uint16_t c = (uint16_t)(y * 240 + x);
            packed[y * 480 + x * 2] = (uint8_t)c;
            packed[y * 480 + x * 2 + 1] = (uint8_t)(c >> 8);
        }
        memset(padded + y * 512, 0xa5, 512);
        memcpy(padded + y * 512, packed + y * 480, 480);
    }
    board_preview_probe_t a = board_preview_probe(packed, 480);
    board_preview_probe_t b = board_preview_probe(padded, 512);
    assert(a.hash == b.hash && a.rgb == b.rgb);
    padded[4 * 512 + 4 * 2] ^= 1;
    assert(board_preview_probe(padded, 512).hash != a.hash);
    memset(packed, 0xff, sizeof(packed));
    assert(board_preview_probe(packed, 480).rgb == 0xffffff);
    memset(packed, 0, sizeof(packed));
    assert(board_preview_probe(packed, 480).rgb == 0);
    for (size_t i = 0; i < sizeof(packed); i += 2) {
        packed[i] = 0xe0; packed[i + 1] = 0x07;
    }
    assert(board_preview_probe(packed, 480).rgb == 0x00ff00);
    assert(board_preview_pink_jump(0x808080, 0xff40ff));
    assert(!board_preview_pink_jump(0xff40ff, 0xff40ff));
    assert(!board_preview_pink_jump(0x808080, 0xc0c0c0));
    assert(!board_preview_pink_jump(0xff40ff, 0x808080));
    puts("preview probe: OK");
    return 0;
}
