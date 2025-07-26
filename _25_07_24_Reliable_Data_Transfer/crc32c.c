#include "crc32c.h"

#define CRC32C_POLYNOMIAL 0x82F63B78

static uint32_t crc32c_table[256];

// init the search table
void crc32c_init() {
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 8; j > 0; j--) {
            if (crc & 1) {
                crc = (crc >> 1) ^ CRC32C_POLYNOMIAL;
            } else {
                crc >>= 1;
            }
        }
        crc32c_table[i] = crc;
    }
}

// calculate checksum
uint32_t crc32c(const void *data, size_t length) {
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; i++) {
        uint8_t byte = buf[i];
        uint32_t table_index = (crc ^ byte) & 0xFF;
        crc = (crc >> 8) ^ crc32c_table[table_index];
    }

    return ~crc;
}