// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROG "linux-defragger-minix-worker"
#define MINIX_MAP_UNIT_SIZE 1024U

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT\n",
                  PROG);
}

static void print_summary_json(const MinixSummary *summary, int detailed)
{
    (void)printf("{\"filesystem\":\"minix\",\"variant\":\"%s\","
                 "\"byte_order\":\"%s\",\"version\":%u",
                 minix_variant_name(summary), minix_byte_order_name(summary),
                 summary->version);
    if (detailed != 0) {
        (void)printf(",\"magic\":%u,\"inode_count\":%u,\"zone_count\":%u,"
                     "\"imap_blocks\":%u,\"zmap_blocks\":%u,"
                     "\"first_data_zone\":%u,\"log_zone_size\":%u,"
                     "\"block_size\":%u,\"zone_size\":%llu,\"max_size\":%u",
                     (unsigned int)summary->magic,
                     (unsigned int)summary->inode_count,
                     (unsigned int)summary->zone_count,
                     (unsigned int)summary->imap_blocks,
                     (unsigned int)summary->zmap_blocks,
                     (unsigned int)summary->first_data_zone,
                     (unsigned int)summary->log_zone_size,
                     (unsigned int)summary->block_size,
                     (unsigned long long)summary->zone_size,
                     (unsigned int)summary->max_size);
    }
    (void)puts("}");
}

static int device_size_bytes(const char *path, uint64_t *size_bytes)
{
    struct stat status;
    if (stat(path, &status) != 0)
        return -1;
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        *size_bytes = (uint64_t)status.st_size;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    uint64_t bytes = 0U;
    const int result = ioctl(fd, BLKGETSIZE64, &bytes);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    if (result != 0)
        return -1;
    *size_bytes = bytes;
    return 0;
}

static int parse_cells(const char *text, uint64_t *cells)
{
    if (text == NULL || *text == '\0')
        return -1;
    errno = 0;
    char *end = NULL;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0ULL)
        return -1;
    *cells = (uint64_t)value;
    return 0;
}

static void print_map_json(const MinixSummary *summary, uint64_t size_bytes,
                           uint64_t requested_cells)
{
    uint64_t total_units = size_bytes / MINIX_MAP_UNIT_SIZE;
    if (size_bytes % MINIX_MAP_UNIT_SIZE != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;

    uint64_t cell_count = requested_cells;
    if (cell_count > total_units)
        cell_count = total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    const uint64_t base = total_units / cell_count;
    const uint64_t remainder = total_units % cell_count;
    uint64_t carry = 0U;
    uint64_t cursor = 0U;

    (void)printf("{\"schema\":1,\"backend\":\"read-only-domain\","
                 "\"filesystem\":\"minix\",\"map_accuracy\":\"summary\","
                 "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
                 "\"total_bytes\":%llu,\"free_bytes\":0,\"used_bytes\":0,"
                 "\"unknown_bytes\":%llu,\"cells\":[",
                 MINIX_MAP_UNIT_SIZE,
                 (unsigned long long)total_units,
                 (unsigned long long)cell_count,
                 (unsigned long long)(total_units * MINIX_MAP_UNIT_SIZE),
                 (unsigned long long)(total_units * MINIX_MAP_UNIT_SIZE));

    for (uint64_t index = 0U; index < cell_count; ++index) {
        uint64_t length = base;
        if (remainder != 0U) {
            const uint64_t threshold = cell_count - remainder;
            if (carry >= threshold) {
                carry -= threshold;
                length++;
            } else {
                carry += remainder;
            }
        }
        const uint64_t start = cursor;
        const uint64_t end = cursor + length - 1U;
        if (index != 0U)
            (void)putchar(',');
        (void)printf("{\"start\":%llu,\"end\":%llu,\"free\":0,\"used\":0,"
                     "\"unknown\":%llu,\"bad\":0,\"fragmented\":0,"
                     "\"directory\":0}",
                     (unsigned long long)start,
                     (unsigned long long)end,
                     (unsigned long long)length);
        cursor += length;
    }

    (void)printf("],\"details\":{\"magic\":\"0x%x\",\"variant\":\"%s\","
                 "\"byte_order\":\"%s\",\"note\":"
                 "\"Minix filesystem detected; zone bitmap location mapping is not yet decoded\"}}\n",
                 (unsigned int)summary->magic,
                 minix_variant_name(summary),
                 minix_byte_order_name(summary));
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }

    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t requested_cells = 0U;
        if (parse_cells(argv[4], &requested_cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        MinixSummary summary;
        char error[256];
        if (minix_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        uint64_t size_bytes = 0U;
        if (device_size_bytes(argv[2], &size_bytes) != 0) {
            (void)fprintf(stderr, "%s: cannot determine device size: %s\n",
                          PROG, strerror(errno));
            return 1;
        }
        print_map_json(&summary, size_bytes, requested_cells);
        return 0;
    }

    if (argc != 3) {
        usage(stderr);
        return 2;
    }

    MinixSummary summary;
    char error[256];
    if (minix_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    if (strcmp(argv[1], "identify") == 0) {
        print_summary_json(&summary, 0);
        return 0;
    }
    if (strcmp(argv[1], "analyse-json") == 0) {
        print_summary_json(&summary, 1);
        return 0;
    }

    usage(stderr);
    return 2;
}
