// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE_BYTES 4096U
#define SUPER_OFFSET 1024U

static void put16(uint8_t *p, uint16_t value, int little)
{
    if (little != 0) {
        p[0] = (uint8_t)(value & 0xffU);
        p[1] = (uint8_t)(value >> 8);
    } else {
        p[0] = (uint8_t)(value >> 8);
        p[1] = (uint8_t)(value & 0xffU);
    }
}

static void put32(uint8_t *p, uint32_t value, int little)
{
    if (little != 0) {
        p[0] = (uint8_t)(value & 0xffU);
        p[1] = (uint8_t)((value >> 8) & 0xffU);
        p[2] = (uint8_t)((value >> 16) & 0xffU);
        p[3] = (uint8_t)(value >> 24);
    } else {
        p[0] = (uint8_t)(value >> 24);
        p[1] = (uint8_t)((value >> 16) & 0xffU);
        p[2] = (uint8_t)((value >> 8) & 0xffU);
        p[3] = (uint8_t)(value & 0xffU);
    }
}

static int write_image(const uint8_t *image, char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-minix-XXXXXX");
    const int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    const ssize_t count = write(fd, image, IMAGE_BYTES);
    const int close_result = close(fd);
    if (count != (ssize_t)IMAGE_BYTES || close_result != 0) {
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static void make_legacy(uint8_t image[IMAGE_BYTES], uint16_t magic,
                        unsigned int version, int little)
{
    memset(image, 0, IMAGE_BYTES);
    uint8_t *sb = image + SUPER_OFFSET;
    put16(sb + 0U, 128U, little);
    put16(sb + 2U, 4096U, little);
    put16(sb + 4U, 1U, little);
    put16(sb + 6U, 1U, little);
    put16(sb + 8U, 10U, little);
    put16(sb + 10U, 0U, little);
    put32(sb + 12U, 0x00100000U, little);
    put16(sb + 16U, magic, little);
    if (version == 2U)
        put32(sb + 20U, 5000U, little);
}

static void make_v3(uint8_t image[IMAGE_BYTES], int little)
{
    memset(image, 0, IMAGE_BYTES);
    uint8_t *sb = image + SUPER_OFFSET;
    put32(sb + 0U, 300U, little);
    put16(sb + 6U, 1U, little);
    put16(sb + 8U, 2U, little);
    put16(sb + 10U, 20U, little);
    put16(sb + 12U, 1U, little);
    put32(sb + 16U, 0x01000000U, little);
    put32(sb + 20U, 12000U, little);
    put16(sb + 24U, 0x4d5aU, little);
    put16(sb + 28U, 4096U, little);
}

static int expect_summary(const uint8_t image[IMAGE_BYTES],
                          const char *variant, const char *order,
                          unsigned int version, uint32_t block_size,
                          uint32_t zones)
{
    char path[64];
    if (write_image(image, path) != 0)
        return 1;

    MinixSummary summary;
    char error[160];
    const int result = minix_read_summary(path, &summary, error, sizeof(error));
    (void)unlink(path);
    if (result != 0) {
        (void)fprintf(stderr, "parse failed: %s\n", error);
        return 1;
    }
    if (strcmp(minix_variant_name(&summary), variant) != 0 ||
        strcmp(minix_byte_order_name(&summary), order) != 0 ||
        summary.version != version || summary.block_size != block_size ||
        summary.zone_count != zones || summary.inode_count == 0U ||
        summary.imap_blocks == 0U || summary.zmap_blocks == 0U ||
        summary.first_data_zone == 0U || summary.zone_size < block_size) {
        (void)fprintf(stderr, "unexpected Minix summary for %s/%s\n", variant, order);
        return 1;
    }
    return 0;
}

int main(void)
{
    uint8_t image[IMAGE_BYTES];

    make_legacy(image, 0x137fU, 1U, 1);
    if (expect_summary(image, "v1", "little", 1U, 1024U, 4096U) != 0)
        return 1;

    make_legacy(image, 0x138fU, 1U, 0);
    if (expect_summary(image, "v1-30char", "big", 1U, 1024U, 4096U) != 0)
        return 1;

    make_legacy(image, 0x2468U, 2U, 1);
    if (expect_summary(image, "v2", "little", 2U, 1024U, 5000U) != 0)
        return 1;

    make_legacy(image, 0x2478U, 2U, 0);
    if (expect_summary(image, "v2-30char", "big", 2U, 1024U, 5000U) != 0)
        return 1;

    make_v3(image, 1);
    if (expect_summary(image, "v3", "little", 3U, 4096U, 12000U) != 0)
        return 1;

    make_v3(image, 0);
    if (expect_summary(image, "v3", "big", 3U, 4096U, 12000U) != 0)
        return 1;

    memset(image, 0, IMAGE_BYTES);
    char path[64];
    if (write_image(image, path) != 0)
        return 1;
    MinixSummary summary;
    if (minix_read_summary(path, &summary, NULL, 0U) == 0) {
        (void)unlink(path);
        (void)fprintf(stderr, "invalid Minix magic was accepted\n");
        return 1;
    }
    (void)unlink(path);

    make_v3(image, 1);
    put32(image + SUPER_OFFSET + 20U, 10U, 1);
    if (write_image(image, path) != 0)
        return 1;
    if (minix_read_summary(path, &summary, NULL, 0U) == 0) {
        (void)unlink(path);
        (void)fprintf(stderr, "invalid Minix geometry was accepted\n");
        return 1;
    }
    (void)unlink(path);

    return 0;
}
