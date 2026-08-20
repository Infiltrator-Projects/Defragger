// SPDX-License-Identifier: GPL-3.0-or-later
#include "ufs_native.h"
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

#define PROG "linux-defragger-ufs-worker"
#define UFS_SUMMARY_UNIT_SIZE 512U

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT\n",
                  PROG);
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

static bool exact_ufs2_ready(const LdUfsSummary *summary)
{
    return summary->cylinder_geometry_known &&
           (summary->variant == LD_UFS_VARIANT_UFS2_LE ||
            summary->variant == LD_UFS_VARIANT_UFS2_BE);
}

static void print_summary_json(const LdUfsSummary *summary, int detailed)
{
    (void)printf("{\"filesystem\":\"ufs\",\"variant\":\"%s\","
                 "\"version\":%u,\"byte_order\":\"%s\"",
                 ufs_variant_name(summary), ufs_version(summary),
                 ufs_byte_order_name(summary));
    if (detailed != 0) {
        (void)printf(",\"superblock_offset\":%llu,\"magic_offset\":%llu",
                     (unsigned long long)summary->superblock_offset,
                     (unsigned long long)summary->magic_offset);
        if (summary->allocation_totals_known) {
            (void)printf(",\"allocation_totals\":\"recorded-superblock\","
                         "\"block_size\":%u,\"fragment_size\":%u,"
                         "\"filesystem_bytes\":%llu,\"free_bytes\":%llu,"
                         "\"used_bytes\":%llu,\"exact_allocation_available\":%s",
                         summary->block_size, summary->fragment_size,
                         (unsigned long long)(summary->filesystem_fragments *
                                              summary->fragment_size),
                         (unsigned long long)ufs_recorded_free_bytes(summary),
                         (unsigned long long)ufs_recorded_used_bytes(summary),
                         exact_ufs2_ready(summary) ? "true" : "false");
        }
    }
    (void)puts("}");
}

static void print_exact_analysis_json(const LdUfsAnalysis *analysis)
{
    const LdUfsSummary *summary = &analysis->summary;
    (void)printf(
        "{\"filesystem\":\"ufs\",\"variant\":\"%s\",\"version\":2,"
        "\"byte_order\":\"%s\",\"superblock_offset\":%llu,"
        "\"magic_offset\":%llu,\"map_accuracy\":\"exact-allocation\","
        "\"block_size\":%u,\"fragment_size\":%u,\"cylinder_groups\":%u,"
        "\"filesystem_bytes\":%llu,\"physical_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu}\n",
        ufs_variant_name(summary), ufs_byte_order_name(summary),
        (unsigned long long)summary->superblock_offset,
        (unsigned long long)summary->magic_offset,
        summary->block_size, summary->fragment_size, summary->cylinder_groups,
        (unsigned long long)analysis->filesystem_bytes,
        (unsigned long long)analysis->physical_bytes,
        (unsigned long long)(analysis->free_fragments_exact * summary->fragment_size),
        (unsigned long long)(analysis->used_fragments_exact * summary->fragment_size));
}

static void print_summary_map(const LdUfsSummary *summary, uint64_t size_bytes,
                              uint64_t requested_cells)
{
    uint64_t total_units = size_bytes / UFS_SUMMARY_UNIT_SIZE;
    if (size_bytes % UFS_SUMMARY_UNIT_SIZE != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;

    uint64_t cell_count = requested_cells;
    if (cell_count > total_units)
        cell_count = total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    const uint64_t rounded_bytes = total_units * UFS_SUMMARY_UNIT_SIZE;
    const uint64_t free_bytes = ufs_recorded_free_bytes(summary);
    const uint64_t used_bytes = ufs_recorded_used_bytes(summary);

    (void)printf("{\"schema\":1,\"backend\":\"read-only-domain\","
                 "\"filesystem\":\"ufs\",\"map_accuracy\":\"summary\","
                 "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
                 "\"total_bytes\":%llu,\"free_bytes\":%llu,\"used_bytes\":%llu,"
                 "\"unknown_bytes\":%llu,\"cells\":[",
                 UFS_SUMMARY_UNIT_SIZE,
                 (unsigned long long)total_units,
                 (unsigned long long)cell_count,
                 (unsigned long long)rounded_bytes,
                 (unsigned long long)free_bytes,
                 (unsigned long long)used_bytes,
                 (unsigned long long)rounded_bytes);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = (index * total_units) / cell_count;
        const uint64_t end_exclusive = ((index + 1U) * total_units) / cell_count;
        const uint64_t length = end_exclusive - start;
        if (index != 0U)
            (void)putchar(',');
        (void)printf("{\"start\":%llu,\"end\":%llu,\"free\":0,\"used\":0,"
                     "\"unknown\":%llu,\"bad\":0,\"fragmented\":0,"
                     "\"directory\":0}",
                     (unsigned long long)start,
                     (unsigned long long)(end_exclusive - 1U),
                     (unsigned long long)length);
    }

    (void)printf("],\"details\":{\"variant\":\"%s\","
                 "\"version\":%u,\"byte_order\":\"%s\","
                 "\"allocation_totals\":\"%s\","
                 "\"note\":\"UFS1 is identified read-only; physical cylinder-group allocation mapping is currently implemented for validated UFS2 only\"}}\n",
                 ufs_variant_name(summary), ufs_version(summary),
                 ufs_byte_order_name(summary),
                 summary->allocation_totals_known ? "recorded-superblock" : "unknown");
}

static int print_exact_map(const char *path, uint64_t requested_cells)
{
    LdUfsAnalysis analysis;
    char error[256];
    if (ufs_analyse_allocation(path, &analysis, NULL, 0U,
                               error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    uint64_t cell_count = requested_cells;
    if (cell_count > analysis.total_units)
        cell_count = analysis.total_units;
    if (cell_count == 0U)
        cell_count = 1U;
    if (cell_count > SIZE_MAX / sizeof(LdUfsMapCell)) {
        (void)fprintf(stderr, "%s: map cell count is too large\n", PROG);
        return 1;
    }

    LdUfsMapCell *cells = calloc((size_t)cell_count, sizeof(*cells));
    if (cells == NULL) {
        (void)fprintf(stderr, "%s: out of memory allocating map cells\n", PROG);
        return 1;
    }
    if (ufs_analyse_allocation(path, &analysis, cells, cell_count,
                               error, sizeof(error)) != 0) {
        free(cells);
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    const LdUfsSummary *summary = &analysis.summary;
    const uint64_t outside =
        analysis.total_units > summary->filesystem_fragments
            ? analysis.total_units - summary->filesystem_fragments : 0U;
    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\","
        "\"filesystem\":\"ufs\",\"map_accuracy\":\"exact-allocation\","
        "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
        "\"total_bytes\":%llu,\"filesystem_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,\"unknown_bytes\":%llu,"
        "\"cells\":[",
        summary->fragment_size,
        (unsigned long long)analysis.total_units,
        (unsigned long long)cell_count,
        (unsigned long long)(analysis.total_units * summary->fragment_size),
        (unsigned long long)analysis.filesystem_bytes,
        (unsigned long long)(analysis.free_fragments_exact * summary->fragment_size),
        (unsigned long long)(analysis.used_fragments_exact * summary->fragment_size),
        (unsigned long long)(outside * summary->fragment_size));

    for (uint64_t index = 0U; index < cell_count; ++index) {
        if (index != 0U)
            (void)putchar(',');
        (void)printf(
            "{\"start\":%llu,\"end\":%llu,\"free\":%llu,\"used\":%llu,"
            "\"unknown\":%llu,\"bad\":0,\"fragmented\":0,\"directory\":0}",
            (unsigned long long)cells[index].start,
            (unsigned long long)cells[index].end,
            (unsigned long long)cells[index].free_count,
            (unsigned long long)cells[index].used_count,
            (unsigned long long)cells[index].outside_count);
    }

    (void)printf(
        "],\"details\":{\"variant\":\"%s\",\"version\":2,"
        "\"byte_order\":\"%s\",\"cylinder_groups\":%u,"
        "\"allocation_basis\":\"validated UFS2 cylinder-group fragment bitmaps\","
        "\"fragmentation\":\"not-yet-decoded\"}}\n",
        ufs_variant_name(summary), ufs_byte_order_name(summary),
        summary->cylinder_groups);
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
        LdUfsSummary summary;
        char error[256];
        if (ufs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        if (exact_ufs2_ready(&summary))
            return print_exact_map(argv[2], requested_cells);

        uint64_t size_bytes = 0U;
        if (device_size_bytes(argv[2], &size_bytes) != 0) {
            (void)fprintf(stderr, "%s: cannot determine device size: %s\n",
                          PROG, strerror(errno));
            return 1;
        }
        print_summary_map(&summary, size_bytes, requested_cells);
        return 0;
    }

    if (argc != 3) {
        usage(stderr);
        return 2;
    }

    LdUfsSummary summary;
    char error[256];
    if (ufs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    if (strcmp(argv[1], "identify") == 0) {
        print_summary_json(&summary, 0);
        return 0;
    }
    if (strcmp(argv[1], "analyse-json") == 0) {
        if (exact_ufs2_ready(&summary)) {
            LdUfsAnalysis analysis;
            if (ufs_analyse_allocation(argv[2], &analysis, NULL, 0U,
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "%s: %s\n", PROG, error);
                return 1;
            }
            print_exact_analysis_json(&analysis);
        } else {
            print_summary_json(&summary, 1);
        }
        return 0;
    }

    usage(stderr);
    return 2;
}
