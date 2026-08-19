// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_UFS_NATIVE_H
#define LD_UFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LD_UFS_VARIANT_UFS1_LE = 1,
    LD_UFS_VARIANT_UFS1_BE = 2,
    LD_UFS_VARIANT_UFS2_LE = 3,
    LD_UFS_VARIANT_UFS2_BE = 4,
} LdUfsVariant;

typedef struct {
    LdUfsVariant variant;
    uint64_t superblock_offset;
    uint64_t magic_offset;
    bool allocation_totals_known;
    uint32_t block_size;
    uint32_t fragment_size;
    uint32_t fragments_per_block;
    uint64_t data_fragments;
    uint64_t free_blocks;
    uint64_t free_fragments;
} LdUfsSummary;

int ufs_read_summary(const char *path, LdUfsSummary *summary,
                     char *error, size_t error_size);
bool ufs_probe(const char *path);
const char *ufs_variant_name(const LdUfsSummary *summary);
const char *ufs_byte_order_name(const LdUfsSummary *summary);
unsigned int ufs_version(const LdUfsSummary *summary);
uint64_t ufs_recorded_data_bytes(const LdUfsSummary *summary);
uint64_t ufs_recorded_free_bytes(const LdUfsSummary *summary);
uint64_t ufs_recorded_used_bytes(const LdUfsSummary *summary);

#endif
