// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_APFS_NATIVE_H
#define LD_APFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t block_size;
    uint64_t block_count;
    uint8_t container_uuid[16];
} ApfsSummary;

int apfs_read_summary(const char *path, ApfsSummary *summary,
                      char *error, size_t error_size);
bool apfs_probe(const char *path);

#endif
