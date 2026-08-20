// SPDX-License-Identifier: GPL-3.0-or-later
#include "ufs_native.h"

#include "ld_io.h"

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

#define UFS_WINDOW_BYTES 8192U
#define UFS_MIN_WINDOW 512U
#define UFS_DISK_STRUCT_BYTES 1376U
#define UFS_DISK_MAGIC_OFFSET 1372U
#define UFS_DISK_SBLKNO_OFFSET 8U
#define UFS_DISK_CBLKNO_OFFSET 12U
#define UFS_DISK_BSIZE_OFFSET 48U
#define UFS_DISK_FSIZE_OFFSET 52U
#define UFS_DISK_FRAG_OFFSET 56U
#define UFS_DISK_CGSIZE_OFFSET 160U
#define UFS_DISK_IPG_OFFSET 188U
#define UFS_DISK_FPG_OFFSET 192U
#define UFS_DISK_NCG_OFFSET 44U
#define UFS_DISK_CSTOTAL_NBFREE_OFFSET 1016U
#define UFS_DISK_CSTOTAL_NFFREE_OFFSET 1032U
#define UFS_DISK_SIZE_OFFSET 1080U
#define UFS_DISK_DSIZE_OFFSET 1088U

#define UFS_CG_MIN_BYTES 168U
#define UFS_CG_MAGIC_OFFSET 4U
#define UFS_CG_INDEX_OFFSET 12U
#define UFS_CG_NDBLK_OFFSET 20U
#define UFS_CG_FREEOFF_OFFSET 96U
#define UFS_CG_MAGIC 0x00090255U

static const uint64_t UFS_CANDIDATES[] = {8192U, 65536U, 262144U};

static const uint8_t UFS1_LE_MAGIC[4] = {0x54U, 0x19U, 0x01U, 0x00U};
static const uint8_t UFS1_BE_MAGIC[4] = {0x00U, 0x01U, 0x19U, 0x54U};
static const uint8_t UFS2_LE_MAGIC[4] = {0x19U, 0x01U, 0x54U, 0x19U};
static const uint8_t UFS2_BE_MAGIC[4] = {0x19U, 0x54U, 0x01U, 0x19U};

typedef struct {
    const uint8_t *magic;
    LdUfsVariant variant;
} UfsMagic;

static const UfsMagic UFS_MAGICS[] = {
    {UFS1_LE_MAGIC, LD_UFS_VARIANT_UFS1_LE},
    {UFS1_BE_MAGIC, LD_UFS_VARIANT_UFS1_BE},
    {UFS2_LE_MAGIC, LD_UFS_VARIANT_UFS2_LE},
    {UFS2_BE_MAGIC, LD_UFS_VARIANT_UFS2_BE},
};

static void ufs_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static bool find_magic(const uint8_t *data, size_t length, const uint8_t magic[4],
                       size_t *position)
{
    if (length < 4U)
        return false;
    for (size_t index = 0U; index <= length - 4U; ++index) {
        if (memcmp(data + index, magic, 4U) == 0) {
            *position = index;
            return true;
        }
    }
    return false;
}

static bool ufs_little_endian(LdUfsVariant variant)
{
    return variant == LD_UFS_VARIANT_UFS1_LE ||
           variant == LD_UFS_VARIANT_UFS2_LE;
}

static uint32_t read_u32(const uint8_t *data, bool little)
{
    if (little) {
        return (uint32_t)data[0] |
               ((uint32_t)data[1] << 8) |
               ((uint32_t)data[2] << 16) |
               ((uint32_t)data[3] << 24);
    }
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static uint64_t read_u64(const uint8_t *data, bool little)
{
    uint64_t value = 0U;
    if (little) {
        for (unsigned int index = 0U; index < 8U; ++index)
            value |= (uint64_t)data[index] << (index * 8U);
    } else {
        for (unsigned int index = 0U; index < 8U; ++index)
            value = (value << 8) | data[index];
    }
    return value;
}

static bool power_of_two_u32(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static void decode_ufs2_allocation(const uint8_t *window, size_t length,
                                   size_t magic_position, LdUfsSummary *summary)
{
    if (summary->variant != LD_UFS_VARIANT_UFS2_LE &&
        summary->variant != LD_UFS_VARIANT_UFS2_BE)
        return;
    if (magic_position < UFS_DISK_MAGIC_OFFSET)
        return;

    const size_t base = magic_position - UFS_DISK_MAGIC_OFFSET;
    if (base > length || length - base < UFS_DISK_STRUCT_BYTES)
        return;

    const bool little = ufs_little_endian(summary->variant);
    const uint32_t sblkno = read_u32(window + base + UFS_DISK_SBLKNO_OFFSET, little);
    const uint32_t cblkno = read_u32(window + base + UFS_DISK_CBLKNO_OFFSET, little);
    const uint32_t block_size = read_u32(window + base + UFS_DISK_BSIZE_OFFSET, little);
    const uint32_t fragment_size = read_u32(window + base + UFS_DISK_FSIZE_OFFSET, little);
    const uint32_t fragments_per_block = read_u32(window + base + UFS_DISK_FRAG_OFFSET, little);
    const uint32_t cylinder_groups = read_u32(window + base + UFS_DISK_NCG_OFFSET, little);
    const uint32_t cylinder_group_size =
        read_u32(window + base + UFS_DISK_CGSIZE_OFFSET, little);
    const uint32_t inodes_per_group =
        read_u32(window + base + UFS_DISK_IPG_OFFSET, little);
    const uint32_t fragments_per_group =
        read_u32(window + base + UFS_DISK_FPG_OFFSET, little);
    const uint64_t free_blocks =
        read_u64(window + base + UFS_DISK_CSTOTAL_NBFREE_OFFSET, little);
    const uint64_t free_fragments =
        read_u64(window + base + UFS_DISK_CSTOTAL_NFFREE_OFFSET, little);
    const uint64_t filesystem_fragments =
        read_u64(window + base + UFS_DISK_SIZE_OFFSET, little);
    const uint64_t data_fragments =
        read_u64(window + base + UFS_DISK_DSIZE_OFFSET, little);

    if (!power_of_two_u32(block_size) || !power_of_two_u32(fragment_size) ||
        block_size < 4096U || fragment_size < 512U || fragment_size > block_size ||
        fragments_per_block == 0U || fragments_per_block > 8U ||
        fragment_size > UINT32_MAX / fragments_per_block ||
        fragment_size * fragments_per_block != block_size ||
        data_fragments == 0U || filesystem_fragments < data_fragments ||
        free_blocks > UINT64_MAX / fragments_per_block)
        return;

    const uint64_t free_data_fragments =
        free_blocks * fragments_per_block + free_fragments;
    if (free_data_fragments < free_fragments ||
        free_data_fragments > data_fragments ||
        data_fragments > UINT64_MAX / fragment_size)
        return;

    summary->allocation_totals_known = true;
    summary->block_size = block_size;
    summary->fragment_size = fragment_size;
    summary->fragments_per_block = fragments_per_block;
    summary->filesystem_fragments = filesystem_fragments;
    summary->data_fragments = data_fragments;
    summary->free_blocks = free_blocks;
    summary->free_fragments = free_fragments;

    if (cylinder_groups == 0U || cylinder_group_size < UFS_CG_MIN_BYTES ||
        cylinder_group_size > block_size || fragments_per_group == 0U ||
        inodes_per_group == 0U || cblkno >= fragments_per_group ||
        sblkno >= fragments_per_group ||
        (uint64_t)cylinder_groups * fragments_per_group < filesystem_fragments ||
        (uint64_t)(cylinder_groups - 1U) * fragments_per_group >=
            filesystem_fragments)
        return;

    summary->cylinder_geometry_known = true;
    summary->cylinder_groups = cylinder_groups;
    summary->cylinder_group_size = cylinder_group_size;
    summary->fragments_per_group = fragments_per_group;
    summary->inodes_per_group = inodes_per_group;
    summary->cylinder_block_fragment = cblkno;
}

static int ufs_size_bytes(int fd, uint64_t *bytes)
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

static int read_exact_at(int fd, void *buffer, size_t bytes, uint64_t offset,
                         char *error, size_t error_size, const char *message)
{
    const ssize_t count = ld_pread_full(fd, buffer, bytes, offset);
    if (count != (ssize_t)bytes) {
        if (count < 0 && error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "%s: %s", message, strerror(errno));
        else
            ufs_error(error, error_size, message);
        return -1;
    }
    return 0;
}

int ufs_read_summary(const char *path, LdUfsSummary *summary,
                     char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        ufs_error(error, error_size, "invalid UFS summary request");
        return -1;
    }
    memset(summary, 0, sizeof(*summary));

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    uint8_t window[UFS_WINDOW_BYTES];
    int result = -1;
    for (size_t candidate_index = 0U;
         candidate_index < sizeof(UFS_CANDIDATES) / sizeof(UFS_CANDIDATES[0]);
         ++candidate_index) {
        const uint64_t offset = UFS_CANDIDATES[candidate_index];
        const ssize_t count = ld_pread_full(fd, window, sizeof(window), offset);
        if (count < 0) {
            if (errno == EINVAL || errno == EIO)
                continue;
            if (error != NULL && error_size != 0U)
                (void)snprintf(error, error_size, "read: %s", strerror(errno));
            (void)close(fd);
            return -1;
        }
        if ((size_t)count < UFS_MIN_WINDOW)
            continue;

        for (size_t magic_index = 0U;
             magic_index < sizeof(UFS_MAGICS) / sizeof(UFS_MAGICS[0]);
             ++magic_index) {
            size_t position = 0U;
            if (!find_magic(window, (size_t)count, UFS_MAGICS[magic_index].magic,
                            &position))
                continue;
            summary->variant = UFS_MAGICS[magic_index].variant;
            summary->superblock_offset = offset;
            summary->magic_offset = offset + (uint64_t)position;
            decode_ufs2_allocation(window, (size_t)count, position, summary);
            result = 0;
            break;
        }
        if (result == 0)
            break;
    }

    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;

    if (result != 0) {
        ufs_error(error, error_size, "not a recognised UFS volume");
        return -1;
    }
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

static void init_cells(LdUfsMapCell *cells, uint64_t cell_count,
                       uint64_t total_units, uint64_t filesystem_units)
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
        const uint64_t outside_start =
            start > filesystem_units ? start : filesystem_units;
        if (end_exclusive > outside_start)
            cells[index].outside_count = end_exclusive - outside_start;
    }
}

static LdUfsMapCell *cell_for_fragment(LdUfsMapCell *cells, uint64_t cell_count,
                                       uint64_t fragment)
{
    if (cells == NULL || cell_count == 0U)
        return NULL;
    uint64_t lo = 0U;
    uint64_t hi = cell_count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2U;
        if (fragment < cells[mid].start)
            hi = mid;
        else if (fragment > cells[mid].end)
            lo = mid + 1U;
        else
            return &cells[mid];
    }
    return NULL;
}

int ufs_analyse_allocation(const char *path, LdUfsAnalysis *analysis,
                           LdUfsMapCell *cells, uint64_t cell_count,
                           char *error, size_t error_size)
{
    if (path == NULL || analysis == NULL) {
        ufs_error(error, error_size, "invalid UFS allocation analysis request");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));
    if (ufs_read_summary(path, &analysis->summary, error, error_size) != 0)
        return -1;

    LdUfsSummary *summary = &analysis->summary;
    if ((summary->variant != LD_UFS_VARIANT_UFS2_LE &&
         summary->variant != LD_UFS_VARIANT_UFS2_BE) ||
        !summary->allocation_totals_known || !summary->cylinder_geometry_known) {
        ufs_error(error, error_size,
                  "exact UFS allocation mapping currently requires validated UFS2 cylinder-group geometry");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }
    uint64_t physical = 0U;
    if (ufs_size_bytes(fd, &physical) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    if (summary->filesystem_fragments > UINT64_MAX / summary->fragment_size) {
        ufs_error(error, error_size, "UFS2 filesystem size overflows");
        (void)close(fd);
        return -1;
    }
    const uint64_t filesystem_bytes =
        summary->filesystem_fragments * summary->fragment_size;
    if (filesystem_bytes > physical) {
        ufs_error(error, error_size,
                  "UFS2 filesystem geometry exceeds the target size");
        (void)close(fd);
        return -1;
    }

    const uint64_t physical_units =
        (physical + summary->fragment_size - 1U) / summary->fragment_size;
    const uint64_t total_units =
        physical_units > summary->filesystem_fragments
            ? physical_units : summary->filesystem_fragments;
    analysis->physical_bytes = physical;
    analysis->filesystem_bytes = filesystem_bytes;
    analysis->total_units = total_units;
    init_cells(cells, cell_count, total_units, summary->filesystem_fragments);

    uint8_t *cg = malloc(summary->cylinder_group_size);
    if (cg == NULL) {
        ufs_error(error, error_size, "out of memory reading UFS2 cylinder group");
        (void)close(fd);
        return -1;
    }

    const bool little = ufs_little_endian(summary->variant);
    uint64_t free_fragments = 0U;
    uint64_t used_fragments = 0U;
    uint64_t covered = 0U;

    for (uint32_t group = 0U; group < summary->cylinder_groups; ++group) {
        const uint64_t group_base =
            (uint64_t)group * summary->fragments_per_group;
        if (group_base >= summary->filesystem_fragments) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "UFS2 cylinder group begins beyond filesystem");
            return -1;
        }
        const uint64_t cg_fragment =
            group_base + summary->cylinder_block_fragment;
        if (cg_fragment > UINT64_MAX / summary->fragment_size) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "UFS2 cylinder group offset overflows");
            return -1;
        }
        const uint64_t cg_offset = cg_fragment * summary->fragment_size;
        if (cg_offset > physical ||
            summary->cylinder_group_size > physical - cg_offset ||
            read_exact_at(fd, cg, summary->cylinder_group_size, cg_offset,
                          error, error_size,
                          "cannot read UFS2 cylinder group") != 0) {
            free(cg);
            (void)close(fd);
            return -1;
        }

        if (read_u32(cg + UFS_CG_MAGIC_OFFSET, little) != UFS_CG_MAGIC ||
            read_u32(cg + UFS_CG_INDEX_OFFSET, little) != group) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "invalid UFS2 cylinder-group header");
            return -1;
        }
        const uint32_t group_fragments =
            read_u32(cg + UFS_CG_NDBLK_OFFSET, little);
        const uint32_t free_offset =
            read_u32(cg + UFS_CG_FREEOFF_OFFSET, little);
        const uint64_t remaining =
            summary->filesystem_fragments - group_base;
        const uint64_t expected =
            remaining < summary->fragments_per_group
                ? remaining : summary->fragments_per_group;
        if (group_fragments != expected ||
            free_offset < UFS_CG_MIN_BYTES ||
            (uint64_t)free_offset + (group_fragments + 7U) / 8U >
                summary->cylinder_group_size) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "invalid UFS2 cylinder-group allocation map");
            return -1;
        }

        const uint8_t *free_map = cg + free_offset;
        for (uint32_t local = 0U; local < group_fragments; ++local) {
            const uint64_t global = group_base + local;
            const bool is_free =
                (free_map[local >> 3U] &
                 (uint8_t)(1U << (local & 7U))) != 0U;
            LdUfsMapCell *cell =
                cell_for_fragment(cells, cell_count, global);
            if (is_free) {
                free_fragments++;
                if (cell != NULL)
                    cell->free_count++;
            } else {
                used_fragments++;
                if (cell != NULL)
                    cell->used_count++;
            }
        }
        covered += group_fragments;
    }

    free(cg);
    (void)close(fd);

    if (covered != summary->filesystem_fragments ||
        free_fragments + used_fragments != summary->filesystem_fragments) {
        ufs_error(error, error_size, "UFS2 cylinder groups do not cover filesystem");
        return -1;
    }
    const uint64_t recorded_free =
        summary->free_blocks * summary->fragments_per_block +
        summary->free_fragments;
    if (free_fragments != recorded_free) {
        ufs_error(error, error_size,
                  "UFS2 cylinder-group free map disagrees with superblock totals");
        return -1;
    }

    analysis->free_fragments_exact = free_fragments;
    analysis->used_fragments_exact = used_fragments;
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

bool ufs_probe(const char *path)
{
    LdUfsSummary summary;
    return ufs_read_summary(path, &summary, NULL, 0U) == 0;
}

const char *ufs_variant_name(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    switch (summary->variant) {
    case LD_UFS_VARIANT_UFS1_LE:
        return "ufs1-le";
    case LD_UFS_VARIANT_UFS1_BE:
        return "ufs1-be";
    case LD_UFS_VARIANT_UFS2_LE:
        return "ufs2-le";
    case LD_UFS_VARIANT_UFS2_BE:
        return "ufs2-be";
    default:
        return "unknown";
    }
}

const char *ufs_byte_order_name(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return ufs_little_endian(summary->variant) ? "little" : "big";
}

unsigned int ufs_version(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return 0U;
    return summary->variant == LD_UFS_VARIANT_UFS1_LE ||
                   summary->variant == LD_UFS_VARIANT_UFS1_BE
               ? 1U
               : 2U;
}

uint64_t ufs_recorded_data_bytes(const LdUfsSummary *summary)
{
    if (summary == NULL || !summary->allocation_totals_known)
        return 0U;
    return summary->data_fragments * summary->fragment_size;
}

uint64_t ufs_recorded_free_bytes(const LdUfsSummary *summary)
{
    if (summary == NULL || !summary->allocation_totals_known)
        return 0U;
    const uint64_t fragments =
        summary->free_blocks * summary->fragments_per_block + summary->free_fragments;
    return fragments * summary->fragment_size;
}

uint64_t ufs_recorded_used_bytes(const LdUfsSummary *summary)
{
    const uint64_t data_bytes = ufs_recorded_data_bytes(summary);
    const uint64_t free_bytes = ufs_recorded_free_bytes(summary);
    return data_bytes >= free_bytes ? data_bytes - free_bytes : 0U;
}
