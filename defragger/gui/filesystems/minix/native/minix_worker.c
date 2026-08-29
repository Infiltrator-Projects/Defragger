// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"
#include "version.h"
#include "infiltratr/core.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG "linux-defragger-minix-worker"

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT\n",
                  PROG);
}

static int parse_cells(const char *text, uint64_t *cells)
{
    return infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, cells)
        ? 0 : -1;
}

static void print_identity(const MinixSummary *summary)
{
    (void)printf("{\"filesystem\":\"minix\",\"variant\":\"%s\","
                 "\"byte_order\":\"%s\",\"version\":%u}\n",
                 minix_variant_name(summary), minix_byte_order_name(summary),
                 summary->version);
}

static void print_analysis(const MinixAnalysis *analysis)
{
    const MinixSummary *summary = &analysis->summary;
    (void)printf(
        "{\"filesystem\":\"minix\",\"variant\":\"%s\",\"byte_order\":\"%s\","
        "\"version\":%u,\"magic\":%u,\"inode_count\":%u,\"zone_count\":%u,"
        "\"imap_blocks\":%u,\"zmap_blocks\":%u,\"first_data_zone\":%u,"
        "\"log_zone_size\":%u,\"block_size\":%u,\"zone_size\":%llu,"
        "\"max_size\":%u,\"filesystem_bytes\":%llu,\"physical_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,\"regular_files\":%llu,"
        "\"directories\":%llu,\"fragmented_files\":%llu,"
        "\"fragmented_directories\":%llu}\n",
        minix_variant_name(summary), minix_byte_order_name(summary),
        summary->version, (unsigned int)summary->magic,
        (unsigned int)summary->inode_count, (unsigned int)summary->zone_count,
        (unsigned int)summary->imap_blocks, (unsigned int)summary->zmap_blocks,
        (unsigned int)summary->first_data_zone,
        (unsigned int)summary->log_zone_size,
        (unsigned int)summary->block_size,
        (unsigned long long)summary->zone_size,
        (unsigned int)summary->max_size,
        (unsigned long long)analysis->filesystem_bytes,
        (unsigned long long)analysis->physical_bytes,
        (unsigned long long)(analysis->free_zones * summary->zone_size),
        (unsigned long long)(analysis->used_zones * summary->zone_size),
        (unsigned long long)analysis->regular_files,
        (unsigned long long)analysis->directories,
        (unsigned long long)analysis->fragmented_files,
        (unsigned long long)analysis->fragmented_directories);
}

static int print_map(const char *path, uint64_t requested_cells)
{
    MinixAnalysis analysis;
    char error[256];
    if (minix_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }
    uint64_t cell_count = requested_cells;
    if (cell_count > analysis.total_units)
        cell_count = analysis.total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    if (cell_count > SIZE_MAX / sizeof(MinixMapCell)) {
        (void)fprintf(stderr, "%s: map cell count is too large\n", PROG);
        return 1;
    }
    MinixMapCell *cells = calloc((size_t)cell_count, sizeof(*cells));
    if (cells == NULL) {
        (void)fprintf(stderr, "%s: out of memory allocating map cells\n", PROG);
        return 1;
    }
    if (minix_analyse(path, &analysis, cells, cell_count,
                      error, sizeof(error)) != 0) {
        free(cells);
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    const MinixSummary *summary = &analysis.summary;
    const uint64_t outside_units =
        analysis.total_units > summary->zone_count
            ? analysis.total_units - summary->zone_count : 0U;
    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\","
        "\"filesystem\":\"minix\",\"map_accuracy\":\"exact\","
        "\"unit_size\":%llu,\"total_units\":%llu,\"cell_count\":%llu,"
        "\"total_bytes\":%llu,\"filesystem_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,\"unknown_bytes\":%llu,"
        "\"regular_files\":%llu,\"directories\":%llu,"
        "\"fragmented_files\":%llu,\"fragmented_directories\":%llu,\"cells\":[",
        (unsigned long long)summary->zone_size,
        (unsigned long long)analysis.total_units,
        (unsigned long long)cell_count,
        (unsigned long long)(analysis.total_units * summary->zone_size),
        (unsigned long long)analysis.filesystem_bytes,
        (unsigned long long)(analysis.free_zones * summary->zone_size),
        (unsigned long long)(analysis.used_zones * summary->zone_size),
        (unsigned long long)(outside_units * summary->zone_size),
        (unsigned long long)analysis.regular_files,
        (unsigned long long)analysis.directories,
        (unsigned long long)analysis.fragmented_files,
        (unsigned long long)analysis.fragmented_directories);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        if (index != 0U)
            (void)putchar(',');
        (void)printf(
            "{\"start\":%llu,\"end\":%llu,\"free\":%llu,\"used\":%llu,"
            "\"unknown\":%llu,\"bad\":0,\"fragmented\":%llu,\"directory\":%llu}",
            (unsigned long long)cells[index].start,
            (unsigned long long)cells[index].end,
            (unsigned long long)cells[index].free_count,
            (unsigned long long)cells[index].used_count,
            (unsigned long long)cells[index].outside_count,
            (unsigned long long)cells[index].fragmented_count,
            (unsigned long long)cells[index].directory_count);
    }
    (void)printf(
        "],\"details\":{\"variant\":\"%s\",\"version\":%u,"
        "\"byte_order\":\"%s\",\"allocation_basis\":\"inode and zone bitmaps\","
        "\"fragmentation_basis\":\"direct and indirect inode zone trees\"}}\n",
        minix_variant_name(summary), summary->version,
        minix_byte_order_name(summary));
    free(cells);
    return 0;
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
        return print_map(argv[2], requested_cells);
    }

    if (argc != 3) {
        usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "identify") == 0) {
        MinixSummary summary;
        char error[256];
        if (minix_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        print_identity(&summary);
        return 0;
    }
    if (strcmp(argv[1], "analyse-json") == 0) {
        MinixAnalysis analysis;
        char error[256];
        if (minix_analyse(argv[2], &analysis, NULL, 0U,
                          error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        print_analysis(&analysis);
        return 0;
    }

    usage(stderr);
    return 2;
}
