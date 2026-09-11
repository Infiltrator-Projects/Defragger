// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "sfs_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SFS_TM_BLOCK_SIZE 4096U
#define SFS_TM_BLOCKS 16384U
#define SFS_TM_BITMAP 1U
#define SFS_TM_ADMIN 2U
#define SFS_TM_EXTENTS 3U
#define SFS_TM_OBJECTS 4U
#define SFS_TM_OBJECT_NODES 5U
#define SFS_TM_RESERVED 16U
#define SFS_TM_FILE_ID 10U
#define SFS_TM_FRAGMENTS 100U
#define SFS_TM_CHUNK_BLOCKS 64U
#define SFS_TM_CHUNK_KIB 256U
#define SFS_TM_LOW_START 128U
#define SFS_TM_LOW_STRIDE 96U
#define SFS_TM_LAST_START 16200U
#define SFS_TM_DATA_BLOCKS (SFS_TM_FRAGMENTS * SFS_TM_CHUNK_BLOCKS)
#define SFS_TM_FILE_BYTES (SFS_TM_DATA_BLOCKS * SFS_TM_BLOCK_SIZE)

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void stamp_checksum(uint8_t *block) {
    uint32_t sum = 1U;
    put32(block + 4U, 0U);
    for (uint32_t offset = 0U; offset < SFS_TM_BLOCK_SIZE; offset += 4U)
        sum += get32(block + offset);
    put32(block + 4U, 0U - sum);
}

static void set_header(uint8_t *block, const char id[4], uint32_t own_block) {
    memcpy(block, id, 4U);
    put32(block + 8U, own_block);
}

static uint32_t fragment_start(uint32_t index) {
    return index + 1U == SFS_TM_FRAGMENTS
        ? SFS_TM_LAST_START
        : SFS_TM_LOW_START + index * SFS_TM_LOW_STRIDE;
}

static void make_root(uint8_t *block, uint32_t own_block, uint16_t sequence) {
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "SFS\0", own_block);
    put16(block + 12U, 3U);
    put16(block + 14U, sequence);
    put32(block + 48U, SFS_TM_BLOCKS);
    put32(block + 52U, SFS_TM_BLOCK_SIZE);
    put32(block + 96U, SFS_TM_BITMAP);
    put32(block + 100U, SFS_TM_ADMIN);
    put32(block + 104U, SFS_TM_OBJECTS);
    put32(block + 108U, SFS_TM_EXTENTS);
    put32(block + 112U, SFS_TM_OBJECT_NODES);
    stamp_checksum(block);
}

static void bitmap_set_free(uint8_t *block, uint32_t number, int is_free) {
    const uint8_t mask = (uint8_t)(0x80U >> (number & 7U));
    uint8_t *slot = block + 12U + number / 8U;
    if (is_free) *slot |= mask;
    else *slot &= (uint8_t)~mask;
}

static void make_bitmap(uint8_t *block) {
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "BTMP", SFS_TM_BITMAP);
    for (uint32_t number = 0U; number < SFS_TM_BLOCKS; ++number)
        bitmap_set_free(block, number, 1);
    for (uint32_t number = 0U; number < SFS_TM_RESERVED; ++number)
        bitmap_set_free(block, number, 0);
    bitmap_set_free(block, SFS_TM_BLOCKS - 1U, 0);
    for (uint32_t fragment = 0U; fragment < SFS_TM_FRAGMENTS; ++fragment) {
        const uint32_t start = fragment_start(fragment);
        for (uint32_t offset = 0U; offset < SFS_TM_CHUNK_BLOCKS; ++offset)
            bitmap_set_free(block, start + offset, 0);
    }
    stamp_checksum(block);
}

static void make_extent_tree(uint8_t *block) {
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "BNDC", SFS_TM_EXTENTS);
    put16(block + 12U, SFS_TM_FRAGMENTS);
    block[14U] = 1U;
    block[15U] = 14U;
    for (uint32_t index = 0U; index < SFS_TM_FRAGMENTS; ++index) {
        uint8_t *node = block + 16U + (size_t)index * 14U;
        const uint32_t start = fragment_start(index);
        put32(node, start);
        put32(node + 4U, index + 1U < SFS_TM_FRAGMENTS ? fragment_start(index + 1U) : 0U);
        put32(node + 8U, index > 0U ? fragment_start(index - 1U) : 0U);
        put16(node + 12U, SFS_TM_CHUNK_BLOCKS);
    }
    stamp_checksum(block);
}

static void make_object_container(uint8_t *block) {
    uint8_t *object;
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "OBJC", SFS_TM_OBJECTS);
    object = block + 24U;
    put32(object + 4U, SFS_TM_FILE_ID);
    put32(object + 8U, 0x0fU);
    put32(object + 12U, fragment_start(0U));
    put32(object + 16U, SFS_TM_FILE_BYTES);
    object[24U] = 0U;
    memcpy(object + 25U, "fragmented-00.bin", 18U);
    object[43U] = 0U;
    stamp_checksum(block);
}

static void make_payload(uint8_t *block, uint32_t fragment, uint32_t within) {
    uint64_t state = UINT64_C(0x6a09e667f3bcc909) ^
                     ((uint64_t)fragment << 32) ^
                     ((uint64_t)within * UINT64_C(0x9e3779b97f4a7c15));
    for (size_t offset = 0U; offset < SFS_TM_BLOCK_SIZE; ++offset) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= UINT64_C(0x2545f4914f6cdd1d);
        block[offset] = (uint8_t)(state >> 56);
    }
}

static int write_block(int fd, uint32_t block_number, const uint8_t *block) {
    return pwrite(fd, block, SFS_TM_BLOCK_SIZE,
                  (off_t)block_number * SFS_TM_BLOCK_SIZE) == (ssize_t)SFS_TM_BLOCK_SIZE ? 0 : -1;
}

int ldtm_format_sfs_volume(const char *path) {
    int fd = -1;
    uint8_t *block = NULL;
    off_t bytes;
    int result = -1;
    if (path == NULL) return -1;
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    bytes = lseek(fd, 0, SEEK_END);
    if (bytes < (off_t)((uint64_t)SFS_TM_BLOCKS * SFS_TM_BLOCK_SIZE)) goto cleanup;
    block = malloc(SFS_TM_BLOCK_SIZE);
    if (block == NULL) goto cleanup;

    make_root(block, 0U, 5U);
    if (write_block(fd, 0U, block) != 0) goto cleanup;
    make_bitmap(block);
    if (write_block(fd, SFS_TM_BITMAP, block) != 0) goto cleanup;
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    if (write_block(fd, SFS_TM_ADMIN, block) != 0 ||
        write_block(fd, SFS_TM_OBJECT_NODES, block) != 0 ||
        write_block(fd, 6U, block) != 0) goto cleanup;
    make_extent_tree(block);
    if (write_block(fd, SFS_TM_EXTENTS, block) != 0) goto cleanup;
    make_object_container(block);
    if (write_block(fd, SFS_TM_OBJECTS, block) != 0) goto cleanup;

    for (uint32_t fragment = 0U; fragment < SFS_TM_FRAGMENTS; ++fragment) {
        const uint32_t start = fragment_start(fragment);
        for (uint32_t within = 0U; within < SFS_TM_CHUNK_BLOCKS; ++within) {
            make_payload(block, fragment, within);
            if (write_block(fd, start + within, block) != 0) goto cleanup;
        }
    }
    make_root(block, SFS_TM_BLOCKS - 1U, 6U);
    if (write_block(fd, SFS_TM_BLOCKS - 1U, block) != 0 || fsync(fd) != 0) goto cleanup;
    result = 0;
cleanup:
    free(block);
    if (fd >= 0) (void)close(fd);
    return result;
}

static int profile_matches(const LdtmFragmentProfile *profile) {
    return profile != NULL && profile->files == 1U &&
           profile->chunks == SFS_TM_FRAGMENTS &&
           profile->chunk_kib == SFS_TM_CHUNK_KIB &&
           profile->directory_initial == 0U && profile->directory_second == 0U;
}

int ldtm_populate_sfs_volume(const char *path, const LdtmFragmentProfile *profile) {
    SfsAnalysis analysis;
    char error[256] = {0};
    if (!profile_matches(profile)) return -1;
    return sfs_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) == 0 &&
           analysis.regular_files == 1U && analysis.fragmented_files == 1U ? 0 : -1;
}

int ldtm_verify_sfs_payload(const char *path, const LdtmFragmentProfile *profile,
                            char *detail, size_t detail_capacity) {
    SfsAnalysis analysis;
    uint8_t *actual = NULL;
    uint8_t *expected = NULL;
    char error[256] = {0};
    int fd = -1;
    int result = -1;
    if (detail != NULL && detail_capacity > 0U) detail[0] = '\0';
    if (!profile_matches(profile)) return -1;
    if (!sfs_probe(path) || sfs_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) != 0)
        goto cleanup;
    if (analysis.block_size != SFS_TM_BLOCK_SIZE || analysis.total_blocks != SFS_TM_BLOCKS ||
        analysis.data_blocks != SFS_TM_DATA_BLOCKS || analysis.regular_files != 1U ||
        analysis.fragmented_files != 1U || analysis.growth_10_satisfied ||
        !analysis.primary_root_valid || !analysis.backup_root_valid || analysis.transaction_pending)
        goto cleanup;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    actual = malloc(SFS_TM_BLOCK_SIZE);
    expected = malloc(SFS_TM_BLOCK_SIZE);
    if (fd < 0 || actual == NULL || expected == NULL) goto cleanup;
    for (uint32_t fragment = 0U; fragment < SFS_TM_FRAGMENTS; ++fragment) {
        const uint32_t start = fragment_start(fragment);
        for (uint32_t within = 0U; within < SFS_TM_CHUNK_BLOCKS; ++within) {
            if (pread(fd, actual, SFS_TM_BLOCK_SIZE,
                      (off_t)(start + within) * SFS_TM_BLOCK_SIZE) != (ssize_t)SFS_TM_BLOCK_SIZE)
                goto cleanup;
            make_payload(expected, fragment, within);
            if (memcmp(actual, expected, SFS_TM_BLOCK_SIZE) != 0) goto cleanup;
        }
    }
    if (detail != NULL && detail_capacity > 0U) {
        (void)snprintf(detail, detail_capacity,
                       "native SFS0 payload verified: 1 x 25 MiB file, 100 fragments, Growth Defrag reserve intentionally unsatisfied");
    }
    result = 0;
cleanup:
    if (result != 0 && detail != NULL && detail_capacity > 0U && detail[0] == '\0')
        (void)snprintf(detail, detail_capacity, "%s", *error != '\0' ? error : "native SFS0 payload validation failed");
    if (fd >= 0) (void)close(fd);
    free(expected);
    free(actual);
    return result;
}
