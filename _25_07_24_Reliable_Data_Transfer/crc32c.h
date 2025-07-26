#ifndef CRC32C_H
#define CRC32C_H

#include <stdint.h>
#include <stddef.h>

void crc32c_init();

uint32_t crc32c(const void *data, size_t length);

#endif  // CRC32C_H