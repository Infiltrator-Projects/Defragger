// SPDX-License-Identifier: GPL-3.0-or-later
#include "btrfs_native.h"
#include "version.h"

#include "infiltratr/core.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG "linux-defragger-btrfs-worker"

typedef struct {
    uint64_t start;
    uint64_t end;
} UnitRange;

typedef struct {
    UnitRange *items;
    size_t count;
} UnitRanges;

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT\n", PROG);
}

static int unit_range_compare(const void *left, const void *right)
{
    const UnitRange *a = left;
    const UnitRange *b = right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

static int make_unit_ranges(const BtrfsRange *ranges, size_t count, uint32_t unit_size,
                            uint64_t limit, UnitRanges *output)
{
    output->items = NULL;
    output->count = 0U;
    if (count == 0U)
        return 0;
    UnitRange *items = calloc(count, sizeof(*items));
    if (items == NULL)
        return -1;
    size_t used = 0U;
    for (size_t i = 0U; i < count; ++i) {
        uint64_t start = ranges[i].start / unit_size;
        uint64_t end = ranges[i].end / unit_size;
        if (ranges[i].end % unit_size != 0U)
            end++;
        if (start > limit) start = limit;
        if (end > limit) end = limit;
        if (end > start)
            items[used++] = (UnitRange){start, end};
    }
    if (used > 1U)
        qsort(items, used, sizeof(*items), unit_range_compare);
    size_t merged = 0U;
    for (size_t i = 0U; i < used; ++i) {
        if (merged != 0U && items[i].start <= items[merged - 1U].end) {
            if (items[i].end > items[merged - 1U].end)
                items[merged - 1U].end = items[i].end;
        } else {
            items[merged++] = items[i];
        }
    }
    output->items = items;
    output->count = merged;
    return 0;
}

static uint64_t overlap_ranges(const UnitRanges *ranges, uint64_t start, uint64_t end)
{
    uint64_t total = 0U;
    for (size_t i = 0U; i < ranges->count; ++i) {
        if (ranges->items[i].end <= start)
            continue;
        if (ranges->items[i].start >= end)
            break;
        const uint64_t left = ranges->items[i].start > start ? ranges->items[i].start : start;
        const uint64_t right = ranges->items[i].end < end ? ranges->items[i].end : end;
        if (right > left)
            total += right - left;
    }
    return total;
}

static int parse_cells(const char *text, uint64_t *cells)
{
    uint64_t value = 0U;
    if (!infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, &value))
        return -1;
    *cells = value;
    return 0;
}

static void print_identify(void)
{
    (void)puts("{\"filesystem\":\"btrfs\"}");
}

static void print_analysis_json(const BtrfsAnalysis *analysis)
{
    const double percent = analysis->regular_files == 0U ? 0.0 :
        (100.0 * (double)analysis->fragmented_files / (double)analysis->regular_files);
    (void)printf(
        "{\"filesystem\":\"btrfs\",\"sector_size\":%u,\"node_size\":%u,"
        "\"device_id\":%" PRIu64 ",\"filesystem_bytes\":%" PRIu64 ","
        "\"physical_bytes\":%" PRIu64 ",\"logical_bytes_used\":%" PRIu64 ","
        "\"chunks\":%zu,\"chunk_tree_blocks\":%zu,\"root_tree_blocks\":%zu,"
        "\"extent_tree_blocks\":%zu,\"regular_files\":%" PRIu64 ","
        "\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64 ","
        "\"fragmented_directories\":%" PRIu64 ",\"fragmentation_percent\":%.6f,"
        "\"filesystem_roots_scanned\":%" PRIu64 ",\"filesystem_tree_blocks\":%" PRIu64 ","
        "\"malformed_items\":%" PRIu64 "}\n",
        analysis->sector_size, analysis->node_size, analysis->device_id,
        analysis->total_bytes, analysis->physical_bytes, analysis->logical_bytes_used,
        analysis->chunk_count, analysis->chunk_tree_blocks, analysis->root_tree_blocks,
        analysis->extent_tree_blocks, analysis->regular_files, analysis->directories,
        analysis->fragmented_files, analysis->fragmented_directories, percent,
        analysis->filesystem_roots_scanned, analysis->filesystem_tree_blocks,
        analysis->malformed_items);
}

static int print_map_json(const BtrfsAnalysis *analysis, uint64_t requested_cells)
{
    const uint64_t unit = analysis->sector_size;
    uint64_t filesystem_units = analysis->total_bytes / unit;
    if (analysis->total_bytes % unit != 0U)
        filesystem_units++;
    uint64_t physical = analysis->physical_bytes > analysis->total_bytes ?
                        analysis->physical_bytes : analysis->total_bytes;
    uint64_t total_units = physical / unit;
    if (physical % unit != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;
    if (filesystem_units > total_units)
        filesystem_units = total_units;
    uint64_t cells = requested_cells;
    if (cells > total_units)
        cells = total_units;
    if (cells == 0U)
        cells = 1U;

    UnitRanges used = {0};
    UnitRanges fragmented = {0};
    if (make_unit_ranges(analysis->used_ranges, analysis->used_range_count,
                         analysis->sector_size, filesystem_units, &used) != 0 ||
        make_unit_ranges(analysis->fragmented_ranges, analysis->fragmented_range_count,
                         analysis->sector_size, filesystem_units, &fragmented) != 0) {
        free(used.items);
        free(fragmented.items);
        return -1;
    }

    uint64_t free_total = 0U;
    uint64_t used_total = 0U;
    uint64_t unknown_total = 0U;
    uint64_t outside_total = 0U;
    uint64_t fragmented_total = 0U;
    for (size_t i = 0U; i < fragmented.count; ++i)
        fragmented_total += fragmented.items[i].end - fragmented.items[i].start;

    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\",\"filesystem\":\"btrfs\","
        "\"map_accuracy\":\"exact-single-device\",\"unit_size\":%u,"
        "\"total_units\":%" PRIu64 ",\"cell_count\":%" PRIu64 ","
        "\"total_bytes\":%" PRIu64 ",",
        analysis->sector_size, total_units, cells, total_units * unit);

    /* Totals are emitted after cell accounting, so buffer the cells in memory as
       compact count records rather than building a second allocation bitmap. */
    typedef struct {
        uint64_t start, end, free_count, used_count, unknown_count, outside_count, fragmented_count;
    } Cell;
    Cell *cell_values = calloc((size_t)cells, sizeof(*cell_values));
    if (cell_values == NULL) {
        free(used.items);
        free(fragmented.items);
        return -1;
    }
    for (uint64_t index = 0U; index < cells; ++index) {
        const uint64_t start = index * total_units / cells;
        uint64_t end = (index + 1U) * total_units / cells;
        if (end <= start)
            end = start + 1U;
        const uint64_t fs_end = end < filesystem_units ? end : filesystem_units;
        const uint64_t fs_start = start < filesystem_units ? start : filesystem_units;
        const uint64_t filesystem_count = fs_end > fs_start ? fs_end - fs_start : 0U;
        const uint64_t used_count = overlap_ranges(&used, start, fs_end);
        const uint64_t free_count = filesystem_count >= used_count ? filesystem_count - used_count : 0U;
        const uint64_t outside_start = start > filesystem_units ? start : filesystem_units;
        const uint64_t outside_count = end > outside_start ? end - outside_start : 0U;
        const uint64_t accounted = free_count + used_count + outside_count;
        const uint64_t length = end - start;
        const uint64_t unknown_count = length > accounted ? length - accounted : 0U;
        uint64_t fragmented_count = overlap_ranges(&fragmented, start, end);
        if (fragmented_count > used_count)
            fragmented_count = used_count;
        cell_values[index] = (Cell){
            start, end - 1U, free_count, used_count, unknown_count,
            outside_count, fragmented_count,
        };
        free_total += free_count;
        used_total += used_count;
        unknown_total += unknown_count;
        outside_total += outside_count;
    }

    (void)printf(
        "\"free_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64 ","
        "\"unknown_bytes\":%" PRIu64 ",\"cells\":[",
        free_total * unit, used_total * unit, unknown_total * unit);
    for (uint64_t index = 0U; index < cells; ++index) {
        const Cell *cell = &cell_values[index];
        if (index != 0U)
            (void)putchar(',');
        (void)printf(
            "{\"start\":%" PRIu64 ",\"end\":%" PRIu64 ",\"free\":%" PRIu64 ","
            "\"used\":%" PRIu64 ",\"unknown\":%" PRIu64 ",\"bad\":0,"
            "\"fragmented\":%" PRIu64 ",\"directory\":0,\"outside\":%" PRIu64 "}",
            cell->start, cell->end, cell->free_count, cell->used_count,
            cell->unknown_count, cell->fragmented_count, cell->outside_count);
    }
    const double percent = analysis->regular_files == 0U ? 0.0 :
        (100.0 * (double)analysis->fragmented_files / (double)analysis->regular_files);
    (void)printf(
        "],\"details\":{\"sector_size\":%u,\"node_size\":%u,\"device_id\":%" PRIu64 ","
        "\"chunks\":%zu,\"logical_bytes_used\":%" PRIu64 ",\"chunk_tree_blocks\":%zu,"
        "\"root_tree_blocks\":%zu,\"extent_tree_blocks\":%zu,"
        "\"physical_units\":%" PRIu64 ",\"filesystem_units\":%" PRIu64 ","
        "\"outside_filesystem_units\":%" PRIu64 ",\"fragmentation_available\":true,"
        "\"fragmentation_basis\":\"Btrfs inode and FILE_EXTENT_ITEM records across live filesystem roots\","
        "\"directory_fragmentation_note\":\"Btrfs directory records share filesystem-tree blocks and do not form private block chains\","
        "\"filesystem_roots_scanned\":%" PRIu64 ",\"filesystem_tree_blocks\":%" PRIu64 ","
        "\"malformed_items\":%" PRIu64 ",\"fragmented_sectors_mapped\":%" PRIu64 "},"
        "\"filesystem_units\":%" PRIu64 ",\"filesystem_bytes\":%" PRIu64 ","
        "\"outside_bytes\":%" PRIu64 ",\"regular_files\":%" PRIu64 ","
        "\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64 ","
        "\"fragmented_directories\":%" PRIu64 ",\"fragmentation_percent\":%.6f}\n",
        analysis->sector_size, analysis->node_size, analysis->device_id,
        analysis->chunk_count, analysis->logical_bytes_used, analysis->chunk_tree_blocks,
        analysis->root_tree_blocks, analysis->extent_tree_blocks, total_units,
        filesystem_units, outside_total, analysis->filesystem_roots_scanned,
        analysis->filesystem_tree_blocks, analysis->malformed_items, fragmented_total,
        filesystem_units, filesystem_units * unit, outside_total * unit,
        analysis->regular_files, analysis->directories, analysis->fragmented_files,
        analysis->fragmented_directories, percent);

    free(cell_values);
    free(used.items);
    free(fragmented.items);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "identify") == 0) {
        if (!btrfs_probe(argv[2]))
            return 1;
        print_identify();
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "map") == 0 && strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (parse_cells(argv[4], &cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        BtrfsAnalysis analysis;
        char error[512] = {0};
        if (btrfs_analyse(argv[2], &analysis, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error : "Btrfs analysis failed");
            return 1;
        }
        const int result = print_map_json(&analysis, cells);
        btrfs_analysis_free(&analysis);
        if (result != 0) {
            (void)fprintf(stderr, "%s: out of memory building Btrfs map\n", PROG);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "analyse-json") == 0) {
        BtrfsAnalysis analysis;
        char error[512] = {0};
        if (btrfs_analyse(argv[2], &analysis, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error : "Btrfs analysis failed");
            return 1;
        }
        print_analysis_json(&analysis);
        btrfs_analysis_free(&analysis);
        return 0;
    }
    usage(stderr);
    return 2;
}
