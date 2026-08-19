// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"

#include "ld_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MINIX_SUPER_OFFSET 1024U
#define MINIX_SUPER_BYTES 64U
#define MINIX_V1_MAGIC 0x137fU
#define MINIX_V1_30_MAGIC 0x138fU
#define MINIX_V2_MAGIC 0x2468U
#define MINIX_V2_30_MAGIC 0x2478U
#define MINIX_V3_MAGIC 0x4d5aU

static void minix_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static uint16_t minix_u16(const uint8_t *p, bool little_endian)
{
    if (little_endian)
        return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t minix_u32(const uint8_t *p, bool little_endian)
{
    if (little_endian) {
        return (uint32_t)p[0] |
               ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    }
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static bool minix_magic_v1_v2(uint16_t magic)
{
    return magic == MINIX_V1_MAGIC || magic == MINIX_V1_30_MAGIC ||
           magic == MINIX_V2_MAGIC || magic == MINIX_V2_30_MAGIC;
}

static bool minix_detect(const uint8_t *superblock, uint16_t *magic,
                         bool *little_endian)
{
    const bool orders[] = {true, false};
    for (size_t index = 0U; index < sizeof(orders) / sizeof(orders[0]); ++index) {
        const bool little = orders[index];
        const uint16_t legacy = minix_u16(superblock + 16U, little);
        if (minix_magic_v1_v2(legacy)) {
            *magic = legacy;
            *little_endian = little;
            return true;
        }
        const uint16_t version3 = minix_u16(superblock + 24U, little);
        if (version3 == MINIX_V3_MAGIC) {
            *magic = version3;
            *little_endian = little;
            return true;
        }
    }
    return false;
}

static int minix_parse(const uint8_t *superblock, MinixSummary *summary,
                       char *error, size_t error_size)
{
    uint16_t magic = 0U;
    bool little_endian = true;
    if (!minix_detect(superblock, &magic, &little_endian)) {
        minix_error(error, error_size, "not a recognised Minix filesystem");
        return -1;
    }

    memset(summary, 0, sizeof(*summary));
    summary->magic = magic;
    summary->little_endian = little_endian;

    if (magic == MINIX_V3_MAGIC) {
        summary->version = 3U;
        summary->inode_count = minix_u32(superblock + 0U, little_endian);
        summary->imap_blocks = minix_u16(superblock + 6U, little_endian);
        summary->zmap_blocks = minix_u16(superblock + 8U, little_endian);
        summary->first_data_zone = minix_u16(superblock + 10U, little_endian);
        summary->log_zone_size = minix_u16(superblock + 12U, little_endian);
        summary->max_size = minix_u32(superblock + 16U, little_endian);
        summary->zone_count = minix_u32(superblock + 20U, little_endian);
        summary->block_size = minix_u16(superblock + 28U, little_endian);
        if (summary->block_size == 0U)
            summary->block_size = 1024U;
    } else {
        summary->version =
            (magic == MINIX_V1_MAGIC || magic == MINIX_V1_30_MAGIC) ? 1U : 2U;
        summary->long_names =
            magic == MINIX_V1_30_MAGIC || magic == MINIX_V2_30_MAGIC;
        summary->inode_count = minix_u16(superblock + 0U, little_endian);
        summary->imap_blocks = minix_u16(superblock + 4U, little_endian);
        summary->zmap_blocks = minix_u16(superblock + 6U, little_endian);
        summary->first_data_zone = minix_u16(superblock + 8U, little_endian);
        summary->log_zone_size = minix_u16(superblock + 10U, little_endian);
        summary->max_size = minix_u32(superblock + 12U, little_endian);
        summary->zone_count = summary->version == 1U
                                  ? minix_u16(superblock + 2U, little_endian)
                                  : minix_u32(superblock + 20U, little_endian);
        summary->block_size = 1024U;
    }

    if (summary->inode_count == 0U || summary->zone_count == 0U ||
        summary->imap_blocks == 0U || summary->zmap_blocks == 0U ||
        summary->first_data_zone == 0U ||
        summary->first_data_zone >= summary->zone_count ||
        summary->block_size < 1024U || summary->block_size > 65536U ||
        (summary->block_size & (summary->block_size - 1U)) != 0U ||
        summary->log_zone_size > 8U) {
        minix_error(error, error_size, "invalid Minix filesystem geometry");
        return -1;
    }

    summary->zone_size =
        (uint64_t)summary->block_size << summary->log_zone_size;
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

int minix_read_summary(const char *path, MinixSummary *summary,
                       char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        minix_error(error, error_size, "invalid Minix summary request");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    uint8_t superblock[MINIX_SUPER_BYTES];
    const ssize_t count = ld_pread_full(fd, superblock, sizeof(superblock),
                                        (off_t)MINIX_SUPER_OFFSET);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;

    if (count < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "read: %s", strerror(errno));
        return -1;
    }
    if ((size_t)count != sizeof(superblock)) {
        minix_error(error, error_size, "Minix volume is shorter than its superblock");
        return -1;
    }
    return minix_parse(superblock, summary, error, error_size);
}

bool minix_probe(const char *path)
{
    MinixSummary summary;
    return minix_read_summary(path, &summary, NULL, 0U) == 0;
}

const char *minix_variant_name(const MinixSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    switch (summary->magic) {
    case MINIX_V1_MAGIC:
        return "v1";
    case MINIX_V1_30_MAGIC:
        return "v1-30char";
    case MINIX_V2_MAGIC:
        return "v2";
    case MINIX_V2_30_MAGIC:
        return "v2-30char";
    case MINIX_V3_MAGIC:
        return "v3";
    default:
        return "unknown";
    }
}

const char *minix_byte_order_name(const MinixSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return summary->little_endian ? "little" : "big";
}
