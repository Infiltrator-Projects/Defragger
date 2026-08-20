// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfs_native.h"
#include "version.h"
#include "infiltratr/core.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG "linux-defragger-sfs-worker"

static void usage(FILE *stream)
{
    (void)fprintf(stream,
        "Usage: %s --version | identify DEVICE | analyse-json DEVICE | map DEVICE --cells COUNT\n",
        PROG);
}

static const char *json_bool(bool value) { return value ? "true" : "false"; }

static int parse_cells(const char *text, uint64_t *cells)
{
    uint64_t value = 0U;
    if (!infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, &value))
        return -1;
    *cells = value;
    return 0;
}

static int analyse(const char *path, SfsAnalysis *analysis)
{
    char error[512] = {0};
    if (sfs_analyse(path, analysis, NULL, 0U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG,
                      error[0] != '\0' ? error : "SFS analysis failed");
        return -1;
    }
    return 0;
}

static void print_identify(void)
{
    (void)puts("{\"filesystem\":\"sfs\",\"format\":\"SFS0\"}");
}

static void print_analysis_json(const SfsAnalysis *analysis)
{
    (void)printf(
        "{\"filesystem\":\"sfs\",\"format\":\"SFS0\","
        "\"structure_version\":%u,\"sequence_number\":%u,"
        "\"block_size\":%u,\"total_blocks\":%u,\"filesystem_bytes\":%" PRIu64 ","
        "\"physical_bytes\":%" PRIu64 ",\"bitmap_base\":%u,\"bitmap_blocks\":%u,"
        "\"used_blocks\":%" PRIu64 ",\"free_blocks\":%" PRIu64 ","
        "\"primary_root_valid\":%s,\"backup_root_valid\":%s,"
        "\"transaction_pending\":%s,\"fragmentation_available\":false}\n",
        analysis->structure_version, analysis->sequence_number,
        analysis->block_size, analysis->total_blocks, analysis->filesystem_bytes,
        analysis->physical_bytes, analysis->bitmap_base, analysis->bitmap_blocks,
        analysis->used_blocks, analysis->free_blocks,
        json_bool(analysis->primary_root_valid), json_bool(analysis->backup_root_valid),
        json_bool(analysis->transaction_pending));
}

static int print_map(const char *path, uint64_t requested_cells)
{
    SfsAnalysis summary;
    if (analyse(path, &summary) != 0)
        return -1;

    const uint64_t physical_units =
        (summary.physical_bytes + summary.block_size - 1U) / summary.block_size;
    const uint64_t total_units = physical_units > summary.total_blocks
                               ? physical_units : summary.total_blocks;
    uint64_t cells = requested_cells;
    if (cells > total_units) cells = total_units;
    if (cells == 0U) cells = 1U;
    if (cells > SIZE_MAX / sizeof(SfsMapCell)) {
        (void)fprintf(stderr, "%s: allocation map is too large\n", PROG);
        return -1;
    }
    SfsMapCell *map = calloc((size_t)cells, sizeof(*map));
    if (map == NULL) {
        (void)fprintf(stderr, "%s: out of memory building SFS allocation map\n", PROG);
        return -1;
    }
    SfsAnalysis analysis;
    char error[512] = {0};
    if (sfs_analyse(path, &analysis, map, cells, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG,
                      error[0] != '\0' ? error : "SFS allocation scan failed");
        free(map);
        return -1;
    }

    const uint64_t outside_units = total_units > analysis.total_blocks
                                 ? total_units - analysis.total_blocks : 0U;
    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\",\"filesystem\":\"sfs\","
        "\"map_accuracy\":\"exact-allocation\",\"unit_size\":%u,"
        "\"total_units\":%" PRIu64 ",\"cell_count\":%" PRIu64 ","
        "\"total_bytes\":%" PRIu64 ",\"filesystem_units\":%u,"
        "\"filesystem_bytes\":%" PRIu64 ",\"outside_bytes\":%" PRIu64 ","
        "\"free_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64 ","
        "\"unknown_bytes\":0,\"cells\":[",
        analysis.block_size, total_units, cells,
        total_units * analysis.block_size, analysis.total_blocks,
        analysis.filesystem_bytes, outside_units * analysis.block_size,
        analysis.free_blocks * analysis.block_size,
        analysis.used_blocks * analysis.block_size);
    for (uint64_t i = 0U; i < cells; ++i) {
        if (i != 0U) (void)putchar(',');
        (void)printf(
            "{\"start\":%" PRIu64 ",\"end\":%" PRIu64 ","
            "\"free\":%" PRIu64 ",\"used\":%" PRIu64 ","
            "\"unknown\":0,\"bad\":0,\"fragmented\":0,\"directory\":0,"
            "\"outside\":%" PRIu64 "}",
            map[i].start, map[i].end, map[i].free_count, map[i].used_count,
            map[i].outside_count);
    }
    (void)printf(
        "],\"details\":{\"format\":\"SFS0\",\"structure_version\":%u,"
        "\"sequence_number\":%u,\"bitmap_base\":%u,\"bitmap_blocks\":%u,"
        "\"primary_root_valid\":%s,\"backup_root_valid\":%s,"
        "\"transaction_pending\":%s,\"fragmentation_available\":false,"
        "\"allocation_basis\":\"validated SFS BTMP free-space bitmap\","
        "\"sfs2_note\":\"SFS2 is not advertised until independent large-file fixtures and compatibility validation are available\"}}\n",
        analysis.structure_version, analysis.sequence_number,
        analysis.bitmap_base, analysis.bitmap_blocks,
        json_bool(analysis.primary_root_valid), json_bool(analysis.backup_root_valid),
        json_bool(analysis.transaction_pending));
    free(map);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "identify") == 0) {
        if (!sfs_probe(argv[2])) return 1;
        print_identify();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "analyse-json") == 0) {
        SfsAnalysis analysis;
        if (analyse(argv[2], &analysis) != 0) return 1;
        print_analysis_json(&analysis);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "map") == 0 && strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (parse_cells(argv[4], &cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        return print_map(argv[2], cells) == 0 ? 0 : 1;
    }
    usage(stderr);
    return 2;
}
