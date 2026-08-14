// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

#define PROG "linux-defragger-minix-worker"

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE\n",
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

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
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
