// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_MINIX_NATIVE_H
#define LINUX_DEFRAGGER_MINIX_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MinixSummary {
    uint16_t magic;
    uint32_t inode_count;
    uint32_t zone_count;
    uint16_t imap_blocks;
    uint16_t zmap_blocks;
    uint32_t first_data_zone;
    uint16_t log_zone_size;
    uint32_t max_size;
    uint32_t block_size;
    uint64_t zone_size;
    unsigned int version;
    bool long_names;
    bool little_endian;
} MinixSummary;

int minix_read_summary(const char *path, MinixSummary *summary,
                       char *error, size_t error_size);
bool minix_probe(const char *path);
const char *minix_variant_name(const MinixSummary *summary);
const char *minix_byte_order_name(const MinixSummary *summary);

#endif
