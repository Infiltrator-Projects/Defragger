// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfs_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCKS 128U
#define TEST_BYTES (TEST_BLOCK_SIZE * TEST_BLOCKS)

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void stamp_checksum(uint8_t *block)
{
    put32(block + 4U, 0U);
    uint32_t sum = 1U;
    for (uint32_t offset = 0U; offset < TEST_BLOCK_SIZE; offset += 4U)
        sum += get32(block + offset);
    put32(block + 4U, 0U - sum);
}

static void set_header(uint8_t *block, const char id[4], uint32_t own_block)
{
    memcpy(block, id, 4U);
    put32(block + 8U, own_block);
}

static void make_root(uint8_t *block, uint32_t own_block, uint16_t sequence)
{
    memset(block, 0, TEST_BLOCK_SIZE);
    set_header(block, "SFS\0", own_block);
    put16(block + 12U, 3U);
    put16(block + 14U, sequence);
    put32(block + 48U, TEST_BLOCKS);
    put32(block + 52U, TEST_BLOCK_SIZE);
    put32(block + 96U, 1U);  /* bitmap */
    put32(block + 100U, 2U); /* admin space */
    put32(block + 104U, 4U); /* root object container */
    put32(block + 108U, 3U); /* extent B-tree root */
    put32(block + 112U, 5U); /* object node root */
    stamp_checksum(block);
}

static void make_bitmap(uint8_t *block)
{
    memset(block, 0, TEST_BLOCK_SIZE);
    set_header(block, "BTMP", 1U);
    for (uint32_t number = 16U; number < 100U; ++number)
        block[12U + number / 8U] |= (uint8_t)(0x80U >> (number & 7U));
    stamp_checksum(block);
}

static void make_image(uint8_t *image, int transaction_pending)
{
    memset(image, 0, TEST_BYTES);
    make_root(image, 0U, 5U);
    make_root(image + (TEST_BLOCKS - 1U) * TEST_BLOCK_SIZE,
              TEST_BLOCKS - 1U, 6U);
    make_bitmap(image + TEST_BLOCK_SIZE);
    if (transaction_pending != 0) {
        uint8_t *marker = image + 6U * TEST_BLOCK_SIZE;
        set_header(marker, "TRFA", 6U);
        stamp_checksum(marker);
    }
}

static int save_image(const uint8_t *image, char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-sfs-XXXXXX");
    const int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    const ssize_t written = write(fd, image, TEST_BYTES);
    const int close_result = close(fd);
    if (written != (ssize_t)TEST_BYTES || close_result != 0) {
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static int analyse_image(const uint8_t *image, SfsAnalysis *analysis,
                         SfsMapCell *cells, uint64_t cell_count,
                         char *error, size_t error_size)
{
    char path[64];
    if (save_image(image, path) != 0)
        return -2;
    const int result = sfs_analyse(path, analysis, cells, cell_count,
                                   error, error_size);
    (void)unlink(path);
    return result;
}

static int probe_image(const uint8_t *image)
{
    char path[64];
    if (save_image(image, path) != 0)
        return -1;
    const int result = sfs_probe(path) ? 1 : 0;
    (void)unlink(path);
    return result;
}

int main(void)
{
    uint8_t *image = malloc(TEST_BYTES);
    if (image == NULL)
        return 1;

    SfsAnalysis analysis;
    SfsMapCell cells[16];
    char error[256] = {0};

    make_image(image, 0);
    if (analyse_image(image, &analysis, cells, 16U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "valid SFS image rejected: %s\n", error);
        free(image);
        return 2;
    }
    if (analysis.block_size != TEST_BLOCK_SIZE ||
        analysis.total_blocks != TEST_BLOCKS ||
        analysis.free_blocks != 84U || analysis.used_blocks != 44U ||
        analysis.sequence_number != 6U ||
        !analysis.primary_root_valid || !analysis.backup_root_valid ||
        analysis.transaction_pending) {
        (void)fprintf(stderr, "unexpected SFS analysis totals or root selection\n");
        free(image);
        return 3;
    }
    uint64_t mapped_free = 0U;
    uint64_t mapped_used = 0U;
    for (size_t index = 0U; index < 16U; ++index) {
        mapped_free += cells[index].free_count;
        mapped_used += cells[index].used_count;
    }
    if (mapped_free != analysis.free_blocks || mapped_used != analysis.used_blocks) {
        (void)fprintf(stderr, "SFS cell accounting disagrees with bitmap totals\n");
        free(image);
        return 4;
    }

    make_image(image, 1);
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) != 0 ||
        !analysis.transaction_pending) {
        (void)fprintf(stderr, "SFS unfinished transaction was not reported: %s\n", error);
        free(image);
        return 5;
    }

    make_image(image, 0);
    image[TEST_BLOCK_SIZE + 20U] ^= 1U;
    if (probe_image(image) != 1) {
        (void)fprintf(stderr, "SFS identity probe depended on bitmap health\n");
        free(image);
        return 6;
    }
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) == 0) {
        (void)fprintf(stderr, "corrupt SFS bitmap checksum was accepted\n");
        free(image);
        return 7;
    }

    make_image(image, 0);
    image[4U] ^= 1U;
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) != 0 ||
        analysis.primary_root_valid || !analysis.backup_root_valid ||
        analysis.sequence_number != 6U) {
        (void)fprintf(stderr, "SFS backup root recovery failed: %s\n", error);
        free(image);
        return 8;
    }

    make_image(image, 0);
    put32(image + (TEST_BLOCKS - 1U) * TEST_BLOCK_SIZE + 48U, TEST_BLOCKS - 1U);
    stamp_checksum(image + (TEST_BLOCKS - 1U) * TEST_BLOCK_SIZE);
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) == 0) {
        (void)fprintf(stderr, "disagreeing SFS redundant-root geometry was accepted\n");
        free(image);
        return 9;
    }

    free(image);
    return 0;
}
