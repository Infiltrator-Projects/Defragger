// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"

#include "ld_io.h"
#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MINIX_SUPER_OFFSET 1024U
#define MINIX_SUPER_BYTES 64U
#define MINIX_V1_MAGIC 0x137fU
#define MINIX_V1_30_MAGIC 0x138fU
#define MINIX_V2_MAGIC 0x2468U
#define MINIX_V2_30_MAGIC 0x2478U
#define MINIX_V3_MAGIC 0x4d5aU
#define MINIX_MODE_TYPE 0170000U
#define MINIX_MODE_DIR 0040000U
#define MINIX_MODE_REG 0100000U
#define MINIX_DIRECT_ZONES 7U

typedef struct {
    int fd;
    const MinixSummary *summary;
    const uint8_t *zmap;
    size_t zmap_bytes;
} MinixWalk;

typedef struct {
    bool have_previous;
    uint32_t previous;
    bool fragmented;
    uint64_t count;
    MinixMapCell *cells;
    uint64_t cell_count;
    bool mark_fragmented;
    bool mark_directory;
} MinixVisit;

static void minix_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static uint16_t minix_u16(const uint8_t *p, bool little_endian)
{
    return little_endian ? infiltratr_load_le16(p) : infiltratr_load_be16(p);
}

static uint32_t minix_u32(const uint8_t *p, bool little_endian)
{
    return little_endian ? infiltratr_load_le32(p) : infiltratr_load_be32(p);
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

static int minix_size_bytes(int fd, uint64_t *bytes)
{
    struct stat status;
    if (fstat(fd, &status) != 0)
        return -1;
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        *bytes = (uint64_t)status.st_size;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, BLKGETSIZE64, bytes) == 0 ? 0 : -1;
}

static bool bitmap_test(const uint8_t *map, size_t map_bytes, uint64_t bit,
                        bool little_endian)
{
    if (little_endian) {
        const uint64_t byte = bit >> 3U;
        if (byte >= map_bytes)
            return false;
        return (map[byte] & (uint8_t)(1U << (bit & 7U))) != 0U;
    }

    const uint64_t word = bit >> 4U;
    const uint64_t offset = word * 2U;
    if (offset + 1U >= map_bytes)
        return false;
    const uint16_t value =
        (uint16_t)(((uint16_t)map[offset] << 8) | map[offset + 1U]);
    return (value & (uint16_t)(1U << (bit & 15U))) != 0U;
}

static bool zone_allocated(const MinixSummary *summary, const uint8_t *zmap,
                           size_t zmap_bytes, uint32_t zone)
{
    if (zone < summary->first_data_zone || zone >= summary->zone_count)
        return false;
    const uint64_t bit =
        (uint64_t)zone - summary->first_data_zone + 1U;
    return bitmap_test(zmap, zmap_bytes, bit, summary->little_endian);
}

static int read_exact_at(int fd, void *buffer, size_t bytes, uint64_t offset,
                         char *error, size_t error_size, const char *message)
{
    const ssize_t count = ld_pread_full(fd, buffer, bytes, offset);
    if (count != (ssize_t)bytes) {
        if (count < 0 && error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "%s: %s", message, strerror(errno));
        else
            minix_error(error, error_size, message);
        return -1;
    }
    return 0;
}

static uint64_t span_for_level(uint64_t entries, unsigned int level)
{
    uint64_t span = 1U;
    for (unsigned int index = 0U; index < level; ++index) {
        if (span > UINT64_MAX / entries)
            return UINT64_MAX;
        span *= entries;
    }
    return span;
}

static void consume_hole(uint64_t *remaining, uint64_t span)
{
    if (*remaining > span)
        *remaining -= span;
    else
        *remaining = 0U;
}

static MinixMapCell *cell_for_zone(MinixVisit *visit, uint32_t zone)
{
    if (visit->cells == NULL || visit->cell_count == 0U)
        return NULL;
    uint64_t lo = 0U;
    uint64_t hi = visit->cell_count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2U;
        if ((uint64_t)zone < visit->cells[mid].start)
            hi = mid;
        else if ((uint64_t)zone > visit->cells[mid].end)
            lo = mid + 1U;
        else
            return &visit->cells[mid];
    }
    return NULL;
}

static int visit_data_zone(const MinixWalk *walk, uint32_t zone,
                           MinixVisit *visit, char *error, size_t error_size)
{
    if (!zone_allocated(walk->summary, walk->zmap, walk->zmap_bytes, zone)) {
        minix_error(error, error_size,
                    "Minix inode references an unallocated or invalid data zone");
        return -1;
    }
    if (visit->have_previous && zone != visit->previous + 1U)
        visit->fragmented = true;
    visit->have_previous = true;
    visit->previous = zone;
    visit->count++;

    MinixMapCell *cell = cell_for_zone(visit, zone);
    if (cell != NULL) {
        if (visit->mark_fragmented)
            cell->fragmented_count++;
        if (visit->mark_directory)
            cell->directory_count++;
    }
    return 0;
}

static uint32_t read_pointer(const uint8_t *block, uint64_t index,
                             unsigned int pointer_bytes, bool little_endian)
{
    const uint8_t *p = block + index * pointer_bytes;
    return pointer_bytes == 2U ? minix_u16(p, little_endian)
                               : minix_u32(p, little_endian);
}

static int walk_indirect(const MinixWalk *walk, uint32_t zone,
                         unsigned int level, uint64_t *remaining,
                         MinixVisit *visit, char *error, size_t error_size)
{
    const unsigned int pointer_bytes =
        walk->summary->version == 1U ? 2U : 4U;
    const uint64_t entries = walk->summary->block_size / pointer_bytes;
    if (*remaining == 0U)
        return 0;
    if (zone == 0U) {
        consume_hole(remaining, span_for_level(entries, level));
        return 0;
    }
    if (!zone_allocated(walk->summary, walk->zmap, walk->zmap_bytes, zone)) {
        minix_error(error, error_size,
                    "Minix inode references an unallocated indirect zone");
        return -1;
    }

    uint8_t *block = malloc(walk->summary->block_size);
    if (block == NULL) {
        minix_error(error, error_size, "out of memory reading Minix indirect zone");
        return -1;
    }
    const uint64_t offset = (uint64_t)zone * walk->summary->zone_size;
    if (read_exact_at(walk->fd, block, walk->summary->block_size, offset,
                      error, error_size, "cannot read Minix indirect zone") != 0) {
        free(block);
        return -1;
    }

    int result = 0;
    for (uint64_t index = 0U; index < entries && *remaining != 0U; ++index) {
        const uint32_t child =
            read_pointer(block, index, pointer_bytes, walk->summary->little_endian);
        if (level == 1U) {
            if (child != 0U &&
                visit_data_zone(walk, child, visit, error, error_size) != 0) {
                result = -1;
                break;
            }
            (*remaining)--;
        } else if (walk_indirect(walk, child, level - 1U, remaining, visit,
                                 error, error_size) != 0) {
            result = -1;
            break;
        }
    }
    free(block);
    return result;
}

static int walk_inode_data(const MinixWalk *walk, const uint8_t *inode,
                           uint64_t size_bytes, MinixVisit *visit,
                           char *error, size_t error_size)
{
    const unsigned int pointer_bytes =
        walk->summary->version == 1U ? 2U : 4U;
    const size_t zone_offset = walk->summary->version == 1U ? 14U : 24U;
    const unsigned int indirect_levels =
        walk->summary->version == 1U ? 2U : 3U;
    uint64_t remaining =
        (size_bytes + walk->summary->zone_size - 1U) / walk->summary->zone_size;

    for (unsigned int index = 0U;
         index < MINIX_DIRECT_ZONES && remaining != 0U; ++index) {
        const uint32_t zone = read_pointer(
            inode + zone_offset, index, pointer_bytes, walk->summary->little_endian);
        if (zone != 0U &&
            visit_data_zone(walk, zone, visit, error, error_size) != 0)
            return -1;
        remaining--;
    }

    for (unsigned int level = 1U;
         level <= indirect_levels && remaining != 0U; ++level) {
        const uint64_t pointer_index = MINIX_DIRECT_ZONES + (level - 1U);
        const uint32_t zone = read_pointer(
            inode + zone_offset, pointer_index, pointer_bytes,
            walk->summary->little_endian);
        if (walk_indirect(walk, zone, level, &remaining, visit,
                          error, error_size) != 0)
            return -1;
    }
    if (remaining != 0U) {
        minix_error(error, error_size,
                    "Minix inode size exceeds its addressable zone tree");
        return -1;
    }
    return 0;
}

static int load_bitmaps(int fd, const MinixSummary *summary,
                        uint8_t **imap_out, size_t *imap_bytes_out,
                        uint8_t **zmap_out, size_t *zmap_bytes_out,
                        char *error, size_t error_size)
{
    if (summary->log_zone_size != 0U) {
        minix_error(error, error_size,
                    "exact Minix analysis requires zone size equal to block size");
        return -1;
    }
    if ((uint64_t)summary->imap_blocks > SIZE_MAX / summary->block_size ||
        (uint64_t)summary->zmap_blocks > SIZE_MAX / summary->block_size) {
        minix_error(error, error_size, "Minix bitmap size overflows address space");
        return -1;
    }
    const size_t imap_bytes =
        (size_t)summary->imap_blocks * summary->block_size;
    const size_t zmap_bytes =
        (size_t)summary->zmap_blocks * summary->block_size;
    uint8_t *imap = malloc(imap_bytes);
    uint8_t *zmap = malloc(zmap_bytes);
    if (imap == NULL || zmap == NULL) {
        free(imap);
        free(zmap);
        minix_error(error, error_size, "out of memory reading Minix bitmaps");
        return -1;
    }

    const uint64_t imap_offset = (uint64_t)summary->block_size * 2U;
    const uint64_t zmap_offset = imap_offset + imap_bytes;
    if (read_exact_at(fd, imap, imap_bytes, imap_offset, error, error_size,
                      "cannot read Minix inode bitmap") != 0 ||
        read_exact_at(fd, zmap, zmap_bytes, zmap_offset, error, error_size,
                      "cannot read Minix zone bitmap") != 0) {
        free(imap);
        free(zmap);
        return -1;
    }

    const uint64_t inode_bits = (uint64_t)imap_bytes * 8U;
    const uint64_t zone_bits = (uint64_t)zmap_bytes * 8U;
    const uint64_t needed_zone_bits =
        (uint64_t)summary->zone_count - summary->first_data_zone + 1U;
    if (inode_bits <= summary->inode_count || zone_bits < needed_zone_bits) {
        free(imap);
        free(zmap);
        minix_error(error, error_size, "Minix allocation bitmap is too small");
        return -1;
    }
    if (!bitmap_test(imap, imap_bytes, 0U, summary->little_endian) ||
        !bitmap_test(zmap, zmap_bytes, 0U, summary->little_endian)) {
        free(imap);
        free(zmap);
        minix_error(error, error_size, "Minix reserved bitmap bit is not allocated");
        return -1;
    }

    *imap_out = imap;
    *imap_bytes_out = imap_bytes;
    *zmap_out = zmap;
    *zmap_bytes_out = zmap_bytes;
    return 0;
}

static void init_cells(MinixMapCell *cells, uint64_t cell_count,
                       uint64_t total_units, uint32_t filesystem_units)
{
    if (cells == NULL || cell_count == 0U)
        return;
    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = index * total_units / cell_count;
        uint64_t end_exclusive = (index + 1U) * total_units / cell_count;
        if (end_exclusive <= start)
            end_exclusive = start + 1U;
        cells[index].start = start;
        cells[index].end = end_exclusive - 1U;
        cells[index].free_count = 0U;
        cells[index].used_count = 0U;
        cells[index].outside_count = 0U;
        cells[index].fragmented_count = 0U;
        cells[index].directory_count = 0U;
        const uint64_t outside_start =
            start > filesystem_units ? start : filesystem_units;
        if (end_exclusive > outside_start)
            cells[index].outside_count = end_exclusive - outside_start;
    }
}

static MinixMapCell *analysis_cell(MinixMapCell *cells, uint64_t cell_count,
                                   uint64_t unit)
{
    if (cells == NULL || cell_count == 0U)
        return NULL;
    for (uint64_t index = 0U; index < cell_count; ++index) {
        if (unit >= cells[index].start && unit <= cells[index].end)
            return &cells[index];
    }
    return NULL;
}

static int scan_allocation(const MinixSummary *summary, const uint8_t *zmap,
                           size_t zmap_bytes, MinixAnalysis *analysis,
                           MinixMapCell *cells, uint64_t cell_count,
                           char *error, size_t error_size)
{
    uint64_t free_zones = 0U;
    uint64_t used_zones = summary->first_data_zone;

    for (uint64_t unit = 0U; unit < summary->first_data_zone; ++unit) {
        MinixMapCell *cell = analysis_cell(cells, cell_count, unit);
        if (cell != NULL)
            cell->used_count++;
    }

    for (uint32_t zone = summary->first_data_zone;
         zone < summary->zone_count; ++zone) {
        const bool allocated = zone_allocated(summary, zmap, zmap_bytes, zone);
        MinixMapCell *cell = analysis_cell(cells, cell_count, zone);
        if (allocated) {
            used_zones++;
            if (cell != NULL)
                cell->used_count++;
        } else {
            free_zones++;
            if (cell != NULL)
                cell->free_count++;
        }
    }
    if (free_zones + used_zones != summary->zone_count) {
        minix_error(error, error_size, "Minix allocation accounting is incomplete");
        return -1;
    }
    analysis->free_zones = free_zones;
    analysis->used_zones = used_zones;
    return 0;
}

static int scan_inodes(int fd, const MinixSummary *summary,
                       const uint8_t *imap, size_t imap_bytes,
                       const uint8_t *zmap, size_t zmap_bytes,
                       MinixAnalysis *analysis, MinixMapCell *cells,
                       uint64_t cell_count, char *error, size_t error_size)
{
    const size_t inode_size = summary->version == 1U ? 32U : 64U;
    const uint64_t inode_table_block =
        2U + summary->imap_blocks + summary->zmap_blocks;
    const uint64_t inode_table_bytes =
        (uint64_t)summary->inode_count * inode_size;
    const uint64_t inode_table_blocks =
        (inode_table_bytes + summary->block_size - 1U) / summary->block_size;
    if (inode_table_block + inode_table_blocks > summary->first_data_zone) {
        minix_error(error, error_size,
                    "Minix inode table overlaps the first data zone");
        return -1;
    }

    uint8_t inode[64];
    MinixWalk walk = {
        .fd = fd,
        .summary = summary,
        .zmap = zmap,
        .zmap_bytes = zmap_bytes,
    };

    for (uint32_t ino = 1U; ino <= summary->inode_count; ++ino) {
        if (!bitmap_test(imap, imap_bytes, ino, summary->little_endian))
            continue;
        const uint64_t offset =
            inode_table_block * summary->block_size +
            (uint64_t)(ino - 1U) * inode_size;
        if (read_exact_at(fd, inode, inode_size, offset, error, error_size,
                          "cannot read allocated Minix inode") != 0)
            return -1;

        const uint16_t mode = minix_u16(inode, summary->little_endian);
        const uint16_t type = (uint16_t)(mode & MINIX_MODE_TYPE);
        if (type != MINIX_MODE_REG && type != MINIX_MODE_DIR)
            continue;
        const uint64_t size_bytes =
            minix_u32(inode + (summary->version == 1U ? 4U : 8U),
                      summary->little_endian);

        MinixVisit first = {0};
        if (walk_inode_data(&walk, inode, size_bytes, &first,
                            error, error_size) != 0)
            return -1;

        const bool directory = type == MINIX_MODE_DIR;
        if (directory) {
            analysis->directories++;
            if (first.fragmented)
                analysis->fragmented_directories++;
        } else {
            analysis->regular_files++;
            if (first.fragmented)
                analysis->fragmented_files++;
        }

        if (directory || first.fragmented) {
            MinixVisit second = {
                .cells = cells,
                .cell_count = cell_count,
                .mark_fragmented = first.fragmented,
                .mark_directory = directory,
            };
            if (walk_inode_data(&walk, inode, size_bytes, &second,
                                error, error_size) != 0)
                return -1;
        }
    }
    return 0;
}

static int minix_read_summary_fd(int fd, MinixSummary *summary,
                                 char *error, size_t error_size)
{
    uint8_t superblock[MINIX_SUPER_BYTES];
    if (read_exact_at(fd, superblock, sizeof(superblock), MINIX_SUPER_OFFSET,
                      error, error_size,
                      "Minix volume is shorter than its superblock") != 0)
        return -1;
    return minix_parse(superblock, summary, error, error_size);
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
    const int result = minix_read_summary_fd(fd, summary, error, error_size);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return result;
}

int minix_analyse(const char *path, MinixAnalysis *analysis,
                  MinixMapCell *cells, uint64_t cell_count,
                  char *error, size_t error_size)
{
    if (path == NULL || analysis == NULL) {
        minix_error(error, error_size, "invalid Minix analysis request");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    MinixSummary summary;
    if (minix_read_summary_fd(fd, &summary, error, error_size) != 0) {
        (void)close(fd);
        return -1;
    }

    uint64_t physical = 0U;
    if (minix_size_bytes(fd, &physical) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    const uint64_t filesystem_bytes =
        (uint64_t)summary.zone_count * summary.zone_size;
    if (filesystem_bytes > physical) {
        minix_error(error, error_size,
                    "Minix filesystem geometry exceeds the target size");
        (void)close(fd);
        return -1;
    }

    uint8_t *imap = NULL;
    uint8_t *zmap = NULL;
    size_t imap_bytes = 0U;
    size_t zmap_bytes = 0U;
    if (load_bitmaps(fd, &summary, &imap, &imap_bytes, &zmap, &zmap_bytes,
                     error, error_size) != 0) {
        (void)close(fd);
        return -1;
    }

    const uint64_t physical_units =
        (physical + summary.zone_size - 1U) / summary.zone_size;
    const uint64_t total_units =
        physical_units > summary.zone_count ? physical_units : summary.zone_count;
    analysis->summary = summary;
    analysis->physical_bytes = physical;
    analysis->filesystem_bytes = filesystem_bytes;
    analysis->total_units = total_units;
    init_cells(cells, cell_count, total_units, summary.zone_count);

    int result = scan_allocation(&summary, zmap, zmap_bytes, analysis,
                                 cells, cell_count, error, error_size);
    if (result == 0)
        result = scan_inodes(fd, &summary, imap, imap_bytes, zmap, zmap_bytes,
                             analysis, cells, cell_count, error, error_size);

    free(imap);
    free(zmap);
    (void)close(fd);
    if (result == 0 && error != NULL && error_size != 0U)
        error[0] = '\0';
    return result;
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
