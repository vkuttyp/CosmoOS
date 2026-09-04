/*
 * crc32c.c - Table-driven CRC-32C.
 */

#include <kernel/crc32c.h>

static uint32_t g_table[256];
static bool g_ready;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (unsigned k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
        g_table[i] = c;
    }
    g_ready = true;
}

uint32_t crc32c_update(uint32_t crc, const void *data, size_t len)
{
    if (!g_ready)
        build_table();
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = g_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

uint32_t crc32c(const void *data, size_t len)
{
    return crc32c_update(0, data, len);
}
