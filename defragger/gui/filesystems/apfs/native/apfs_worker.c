// SPDX-License-Identifier: GPL-3.0-or-later
#include "apfs_native.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

#define PROG "linux-defragger-apfs-worker"

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: %s --version | identify DEVICE | analyse-json DEVICE\n", PROG);
}

static void print_uuid(const uint8_t uuid[16])
{
    for (size_t index = 0U; index < 16U; ++index)
        printf("%02x", (unsigned int)uuid[index]);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }
    if (argc != 3) {
        usage(stderr);
        return 2;
    }

    ApfsSummary summary;
    char error[256];
    if (apfs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    if (strcmp(argv[1], "identify") == 0) {
        printf("{\"filesystem\":\"apfs\",\"block_size\":%u,\"block_count\":%llu}\n",
               (unsigned int)summary.block_size,
               (unsigned long long)summary.block_count);
        return 0;
    }
    if (strcmp(argv[1], "analyse-json") == 0) {
        printf("{\"filesystem\":\"apfs\",\"block_size\":%u,\"block_count\":%llu,\"container_uuid\":\"",
               (unsigned int)summary.block_size,
               (unsigned long long)summary.block_count);
        print_uuid(summary.container_uuid);
        puts("\"}");
        return 0;
    }

    usage(stderr);
    return 2;
}
