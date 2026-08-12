// SPDX-License-Identifier: GPL-3.0-or-later
#include "xfs_native.h"
#include "ld_io.h"
#include "ld_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define XFS_SB_MAGIC "XFSB"
#define XFS_AGF_MAGIC "XAGF"
#define XFS_AGI_MAGIC "XAGI"
#define XFS_ABTB_MAGIC "ABTB"
#define XFS_ABTB_CRC_MAGIC "AB3B"
#define XFS_IBT_MAGIC "IABT"
#define XFS_IBT_CRC_MAGIC "IAB3"
#define XFS_BMAP_MAGIC "BMAP"
#define XFS_BMAP_CRC_MAGIC "BMA3"
#define XFS_DINODE_MAGIC UINT16_C(0x494e)

#define XFS_MODE_TYPE_MASK UINT16_C(0170000)
#define XFS_MODE_REG UINT16_C(0100000)
#define XFS_MODE_DIR UINT16_C(0040000)

typedef int (*LeafCallback)(const uint8_t *records, size_t bytes, void *context, char **error);

typedef struct {
    int fd;
    uint64_t size;
    XfsGeometry *g;
} XfsReader;

typedef struct {
    XfsReader *reader;
    uint32_t agno;
    const char *magic_old;
    const char *magic_crc;
    size_t key_size;
    size_t ptr_size;
    size_t record_size;
    uint64_t blocks_read;
    XfsU64Vec visited;
} ShortWalker;

typedef struct {
    XfsReader *reader;
    XfsGeometry *g;
    uint64_t blocks_read;
    XfsU64Vec visited;
} BmapWalker;

typedef struct {
    XfsCatalogue *catalogue;
    uint32_t agno;
    uint64_t ag_len;
    uint64_t previous_end;
    bool have_previous;
    uint64_t actual_free;
    uint64_t extents;
} FreeContext;

typedef struct {
    uint32_t startino;
    uint16_t holemask;
    uint64_t free_mask;
} InodeChunk;

typedef struct {
    InodeChunk *items;
    size_t count;
    size_t capacity;
} InodeChunkVec;

typedef struct {
    InodeChunkVec *chunks;
    bool sparse;
} InobtContext;

static void *vector_grow(void *items, size_t *capacity, size_t count, size_t item_size) {
    if (count < *capacity) return items;
    size_t next = *capacity == 0 ? 16U : *capacity * 2U;
    if (next > SIZE_MAX / item_size) ld_die("XFS native vector overflow");
    items = ld_xrealloc(items, next * item_size);
    *capacity = next;
    return items;
}

static void object_push(XfsObjectVec *vec, XfsObject object) {
    vec->items = vector_grow(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = object;
}

static void ag_detail_push(XfsAgDetailVec *vec, XfsAgDetail detail) {
    vec->items = vector_grow(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = detail;
}

static void inode_chunk_push(InodeChunkVec *vec, InodeChunk chunk) {
    vec->items = vector_grow(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = chunk;
}

static bool u64_seen(const XfsU64Vec *vec, uint64_t value) {
    for (size_t index = 0; index < vec->count; ++index)
        if (vec->items[index] == value) return true;
    return false;
}

static uint64_t reader_size(int fd, const struct stat *status) {
    if (S_ISREG(status->st_mode)) return (uint64_t)status->st_size;
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0) return bytes;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end >= 0) return (uint64_t)end;
    return 0;
}

static int reader_open(const char *path, XfsReader *reader, char **error) {
    memset(reader, 0, sizeof(*reader));
    reader->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (reader->fd < 0) {
        xfs_set_error(error, "cannot open XFS target: %s", strerror(errno));
        return -1;
    }
    struct stat status;
    if (fstat(reader->fd, &status) != 0) {
        xfs_set_error(error, "cannot stat XFS target: %s", strerror(errno));
        close(reader->fd);
        reader->fd = -1;
        return -1;
    }
    reader->size = reader_size(reader->fd, &status);
    return 0;
}

static void reader_close(XfsReader *reader) {
    if (reader->fd >= 0) close(reader->fd);
    reader->fd = -1;
}

static int read_exact(XfsReader *reader, void *buffer, size_t length, uint64_t offset,
                      const char *what, char **error) {
    ssize_t count = ld_pread_full(reader->fd, buffer, length, offset);
    if (count < 0) {
        xfs_set_error(error, "%s: %s", what, strerror(errno));
        return -1;
    }
    if ((size_t)count != length) {
        xfs_set_error(error, "%s is truncated", what);
        return -1;
    }
    return 0;
}

static int geometry_parse(const uint8_t *sb, uint64_t reader_bytes, XfsGeometry *g, char **error) {
    memset(g, 0, sizeof(*g));
    if (memcmp(sb, XFS_SB_MAGIC, 4) != 0) {
        xfs_set_error(error, "not an XFS filesystem");
        return -1;
    }
    g->block_size = xfs_be32(sb + 4);
    g->dblocks = xfs_be64(sb + 8);
    g->rblocks = xfs_be64(sb + 16);
    memcpy(g->uuid, sb + 32, 16);
    g->logstart = xfs_be64(sb + 48);
    g->rootino = xfs_be64(sb + 56);
    g->rbmino = xfs_be64(sb + 64);
    g->rsumino = xfs_be64(sb + 72);
    g->agblocks = xfs_be32(sb + 84);
    g->agcount = xfs_be32(sb + 88);
    g->logblocks = xfs_be32(sb + 96);
    g->version = (uint16_t)(xfs_be16(sb + 100) & UINT16_C(0x000f));
    g->sector_size = xfs_be16(sb + 102);
    g->inode_size = xfs_be16(sb + 104);
    g->inopblock = xfs_be16(sb + 106);
    g->blocklog = sb[120];
    g->sectlog = sb[121];
    g->inodelog = sb[122];
    g->inopblog = sb[123];
    g->agblklog = sb[124];
    g->icount = xfs_be64(sb + 128);
    g->ifree = xfs_be64(sb + 136);
    g->fdblocks = xfs_be64(sb + 144);
    g->uquotino = xfs_be64(sb + 160);
    g->gquotino = xfs_be64(sb + 168);
    g->features_ro_compat = xfs_be32(sb + 212);
    g->incompat = xfs_be32(sb + 216);
    g->log_incompat = xfs_be32(sb + 220);
    g->pquotino = xfs_be64(sb + 232);
    if ((g->incompat & XFS_SB_FEAT_INCOMPAT_META_UUID) != 0)
        memcpy(g->meta_uuid, sb + 248, 16);
    else
        memcpy(g->meta_uuid, g->uuid, 16);
    g->v5 = g->version == 5;
    g->sparse_inodes = (g->incompat & XFS_SB_FEAT_INCOMPAT_SPINODES) != 0;

    if (g->block_size < 512U || g->block_size > 65536U ||
        (g->block_size & (g->block_size - 1U)) != 0) {
        xfs_set_error(error, "unsupported XFS block size");
        return -1;
    }
    if (g->dblocks == 0 || (reader_bytes != 0 && g->dblocks > reader_bytes / g->block_size)) {
        xfs_set_error(error, "invalid XFS data-device size");
        return -1;
    }
    if (g->agblocks == 0 || g->agcount == 0) {
        xfs_set_error(error, "invalid XFS allocation-group geometry");
        return -1;
    }
    if (g->sector_size < 512U || g->sector_size > g->block_size ||
        (g->sector_size & (g->sector_size - 1U)) != 0) {
        xfs_set_error(error, "unsupported XFS sector size");
        return -1;
    }
    if (g->inode_size < 256U || g->inode_size > g->block_size ||
        (g->inode_size & (g->inode_size - 1U)) != 0) {
        xfs_set_error(error, "unsupported XFS inode size");
        return -1;
    }
    if (g->inopblock != g->block_size / g->inode_size) {
        xfs_set_error(error, "inconsistent XFS inode geometry");
        return -1;
    }
    if (g->blocklog >= 63U || (UINT64_C(1) << g->blocklog) != g->block_size ||
        g->inopblog >= 63U || (UINT64_C(1) << g->inopblog) != g->inopblock) {
        xfs_set_error(error, "inconsistent XFS logarithmic geometry");
        return -1;
    }
    if (g->agblklog == 0 || g->agblklog > 32U) {
        xfs_set_error(error, "unsupported XFS allocation-group address width");
        return -1;
    }
    return 0;
}

bool xfs_probe_path(const char *path) {
    XfsReader reader;
    char *error = NULL;
    if (reader_open(path, &reader, &error) != 0) {
        xfs_clear_error(&error);
        return false;
    }
    uint8_t magic[4];
    bool result = read_exact(&reader, magic, sizeof(magic), 0, "XFS signature", &error) == 0 &&
                  memcmp(magic, XFS_SB_MAGIC, 4) == 0;
    xfs_clear_error(&error);
    reader_close(&reader);
    return result;
}

static int short_walk_block(ShortWalker *walker, uint32_t block, LeafCallback callback,
                            void *context, char **error) {
    uint64_t ag_len = xfs_ag_length(walker->reader->g, walker->agno);
    if (block == 0 || block >= ag_len) {
        xfs_set_error(error, "XFS B+tree block points outside its allocation group");
        return -1;
    }
    if (u64_seen(&walker->visited, block)) {
        xfs_set_error(error, "loop in XFS allocation-group B+tree");
        return -1;
    }
    xfs_u64_push(&walker->visited, block);
    uint32_t bs = walker->reader->g->block_size;
    uint8_t *raw = ld_xmalloc(bs);
    if (read_exact(walker->reader, raw, bs,
                   xfs_ag_offset(walker->reader->g, walker->agno, block),
                   "XFS B+tree block", error) != 0) {
        free(raw);
        return -1;
    }
    bool crc = memcmp(raw, walker->magic_crc, 4) == 0;
    if (!crc && memcmp(raw, walker->magic_old, 4) != 0) {
        free(raw);
        xfs_set_error(error, "invalid XFS allocation-group B+tree magic");
        return -1;
    }
    size_t header = crc ? 56U : 16U;
    uint16_t level = xfs_be16(raw + 4);
    uint16_t nrecs = xfs_be16(raw + 6);
    if (level > XFS_NATIVE_MAX_BTREE_LEVEL) {
        free(raw);
        xfs_set_error(error, "invalid XFS B+tree level");
        return -1;
    }
    walker->blocks_read++;
    if (walker->blocks_read > XFS_NATIVE_MAX_BTREE_BLOCKS) {
        free(raw);
        xfs_set_error(error, "XFS B+tree traversal exceeded the safety limit");
        return -1;
    }
    if (level == 0) {
        if ((size_t)nrecs > (bs - header) / walker->record_size) {
            free(raw);
            xfs_set_error(error, "truncated XFS B+tree leaf");
            return -1;
        }
        int result = callback(raw + header, (size_t)nrecs * walker->record_size, context, error);
        free(raw);
        return result;
    }
    size_t denom = walker->key_size + walker->ptr_size;
    size_t maxrecs = (bs - header) / denom;
    if (nrecs == 0 || nrecs > maxrecs) {
        free(raw);
        xfs_set_error(error, "invalid XFS B+tree node record count");
        return -1;
    }
    size_t ptr_base = header + maxrecs * walker->key_size;
    uint64_t *children = ld_xmalloc((size_t)nrecs * sizeof(*children));
    for (uint16_t index = 0; index < nrecs; ++index) {
        size_t pos = ptr_base + (size_t)index * walker->ptr_size;
        uint64_t child = walker->ptr_size == 4 ? xfs_be32(raw + pos) : xfs_be64(raw + pos);
        if (child == 0 || child >= ag_len) {
            free(children);
            free(raw);
            xfs_set_error(error, "XFS B+tree child points outside its allocation group");
            return -1;
        }
        children[index] = child;
    }
    free(raw);
    for (uint16_t index = 0; index < nrecs; ++index) {
        if (short_walk_block(walker, (uint32_t)children[index], callback, context, error) != 0) {
            free(children);
            return -1;
        }
    }
    free(children);
    return 0;
}

static int free_leaf(const uint8_t *records, size_t bytes, void *context0, char **error) {
    FreeContext *context = context0;
    if (bytes % 8U != 0) {
        xfs_set_error(error, "invalid XFS free-space leaf length");
        return -1;
    }
    for (size_t pos = 0; pos < bytes; pos += 8U) {
        uint32_t start = xfs_be32(records + pos);
        uint32_t length = xfs_be32(records + pos + 4U);
        if (length == 0 || (uint64_t)start + length > context->ag_len ||
            (context->have_previous && start < context->previous_end)) {
            xfs_set_error(error, "invalid XFS free-space record in allocation group %u",
                          context->agno);
            return -1;
        }
        uint64_t base = (uint64_t)context->agno * context->catalogue->geometry.agblocks;
        xfs_range_push(&context->catalogue->free_ranges, base + start, base + start + length);
        context->actual_free += length;
        context->extents++;
        context->previous_end = (uint64_t)start + length;
        context->have_previous = true;
    }
    return 0;
}

static int scan_free_space(XfsReader *reader, XfsCatalogue *catalogue, char **error) {
    XfsGeometry *g = &catalogue->geometry;
    for (uint32_t agno = 0; agno < g->agcount; ++agno) {
        uint64_t ag_len = xfs_ag_length(g, agno);
        if (ag_len == 0) break;
        uint8_t *agf = ld_xmalloc(g->sector_size);
        if (read_exact(reader, agf, g->sector_size,
                       xfs_ag_offset(g, agno, 0) + g->sector_size,
                       "XFS AGF", error) != 0) {
            free(agf);
            return -1;
        }
        if (memcmp(agf, XFS_AGF_MAGIC, 4) != 0 || xfs_be32(agf + 8) != agno) {
            free(agf);
            xfs_set_error(error, "invalid XFS AGF header for allocation group %u", agno);
            return -1;
        }
        uint32_t recorded_len = xfs_be32(agf + 12);
        uint32_t root = xfs_be32(agf + 16);
        uint32_t freeblks = xfs_be32(agf + (g->v5 ? 52U : 44U));
        uint32_t longest = xfs_be32(agf + (g->v5 ? 56U : 48U));
        free(agf);
        if (recorded_len != ag_len) {
            xfs_set_error(error, "XFS allocation group %u has inconsistent length", agno);
            return -1;
        }
        ShortWalker walker = {
            .reader = reader,
            .agno = agno,
            .magic_old = XFS_ABTB_MAGIC,
            .magic_crc = XFS_ABTB_CRC_MAGIC,
            .key_size = 8,
            .ptr_size = 4,
            .record_size = 8,
        };
        FreeContext context = {
            .catalogue = catalogue,
            .agno = agno,
            .ag_len = ag_len,
        };
        if (short_walk_block(&walker, root, free_leaf, &context, error) != 0) {
            xfs_u64_free(&walker.visited);
            return -1;
        }
        catalogue->bnobt_blocks += walker.blocks_read;
        xfs_u64_free(&walker.visited);
        ag_detail_push(&catalogue->allocation_groups, (XfsAgDetail){
            .agno = agno,
            .blocks = ag_len,
            .free_blocks = context.actual_free,
            .recorded_free_blocks = freeblks,
            .longest_free_extent = longest,
            .free_extents = context.extents,
            .free_count_matches_agf = context.actual_free == freeblks,
        });
    }
    xfs_range_sort_merge(&catalogue->free_ranges);
    uint64_t cursor = 0;
    for (size_t index = 0; index < catalogue->free_ranges.count; ++index) {
        XfsRange range = catalogue->free_ranges.items[index];
        if (cursor < range.start) xfs_range_push(&catalogue->used_ranges, cursor, range.start);
        if (range.end > cursor) cursor = range.end;
    }
    if (cursor < g->dblocks) xfs_range_push(&catalogue->used_ranges, cursor, g->dblocks);
    return 0;
}

static int inobt_leaf(const uint8_t *records, size_t bytes, void *context0, char **error) {
    InobtContext *context = context0;
    if (bytes % 16U != 0) {
        xfs_set_error(error, "invalid XFS inode B+tree leaf length");
        return -1;
    }
    for (size_t pos = 0; pos < bytes; pos += 16U) {
        uint32_t startino = xfs_be32(records + pos);
        uint16_t holemask = context->sparse ? xfs_be16(records + pos + 4U) : 0;
        uint8_t count = context->sparse ? records[pos + 6U] : 64U;
        uint64_t free_mask = xfs_be64(records + pos + 8U);
        if (count > 64U) {
            xfs_set_error(error, "invalid XFS inode-chunk count");
            return -1;
        }
        inode_chunk_push(context->chunks, (InodeChunk){startino, holemask, free_mask});
    }
    return 0;
}

static int inode_chunks(XfsReader *reader, uint32_t agno, InodeChunkVec *chunks,
                        uint64_t *blocks_read, char **error) {
    XfsGeometry *g = reader->g;
    uint8_t *agi = ld_xmalloc(g->sector_size);
    if (read_exact(reader, agi, g->sector_size,
                   xfs_ag_offset(g, agno, 0) + 2U * g->sector_size,
                   "XFS AGI", error) != 0) {
        free(agi);
        return -1;
    }
    if (memcmp(agi, XFS_AGI_MAGIC, 4) != 0 || xfs_be32(agi + 8) != agno) {
        free(agi);
        xfs_set_error(error, "invalid XFS AGI header for allocation group %u", agno);
        return -1;
    }
    uint32_t root = xfs_be32(agi + 20);
    free(agi);
    ShortWalker walker = {
        .reader = reader,
        .agno = agno,
        .magic_old = XFS_IBT_MAGIC,
        .magic_crc = XFS_IBT_CRC_MAGIC,
        .key_size = 4,
        .ptr_size = 4,
        .record_size = 16,
    };
    InobtContext context = {.chunks = chunks, .sparse = g->sparse_inodes};
    int result = short_walk_block(&walker, root, inobt_leaf, &context, error);
    *blocks_read += walker.blocks_read;
    xfs_u64_free(&walker.visited);
    return result;
}

static int decode_extent(const uint8_t *record, const XfsGeometry *g, XfsExtent *extent,
                         char **error) {
    uint64_t l0 = xfs_be64(record);
    uint64_t l1 = xfs_be64(record + 8);
    bool unwritten = (l0 >> 63) != 0;
    uint64_t logical = (l0 & ((UINT64_C(1) << 63) - 1)) >> 9;
    uint64_t fsblock = ((l0 & UINT64_C(0x1ff)) << 43) | (l1 >> 21);
    uint64_t length = l1 & ((UINT64_C(1) << 21) - 1);
    uint64_t agno = fsblock >> g->agblklog;
    uint64_t agbno = fsblock & ((UINT64_C(1) << g->agblklog) - 1);
    uint64_t physical = agno * g->agblocks + agbno;
    if (length == 0 || physical >= g->dblocks || length > g->dblocks - physical ||
        agbno >= g->agblocks || length > g->agblocks - agbno) {
        xfs_set_error(error, "XFS extent points outside the data device");
        return -1;
    }
    *extent = (XfsExtent){logical, physical, length, unwritten};
    return 0;
}

static int bmap_external(BmapWalker *walker, uint64_t fsblock, int expected_level,
                         XfsExtentVec *extents, XfsU64Vec *tree_blocks, char **error) {
    XfsGeometry *g = walker->g;
    uint64_t agno = fsblock >> g->agblklog;
    uint64_t agbno = fsblock & ((UINT64_C(1) << g->agblklog) - 1);
    uint64_t physical = agno * g->agblocks + agbno;
    if (agno >= g->agcount || agbno >= xfs_ag_length(g, (uint32_t)agno) || physical >= g->dblocks) {
        xfs_set_error(error, "XFS bmap B+tree block points outside the filesystem");
        return -1;
    }
    if (u64_seen(&walker->visited, physical)) {
        xfs_set_error(error, "loop or shared block in XFS bmap B+tree");
        return -1;
    }
    xfs_u64_push(&walker->visited, physical);
    xfs_u64_push(tree_blocks, physical);
    uint8_t *raw = ld_xmalloc(g->block_size);
    if (read_exact(walker->reader, raw, g->block_size, physical * g->block_size,
                   "XFS bmap B+tree block", error) != 0) {
        free(raw);
        return -1;
    }
    bool crc = memcmp(raw, XFS_BMAP_CRC_MAGIC, 4) == 0;
    if (!crc && memcmp(raw, XFS_BMAP_MAGIC, 4) != 0) {
        free(raw);
        xfs_set_error(error, "invalid XFS bmap B+tree magic");
        return -1;
    }
    size_t header = crc ? 72U : 24U;
    uint16_t level = xfs_be16(raw + 4);
    uint16_t nrecs = xfs_be16(raw + 6);
    if (level > XFS_NATIVE_MAX_BTREE_LEVEL || (expected_level >= 0 && level != (uint16_t)expected_level)) {
        free(raw);
        xfs_set_error(error, "invalid XFS bmap tree level");
        return -1;
    }
    walker->blocks_read++;
    if (walker->blocks_read > XFS_NATIVE_MAX_BTREE_BLOCKS) {
        free(raw);
        xfs_set_error(error, "XFS bmap traversal exceeded the safety limit");
        return -1;
    }
    if (level == 0) {
        if ((size_t)nrecs > (g->block_size - header) / 16U) {
            free(raw);
            xfs_set_error(error, "truncated XFS bmap leaf");
            return -1;
        }
        for (uint16_t index = 0; index < nrecs; ++index) {
            XfsExtent extent;
            if (decode_extent(raw + header + (size_t)index * 16U, g, &extent, error) != 0) {
                free(raw);
                return -1;
            }
            xfs_extent_push(extents, extent);
        }
        free(raw);
        return 0;
    }
    size_t maxrecs = (g->block_size - header) / 16U;
    if (nrecs == 0 || nrecs > maxrecs) {
        free(raw);
        xfs_set_error(error, "invalid XFS bmap node record count");
        return -1;
    }
    size_t ptr_base = header + maxrecs * 8U;
    uint64_t *children = ld_xmalloc((size_t)nrecs * sizeof(*children));
    for (uint16_t index = 0; index < nrecs; ++index)
        children[index] = xfs_be64(raw + ptr_base + (size_t)index * 8U);
    free(raw);
    for (uint16_t index = 0; index < nrecs; ++index) {
        if (bmap_external(walker, children[index], (int)level - 1,
                          extents, tree_blocks, error) != 0) {
            free(children);
            return -1;
        }
    }
    free(children);
    return 0;
}

static int inode_fork(BmapWalker *walker, const uint8_t *inode, uint32_t core_size,
                      uint32_t fork_size, uint8_t format, uint64_t nextents,
                      XfsExtentVec *extents, XfsU64Vec *tree_blocks, char **error) {
    XfsGeometry *g = walker->g;
    if ((uint64_t)core_size + fork_size > g->inode_size) {
        xfs_set_error(error, "invalid XFS inode fork geometry");
        return -1;
    }
    const uint8_t *fork = inode + core_size;
    if (format == XFS_DINODE_FMT_LOCAL) return 0;
    if (format == XFS_DINODE_FMT_EXTENTS) {
        if (nextents > XFS_NATIVE_MAX_EXTENTS_PER_INODE || nextents > fork_size / 16U) {
            xfs_set_error(error, "invalid XFS direct extent count");
            return -1;
        }
        for (uint64_t index = 0; index < nextents; ++index) {
            XfsExtent extent;
            if (decode_extent(fork + index * 16U, g, &extent, error) != 0) return -1;
            xfs_extent_push(extents, extent);
        }
        xfs_extent_sort_coalesce(extents);
        return 0;
    }
    if (format != XFS_DINODE_FMT_BTREE) {
        xfs_set_error(error, "unsupported XFS inode data-fork format");
        return -1;
    }
    if (fork_size < 4U) {
        xfs_set_error(error, "truncated XFS inode bmap root");
        return -1;
    }
    uint16_t level = xfs_be16(fork);
    uint16_t nrecs = xfs_be16(fork + 2);
    if (level == 0 || level > XFS_NATIVE_MAX_BTREE_LEVEL) {
        xfs_set_error(error, "invalid XFS inode bmap root level");
        return -1;
    }
    size_t maxrecs = (fork_size - 4U) / 16U;
    if (nrecs == 0 || nrecs > maxrecs) {
        xfs_set_error(error, "invalid XFS inode bmap root count");
        return -1;
    }
    size_t ptr_base = 4U + maxrecs * 8U;
    for (uint16_t index = 0; index < nrecs; ++index) {
        uint64_t child = xfs_be64(fork + ptr_base + (size_t)index * 8U);
        if (bmap_external(walker, child, (int)level - 1, extents, tree_blocks, error) != 0)
            return -1;
    }
    xfs_extent_sort_coalesce(extents);
    if (nextents != 0 && extents->count > nextents) {
        xfs_set_error(error, "XFS inode extent tree exceeds its recorded extent count");
        return -1;
    }
    return 0;
}

static uint64_t inode_number(const XfsGeometry *g, uint32_t agno, uint32_t agino) {
    return ((uint64_t)agno << (g->agblklog + g->inopblog)) | agino;
}

static bool internal_inode(const XfsGeometry *g, uint64_t inode) {
    const uint64_t values[] = {g->rbmino, g->rsumino, g->uquotino, g->gquotino, g->pquotino};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index)
        if (values[index] != 0 && values[index] != UINT64_MAX && values[index] == inode)
            return true;
    return false;
}

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t owner;
} OwnedRange;

typedef struct {
    OwnedRange *items;
    size_t count;
    size_t capacity;
} OwnedRangeVec;

static void owned_push(OwnedRangeVec *vec, OwnedRange range) {
    vec->items = vector_grow(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = range;
}

static int owned_compare(const void *left0, const void *right0) {
    const OwnedRange *left = left0;
    const OwnedRange *right = right0;
    if (left->start < right->start) return -1;
    if (left->start > right->start) return 1;
    if (left->end < right->end) return -1;
    if (left->end > right->end) return 1;
    return 0;
}

static int scan_inodes(XfsReader *reader, bool strict, XfsCatalogue *catalogue, char **error) {
    XfsGeometry *g = &catalogue->geometry;
    uint64_t allocated = g->icount >= g->ifree ? g->icount - g->ifree : 0;
    if (allocated > XFS_NATIVE_MAX_INODES) {
        xfs_set_error(error, "XFS allocated-inode count exceeds the native analyser safety limit");
        return -1;
    }
    BmapWalker bmap = {.reader = reader, .g = g};
    OwnedRangeVec owned = {0};
    for (uint32_t agno = 0; agno < g->agcount; ++agno) {
        InodeChunkVec chunks = {0};
        if (inode_chunks(reader, agno, &chunks, &catalogue->inobt_blocks, error) != 0) {
            free(chunks.items);
            xfs_u64_free(&bmap.visited);
            free(owned.items);
            return -1;
        }
        uint64_t ag_len = xfs_ag_length(g, agno);
        for (size_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
            InodeChunk chunk = chunks.items[chunk_index];
            for (unsigned index = 0; index < 64U; ++index) {
                if ((chunk.holemask & (UINT16_C(1) << (index / 4U))) != 0) continue;
                if ((chunk.free_mask & (UINT64_C(1) << index)) != 0) continue;
                uint32_t agino = chunk.startino + index;
                uint32_t agbno = agino >> g->inopblog;
                uint32_t inode_index = agino & (g->inopblock - 1U);
                if (agbno >= ag_len) {
                    catalogue->malformed_inodes++;
                    continue;
                }
                uint64_t offset = xfs_ag_offset(g, agno, agbno) + (uint64_t)inode_index * g->inode_size;
                uint8_t *inode = ld_xmalloc(g->inode_size);
                if (read_exact(reader, inode, g->inode_size, offset, "XFS inode", error) != 0) {
                    free(inode);
                    free(chunks.items);
                    xfs_u64_free(&bmap.visited);
                    free(owned.items);
                    return -1;
                }
                if (xfs_be16(inode) != XFS_DINODE_MAGIC) {
                    free(inode);
                    catalogue->malformed_inodes++;
                    continue;
                }
                uint16_t mode = xfs_be16(inode + 2);
                uint16_t kind_bits = (uint16_t)(mode & XFS_MODE_TYPE_MASK);
                bool is_file = kind_bits == XFS_MODE_REG;
                bool is_directory = kind_bits == XFS_MODE_DIR;
                if (!is_file && !is_directory) {
                    free(inode);
                    continue;
                }
                uint64_t ino = inode_number(g, agno, agino);
                uint8_t version = inode[4];
                uint8_t data_format = inode[5];
                if ((version < 1U || version > 3U) ||
                    (data_format != XFS_DINODE_FMT_LOCAL && data_format != XFS_DINODE_FMT_EXTENTS && data_format != XFS_DINODE_FMT_BTREE)) {
                    free(inode);
                    catalogue->malformed_inodes++;
                    continue;
                }
                uint32_t core_size = version == 3U ? 176U : 100U;
                uint8_t forkoff = inode[82];
                uint32_t fork_end = forkoff != 0 ? core_size + (uint32_t)forkoff * 8U : g->inode_size;
                if (fork_end < core_size || fork_end > g->inode_size) {
                    free(inode);
                    if (strict) {
                        xfs_set_error(error, "inode %" PRIu64 " has invalid fork geometry", ino);
                        free(chunks.items);
                        xfs_u64_free(&bmap.visited);
                        free(owned.items);
                        return -1;
                    }
                    catalogue->malformed_inodes++;
                    continue;
                }
                uint32_t fork_size = fork_end - core_size;
                uint16_t flags = xfs_be16(inode + 90);
                uint64_t flags2 = version == 3U ? xfs_be64(inode + 120) : 0;
                uint64_t nextents = (flags2 & XFS_DIFLAG2_NREXT64) != 0 ? xfs_be64(inode + 24) : xfs_be32(inode + 76);

                if ((flags & XFS_DIFLAG_REALTIME) != 0) {
                    catalogue->realtime_inodes++;
                    if (is_file) catalogue->regular_files++; else catalogue->directories++;
                    free(inode);
                    if (strict) {
                        xfs_set_error(error, "inode %" PRIu64 " uses the XFS realtime device", ino);
                        free(chunks.items);
                        xfs_u64_free(&bmap.visited);
                        free(owned.items);
                        return -1;
                    }
                    catalogue->skipped_metadata_inodes++;
                    continue;
                }
                if (strict && is_file && data_format == XFS_DINODE_FMT_LOCAL) {
                    free(inode);
                    xfs_set_error(error, "regular-file inode %" PRIu64 " has an invalid local data fork", ino);
                    free(chunks.items);
                    xfs_u64_free(&bmap.visited);
                    free(owned.items);
                    return -1;
                }

                XfsExtentVec extents = {0};
                XfsU64Vec tree_blocks = {0};
                size_t visited_before = bmap.visited.count;
                if (inode_fork(&bmap, inode, core_size, fork_size, data_format, nextents,
                               &extents, &tree_blocks, error) != 0) {
                    xfs_extent_free(&extents);
                    xfs_u64_free(&tree_blocks);
                    free(inode);
                    if (strict) {
                        char *detail = error != NULL && *error != NULL ? ld_xstrdup(*error) : ld_xstrdup("unsupported data fork");
                        xfs_set_error(error, "inode %" PRIu64 " data fork is unsupported: %s", ino, detail);
                        free(detail);
                        free(chunks.items);
                        xfs_u64_free(&bmap.visited);
                        free(owned.items);
                        return -1;
                    }
                    xfs_clear_error(error);
                    catalogue->malformed_inodes++;
                    continue;
                }
                (void)visited_before;
                catalogue->inodes_scanned++;
                if (is_file) catalogue->regular_files++; else catalogue->directories++;

                bool filesystem_metadata = internal_inode(g, ino) || (flags2 & XFS_DIFLAG2_METADATA) != 0;
                if (strict && is_file && (flags & XFS_DIFLAG_NODEFRAG) != 0 && !filesystem_metadata && extents.count != 0) {
                    xfs_extent_free(&extents);
                    xfs_u64_free(&tree_blocks);
                    free(inode);
                    xfs_set_error(error, "inode %" PRIu64 " has the XFS nodefrag flag", ino);
                    free(chunks.items);
                    xfs_u64_free(&bmap.visited);
                    free(owned.items);
                    return -1;
                }
                bool protected_inode = filesystem_metadata || (flags & XFS_DIFLAG_NODEFRAG) != 0;
                if (protected_inode || (flags2 & XFS_DIFLAG2_REFLINK) != 0) {
                    if (strict && is_file && (flags2 & XFS_DIFLAG2_REFLINK) != 0 && extents.count != 0) {
                        xfs_extent_free(&extents);
                        xfs_u64_free(&tree_blocks);
                        free(inode);
                        xfs_set_error(error, "inode %" PRIu64 " has shared reflink extents", ino);
                        free(chunks.items);
                        xfs_u64_free(&bmap.visited);
                        free(owned.items);
                        return -1;
                    }
                    xfs_extent_free(&extents);
                    xfs_u64_free(&tree_blocks);
                    free(inode);
                    catalogue->skipped_metadata_inodes++;
                    continue;
                }

                bool fragmented = extents.count > 1U;
                for (size_t extent_index = 0; extent_index < extents.count; ++extent_index) {
                    XfsExtent extent = extents.items[extent_index];
                    if (is_directory) xfs_range_push(&catalogue->directory_ranges, extent.physical, extent.physical + extent.length);
                    if (fragmented) xfs_range_push(&catalogue->fragmented_ranges, extent.physical, extent.physical + extent.length);
                    owned_push(&owned, (OwnedRange){extent.physical, extent.physical + extent.length, ino});
                }
                if (fragmented) {
                    if (is_file) catalogue->fragmented_files++;
                    else catalogue->fragmented_directories++;
                }

                XfsObject object = {
                    .inode = ino,
                    .inode_offset = offset,
                    .size = xfs_be64(inode + 56),
                    .nblocks = xfs_be64(inode + 64),
                    .version = version,
                    .data_format = data_format,
                    .core_size = core_size,
                    .fork_size = fork_size,
                    .flags = flags,
                    .flags2 = flags2,
                    .is_file = is_file,
                    .extents = extents,
                    .bmap_blocks = tree_blocks,
                };
                object_push(&catalogue->objects, object);
                free(inode);
            }
        }
        free(chunks.items);
    }
    catalogue->bmap_blocks = bmap.blocks_read;
    xfs_u64_free(&bmap.visited);
    xfs_range_sort_merge(&catalogue->fragmented_ranges);
    xfs_range_sort_merge(&catalogue->directory_ranges);

    if (owned.count > 1U) {
        qsort(owned.items, owned.count, sizeof(*owned.items), owned_compare);
        for (size_t index = 1; index < owned.count; ++index) {
            OwnedRange previous = owned.items[index - 1];
            OwnedRange current = owned.items[index];
            if (current.start < previous.end && current.owner != previous.owner) {
                if (strict) {
                    xfs_set_error(error,
                                  "XFS block ranges overlap between inodes %" PRIu64 " and %" PRIu64,
                                  previous.owner, current.owner);
                    free(owned.items);
                    return -1;
                }
                catalogue->malformed_inodes++;
            }
        }
    }
    free(owned.items);
    if (strict && catalogue->malformed_inodes != 0) {
        xfs_set_error(error, "XFS catalogue found %" PRIu64 " malformed allocated inode(s)",
                      catalogue->malformed_inodes);
        return -1;
    }
    return 0;
}

int xfs_scan_catalogue(const char *path, bool strict, XfsCatalogue *out, char **error) {
    memset(out, 0, sizeof(*out));
    XfsReader reader;
    if (reader_open(path, &reader, error) != 0) return -1;
    uint8_t sb[512];
    if (read_exact(&reader, sb, sizeof(sb), 0, "XFS superblock", error) != 0) {
        reader_close(&reader);
        return -1;
    }
    if (geometry_parse(sb, reader.size, &out->geometry, error) != 0) {
        reader_close(&reader);
        return -1;
    }
    reader.g = &out->geometry;
    if (scan_free_space(&reader, out, error) != 0 || scan_inodes(&reader, strict, out, error) != 0) {
        reader_close(&reader);
        xfs_catalogue_free(out);
        return -1;
    }
    reader_close(&reader);
    return 0;
}

int xfs_validate_writer_support(const XfsCatalogue *catalogue, char **error) {
    const XfsGeometry *g = &catalogue->geometry;
    if (g->rblocks != 0) {
        xfs_set_error(error, "XFS realtime data devices are not supported");
        return -1;
    }
    if (g->logstart == 0) {
        xfs_set_error(error, "XFS filesystems with an external log are not supported");
        return -1;
    }
    if (!g->v5) {
        xfs_set_error(error, "raw XFS mutation currently requires the CRC-enabled v5 format");
        return -1;
    }
    if ((g->incompat & XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR) != 0) {
        xfs_set_error(error, "XFS filesystem is marked needs-repair");
        return -1;
    }
    const uint32_t supported_incompat =
        XFS_SB_FEAT_INCOMPAT_FTYPE | XFS_SB_FEAT_INCOMPAT_SPINODES |
        XFS_SB_FEAT_INCOMPAT_META_UUID | XFS_SB_FEAT_INCOMPAT_BIGTIME |
        XFS_SB_FEAT_INCOMPAT_NREXT64 | XFS_SB_FEAT_INCOMPAT_EXCHRANGE;
    const uint32_t supported_ro = XFS_SB_FEAT_RO_COMPAT_FINOBT |
        XFS_SB_FEAT_RO_COMPAT_RMAPBT | XFS_SB_FEAT_RO_COMPAT_REFLINK |
        XFS_SB_FEAT_RO_COMPAT_INOBTCNT;
    uint32_t unknown_incompat = g->incompat & ~supported_incompat & ~XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
    uint32_t unknown_ro = g->features_ro_compat & ~supported_ro;
    if (unknown_incompat != 0) {
        xfs_set_error(error, "XFS incompatible feature mask 0x%x is not supported by the raw writer",
                      unknown_incompat);
        return -1;
    }
    if (unknown_ro != 0) {
        xfs_set_error(error, "XFS read-only-compatible feature mask 0x%x is not supported by the raw writer",
                      unknown_ro);
        return -1;
    }
    for (size_t index = 0; index < catalogue->objects.count; ++index) {
        const XfsObject *item = &catalogue->objects.items[index];
        if (item->is_file && (item->data_format != XFS_DINODE_FMT_EXTENTS || item->bmap_blocks.count != 0)) {
            xfs_set_error(error,
                          "regular-file inode %" PRIu64 " uses an external/btree data fork; "
                          "the native writer rewrites only direct extent forks",
                          item->inode);
            return -1;
        }
    }
    return 0;
}

static void print_uuid(const uint8_t uuid[16]) {
    for (unsigned index = 0; index < 16U; ++index) printf("%02x", uuid[index]);
}

static void print_ranges(const XfsRangeVec *ranges) {
    putchar('[');
    for (size_t index = 0; index < ranges->count; ++index) {
        if (index != 0) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 "]", ranges->items[index].start, ranges->items[index].end);
    }
    putchar(']');
}

int xfs_emit_analysis_json(const char *path, char **error) {
    XfsCatalogue catalogue;
    if (xfs_scan_catalogue(path, false, &catalogue, error) != 0) return -1;
    const XfsGeometry *g = &catalogue.geometry;
    printf("{\"filesystem\":\"xfs\",\"block_size\":%u,\"sector_size\":%u,"
           "\"inode_size\":%u,\"dblocks\":%" PRIu64 ",\"agblocks\":%u,"
           "\"fdblocks\":%" PRIu64 ",\"uuid\":\"",
           g->block_size, g->sector_size, g->inode_size, g->dblocks, g->agblocks, g->fdblocks);
    print_uuid(g->uuid);
    printf("\",\"free_ranges\":");
    print_ranges(&catalogue.free_ranges);
    printf(",\"allocation_groups\":[");
    for (size_t index = 0; index < catalogue.allocation_groups.count; ++index) {
        const XfsAgDetail *ag = &catalogue.allocation_groups.items[index];
        if (index != 0) putchar(',');
        printf("{\"ag\":%u,\"blocks\":%" PRIu64 ",\"free_blocks\":%" PRIu64
               ",\"recorded_free_blocks\":%" PRIu64 ",\"longest_free_extent\":%" PRIu64
               ",\"free_extents\":%" PRIu64 ",\"free_count_matches_agf\":%s}",
               ag->agno, ag->blocks, ag->free_blocks, ag->recorded_free_blocks,
               ag->longest_free_extent, ag->free_extents,
               ag->free_count_matches_agf ? "true" : "false");
    }
    double fragmentation = 100.0 * (double)catalogue.fragmented_files /
                           (double)(catalogue.regular_files == 0 ? 1 : catalogue.regular_files);
    printf("],\"bnobt_blocks\":%" PRIu64 ",\"regular_files\":%" PRIu64
           ",\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64
           ",\"fragmented_directories\":%" PRIu64 ",\"fragmentation_percent\":%.9g"
           ",\"inodes_scanned\":%" PRIu64 ",\"malformed_inodes\":%" PRIu64
           ",\"realtime_inodes\":%" PRIu64 ",\"inobt_blocks\":%" PRIu64
           ",\"bmap_blocks\":%" PRIu64 ",\"fragmented_ranges\":",
           catalogue.bnobt_blocks, catalogue.regular_files, catalogue.directories,
           catalogue.fragmented_files, catalogue.fragmented_directories, fragmentation,
           catalogue.inodes_scanned, catalogue.malformed_inodes, catalogue.realtime_inodes,
           catalogue.inobt_blocks, catalogue.bmap_blocks);
    print_ranges(&catalogue.fragmented_ranges);
    printf(",\"directory_ranges\":");
    print_ranges(&catalogue.directory_ranges);
    printf("}\n");
    xfs_catalogue_free(&catalogue);
    return 0;
}
