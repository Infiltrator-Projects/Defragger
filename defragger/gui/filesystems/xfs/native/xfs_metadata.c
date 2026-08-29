// SPDX-License-Identifier: GPL-3.0-or-later
#include "xfs_native.h"
#include "ld_io.h"
#include "ld_runtime.h"

#include "infiltratr/endian.h"
#include "infiltratr/arithmetic.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XFS_META_HEADER 56U
#define XFS_AGF_CRC_FIELD 216U
#define XFS_AGFL_CRC_FIELD 32U
#define XFS_BTREE_CRC_FIELD 52U
#define XFS_LOG_MAGIC UINT32_C(0xfeedbabe)
#define XFS_LOG_CLIENT UINT8_C(0xaa)
#define XFS_LOG_UNMOUNT_TRANS UINT8_C(0x20)

static const uint8_t MAGIC_AGF[4] = {'X','A','G','F'};
static const uint8_t MAGIC_AGFL[4] = {'X','A','F','L'};
static const uint8_t MAGIC_BNOBT[4] = {'A','B','3','B'};
static const uint8_t MAGIC_CNTBT[4] = {'A','B','3','C'};
static const uint8_t MAGIC_RMAPBT[4] = {'R','M','B','3'};

typedef enum {
    TREE_BNOBT,
    TREE_CNTBT,
    TREE_RMAPBT,
} TreeKind;

typedef struct {
    TreeKind kind;
    const uint8_t *magic;
    size_t record_size;
    size_t key_size;
    size_t summary_size;
} TreeSpec;

static const TreeSpec SPEC_BNOBT = {TREE_BNOBT, MAGIC_BNOBT, 8U, 8U, 8U};
static const TreeSpec SPEC_CNTBT = {TREE_CNTBT, MAGIC_CNTBT, 8U, 8U, 8U};
static const TreeSpec SPEC_RMAPBT = {TREE_RMAPBT, MAGIC_RMAPBT, 24U, 40U, 20U};

typedef struct {
    uint8_t *items;
    size_t count;
    size_t capacity;
    size_t width;
} RawVec;

typedef struct {
    uint32_t block;
    uint32_t left;
    uint32_t right;
} Link;

typedef struct {
    Link *items;
    size_t count;
    size_t capacity;
} LinkVec;

typedef struct {
    uint32_t block;
    uint8_t low[20];
    uint8_t high[20];
} NodeSummary;

typedef struct {
    NodeSummary *items;
    size_t count;
    size_t capacity;
} SummaryVec;

typedef struct {
    int fd;
    const XfsGeometry *g;
    bool writable;
} RawMeta;

typedef struct {
    uint32_t agno;
    XfsRmapRecord record;
} AgRmap;

typedef struct {
    AgRmap *items;
    size_t count;
    size_t capacity;
} AgRmapVec;

typedef struct {
    uint64_t owner;
    uint64_t logical;
    uint32_t physical;
    uint32_t count;
    bool unwritten;
} SemanticRmap;

typedef struct {
    SemanticRmap *items;
    size_t count;
    size_t capacity;
} SemanticVec;

static uint32_t le32(const uint8_t *p) {
    return infiltratr_load_le32(p);
}

static void *grow_array(void *items, size_t *capacity, size_t count, size_t width) {
    if (count == SIZE_MAX ||
        !infiltratr_array_reserve(&items, capacity, width, count + 1U, 16U))
        ld_die("XFS metadata vector growth failed");
    return items;
}

static void raw_init(RawVec *vec, size_t width) {
    memset(vec, 0, sizeof(*vec));
    vec->width = width;
}

static void raw_push(RawVec *vec, const uint8_t *data) {
    vec->items = grow_array(vec->items, &vec->capacity, vec->count, vec->width);
    memcpy(vec->items + vec->count * vec->width, data, vec->width);
    vec->count++;
}

static const uint8_t *raw_at_const(const RawVec *vec, size_t index) {
    return vec->items + index * vec->width;
}

static void raw_free(RawVec *vec) {
    if (vec == NULL) return;
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void link_push(LinkVec *vec, Link link) {
    vec->items = grow_array(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = link;
}

static void link_free(LinkVec *vec) {
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void summary_push(SummaryVec *vec, const NodeSummary *summary) {
    vec->items = grow_array(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = *summary;
}

static void summary_free(SummaryVec *vec) {
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void ag_rmap_push(AgRmapVec *vec, AgRmap item) {
    vec->items = grow_array(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = item;
}

static void ag_rmap_free(AgRmapVec *vec) {
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void semantic_push(SemanticVec *vec, SemanticRmap item) {
    vec->items = grow_array(vec->items, &vec->capacity, vec->count, sizeof(*vec->items));
    vec->items[vec->count++] = item;
}

static void semantic_free(SemanticVec *vec) {
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static int u64_compare(const void *left0, const void *right0) {
    uint64_t left = *(const uint64_t *)left0;
    uint64_t right = *(const uint64_t *)right0;
    return left < right ? -1 : left > right ? 1 : 0;
}

static uint64_t rmap_offset(const XfsRmapRecord *record) {
    return record->offset_flags & XFS_RMAP_OFF_MASK;
}

static uint64_t rmap_key_flags(const XfsRmapRecord *record) {
    return record->offset_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK);
}

static void rmap_low_key(const XfsRmapRecord *record, uint8_t out[20]) {
    xfs_put_be32(out, record->start);
    xfs_put_be64(out + 4, record->owner);
    xfs_put_be64(out + 12, rmap_offset(record) | rmap_key_flags(record));
}

static void rmap_high_key(const XfsRmapRecord *record, uint8_t out[20]) {
    uint64_t offset = rmap_offset(record);
    if ((record->owner & (UINT64_C(1) << 63)) == 0 &&
        (record->offset_flags & XFS_RMAP_BMBT_BLOCK) == 0)
        offset += record->count - 1U;
    xfs_put_be32(out, record->start + record->count - 1U);
    xfs_put_be64(out + 4, record->owner);
    xfs_put_be64(out + 12, offset | rmap_key_flags(record));
}

static int rmap_compare(const void *left0, const void *right0) {
    const XfsRmapRecord *left = left0;
    const XfsRmapRecord *right = right0;
    if (left->start < right->start) return -1;
    if (left->start > right->start) return 1;
    if (left->owner < right->owner) return -1;
    if (left->owner > right->owner) return 1;
    uint64_t lk = rmap_offset(left) | rmap_key_flags(left);
    uint64_t rk = rmap_offset(right) | rmap_key_flags(right);
    return lk < rk ? -1 : lk > rk ? 1 : 0;
}

static int semantic_compare(const void *left0, const void *right0) {
    const SemanticRmap *left = left0;
    const SemanticRmap *right = right0;
    if (left->owner < right->owner) return -1;
    if (left->owner > right->owner) return 1;
    if (left->logical < right->logical) return -1;
    if (left->logical > right->logical) return 1;
    if (left->physical < right->physical) return -1;
    if (left->physical > right->physical) return 1;
    if (left->unwritten != right->unwritten) return left->unwritten ? 1 : -1;
    return 0;
}

static void semantic_sort_coalesce(SemanticVec *vec) {
    if (vec->count < 2) return;
    qsort(vec->items, vec->count, sizeof(*vec->items), semantic_compare);
    size_t out = 0;
    for (size_t index = 0; index < vec->count; ++index) {
        SemanticRmap current = vec->items[index];
        if (out != 0) {
            SemanticRmap *old = &vec->items[out - 1];
            if (old->owner == current.owner && old->unwritten == current.unwritten &&
                old->logical + old->count == current.logical &&
                (uint64_t)old->physical + old->count == current.physical) {
                if ((uint64_t)old->count + current.count > UINT32_MAX)
                    ld_die("XFS rmap semantic run exceeds 32-bit block count");
                old->count += current.count;
                continue;
            }
        }
        vec->items[out++] = current;
    }
    vec->count = out;
}

static bool semantic_equal(const SemanticVec *left, const SemanticVec *right) {
    if (left->count != right->count) return false;
    for (size_t index = 0; index < left->count; ++index) {
        const SemanticRmap *a = &left->items[index];
        const SemanticRmap *b = &right->items[index];
        if (a->owner != b->owner || a->logical != b->logical ||
            a->physical != b->physical || a->count != b->count ||
            a->unwritten != b->unwritten)
            return false;
    }
    return true;
}

static int meta_open(RawMeta *meta, const char *path, const XfsGeometry *g,
                     bool writable, char **error) {
    memset(meta, 0, sizeof(*meta));
    meta->fd = -1;
    meta->g = g;
    meta->writable = writable;
    meta->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (meta->fd < 0) {
        xfs_set_error(error, "cannot open XFS allocation metadata: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void meta_close(RawMeta *meta) {
    if (meta->fd >= 0) (void)close(meta->fd);
    meta->fd = -1;
}

static int meta_read(const RawMeta *meta, void *data, size_t length, uint64_t offset,
                     const char *what, char **error) {
    ssize_t got = ld_pread_full(meta->fd, data, length, offset);
    if (got < 0 || (size_t)got != length) {
        xfs_set_error(error, "short %s read", what);
        return -1;
    }
    return 0;
}

static int meta_write(const RawMeta *meta, const void *data, size_t length, uint64_t offset,
                      const char *what, char **error) {
    if (!meta->writable) {
        xfs_set_error(error, "attempted %s write through read-only XFS metadata handle", what);
        return -1;
    }
    ssize_t wrote = ld_pwrite_full(meta->fd, data, length, offset);
    if (wrote < 0 || (size_t)wrote != length) {
        xfs_set_error(error, "short %s write", what);
        return -1;
    }
    return 0;
}

static int read_agf(const RawMeta *meta, uint32_t agno, uint8_t *agf, char **error) {
    if (meta_read(meta, agf, meta->g->sector_size,
                  xfs_ag_offset(meta->g, agno, 0) + meta->g->sector_size,
                  "XFS AGF", error) != 0)
        return -1;
    if (memcmp(agf, MAGIC_AGF, 4) != 0 || xfs_be32(agf + 8) != agno) {
        xfs_set_error(error, "invalid XFS AGF for allocation group %u", agno);
        return -1;
    }
    return 0;
}

static void record_summary(const TreeSpec *spec, const uint8_t *record,
                           uint8_t low[20], uint8_t high[20]) {
    memset(low, 0, 20);
    memset(high, 0, 20);
    if (spec->kind != TREE_RMAPBT) {
        memcpy(low, record, 8);
        memcpy(high, record, 8);
        return;
    }
    XfsRmapRecord r = {
        .start = xfs_be32(record),
        .count = xfs_be32(record + 4),
        .owner = xfs_be64(record + 8),
        .offset_flags = xfs_be64(record + 16),
    };
    rmap_low_key(&r, low);
    rmap_high_key(&r, high);
}

static bool u64_contains(const XfsU64Vec *vec, uint64_t value) {
    for (size_t index = 0; index < vec->count; ++index)
        if (vec->items[index] == value) return true;
    return false;
}

static int walk_tree(const RawMeta *meta, uint32_t agno, uint32_t block,
                     int expected_level, const TreeSpec *spec, XfsU64Vec *seen,
                     XfsU64Vec *blocks, RawVec *records, LinkVec levels[XFS_NATIVE_MAX_BTREE_LEVEL + 1U],
                     uint8_t low[20], uint8_t high[20], char **error) {
    uint64_t ag_len = xfs_ag_length(meta->g, agno);
    if (block == 0 || block >= ag_len) {
        xfs_set_error(error, "invalid XFS allocation-tree root/child in AG %u", agno);
        return -1;
    }
    if (u64_contains(seen, block)) {
        xfs_set_error(error, "cycle in XFS allocation metadata B+tree");
        return -1;
    }
    xfs_u64_push(seen, block);
    xfs_u64_push(blocks, block);
    if (blocks->count > XFS_NATIVE_MAX_BTREE_BLOCKS) {
        xfs_set_error(error, "XFS allocation metadata traversal exceeded the safety limit");
        return -1;
    }
    uint32_t bs = meta->g->block_size;
    uint8_t *raw = ld_xmalloc(bs);
    if (meta_read(meta, raw, bs, xfs_ag_offset(meta->g, agno, block),
                  "XFS allocation B+tree block", error) != 0) {
        free(raw);
        return -1;
    }
    if (memcmp(raw, spec->magic, 4) != 0) {
        free(raw);
        xfs_set_error(error, "unexpected XFS allocation B+tree magic in AG %u block %u", agno, block);
        return -1;
    }
    if (le32(raw + XFS_BTREE_CRC_FIELD) != xfs_crc_field(raw, bs, XFS_BTREE_CRC_FIELD)) {
        free(raw);
        xfs_set_error(error, "XFS allocation B+tree CRC mismatch in AG %u block %u", agno, block);
        return -1;
    }
    if (xfs_be32(raw + 48) != agno) {
        free(raw);
        xfs_set_error(error, "XFS allocation B+tree owner does not match its allocation group");
        return -1;
    }
    uint64_t expected_daddr = ((uint64_t)agno * meta->g->agblocks + block) * (bs / 512U);
    if (xfs_be64(raw + 16) != expected_daddr) {
        free(raw);
        xfs_set_error(error, "XFS allocation B+tree disk address is inconsistent");
        return -1;
    }
    uint16_t level = xfs_be16(raw + 4);
    uint16_t nrecs = xfs_be16(raw + 6);
    if (level > XFS_NATIVE_MAX_BTREE_LEVEL || (expected_level >= 0 && level != (uint16_t)expected_level)) {
        free(raw);
        xfs_set_error(error, "XFS allocation B+tree child level is inconsistent");
        return -1;
    }
    link_push(&levels[level], (Link){block, xfs_be32(raw + 8), xfs_be32(raw + 12)});
    if (level == 0) {
        if (nrecs == 0 || (size_t)nrecs > (bs - XFS_META_HEADER) / spec->record_size) {
            free(raw);
            xfs_set_error(error, "invalid XFS allocation B+tree leaf record count");
            return -1;
        }
        for (uint16_t index = 0; index < nrecs; ++index)
            raw_push(records, raw + XFS_META_HEADER + (size_t)index * spec->record_size);
        record_summary(spec, raw + XFS_META_HEADER, low, high);
        uint8_t unused[20];
        record_summary(spec, raw + XFS_META_HEADER + (size_t)(nrecs - 1U) * spec->record_size,
                       unused, high);
        free(raw);
        return 0;
    }
    size_t maxrecs = (bs - XFS_META_HEADER) / (spec->key_size + 4U);
    if (nrecs == 0 || nrecs > maxrecs) {
        free(raw);
        xfs_set_error(error, "invalid XFS allocation B+tree internal record count");
        return -1;
    }
    size_t ptrbase = XFS_META_HEADER + maxrecs * spec->key_size;
    bool have = false;
    for (uint16_t index = 0; index < nrecs; ++index) {
        uint32_t child = xfs_be32(raw + ptrbase + (size_t)index * 4U);
        uint8_t child_low[20], child_high[20];
        if (walk_tree(meta, agno, child, (int)level - 1, spec, seen, blocks, records,
                      levels, child_low, child_high, error) != 0) {
            free(raw);
            return -1;
        }
        const uint8_t *stored = raw + XFS_META_HEADER + (size_t)index * spec->key_size;
        if (spec->kind == TREE_RMAPBT) {
            if (memcmp(stored, child_low, 20) != 0 || memcmp(stored + 20, child_high, 20) != 0) {
                free(raw);
                xfs_set_error(error, "XFS rmapbt internal low/high separator pair is inconsistent");
                return -1;
            }
        } else if (memcmp(stored, child_low, spec->summary_size) != 0) {
            free(raw);
            xfs_set_error(error, "XFS allocation B+tree internal separator key is inconsistent");
            return -1;
        }
        if (!have) {
            memcpy(low, child_low, spec->summary_size);
            have = true;
        }
        memcpy(high, child_high, spec->summary_size);
    }
    free(raw);
    return 0;
}

static int read_tree(const RawMeta *meta, uint32_t agno, uint32_t root,
                     const TreeSpec *spec, XfsU64Vec *blocks, RawVec *records,
                     char **error) {
    XfsU64Vec seen = {0};
    LinkVec levels[XFS_NATIVE_MAX_BTREE_LEVEL + 1U];
    memset(levels, 0, sizeof(levels));
    raw_init(records, spec->record_size);
    uint8_t low[20], high[20];
    int result = walk_tree(meta, agno, root, -1, spec, &seen, blocks, records,
                           levels, low, high, error);
    if (result == 0) {
        for (size_t level = 0; level <= XFS_NATIVE_MAX_BTREE_LEVEL; ++level) {
            LinkVec *nodes = &levels[level];
            for (size_t index = 0; index < nodes->count; ++index) {
                uint32_t want_left = index == 0 ? XFS_NULLAGBLOCK : nodes->items[index - 1].block;
                uint32_t want_right = index + 1U == nodes->count ? XFS_NULLAGBLOCK : nodes->items[index + 1U].block;
                if (nodes->items[index].left != want_left || nodes->items[index].right != want_right) {
                    xfs_set_error(error, "XFS allocation B+tree sibling chain is inconsistent at level %zu", level);
                    result = -1;
                    break;
                }
            }
            if (result != 0) break;
        }
    }
    xfs_u64_free(&seen);
    for (size_t level = 0; level <= XFS_NATIVE_MAX_BTREE_LEVEL; ++level)
        link_free(&levels[level]);
    if (result != 0) {
        xfs_u64_free(blocks);
        raw_free(records);
    }
    return result;
}

static int read_agfl(const RawMeta *meta, uint32_t agno, const uint8_t *agf,
                     XfsU64Vec *entries, uint32_t *capacity, char **error) {
    uint16_t ss = meta->g->sector_size;
    uint8_t *raw = ld_xmalloc(ss);
    uint64_t offset = xfs_ag_offset(meta->g, agno, 0) + 3U * ss;
    if (meta_read(meta, raw, ss, offset, "XFS AGFL", error) != 0) {
        free(raw);
        return -1;
    }
    if (memcmp(raw, MAGIC_AGFL, 4) != 0 || xfs_be32(raw + 4) != agno) {
        free(raw);
        xfs_set_error(error, "invalid XFS AGFL for allocation group %u", agno);
        return -1;
    }
    if (le32(raw + XFS_AGFL_CRC_FIELD) != xfs_crc_field(raw, ss, XFS_AGFL_CRC_FIELD)) {
        free(raw);
        xfs_set_error(error, "XFS AGFL CRC mismatch in allocation group %u", agno);
        return -1;
    }
    *capacity = (ss - 36U) / 4U;
    uint32_t first = xfs_be32(agf + 40);
    uint32_t count = xfs_be32(agf + 48);
    if (count > *capacity || (count != 0 && first >= *capacity)) {
        free(raw);
        xfs_set_error(error, "invalid XFS AGFL ring geometry");
        return -1;
    }
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t slot = (first + index) % *capacity;
        uint32_t value = xfs_be32(raw + 36U + (size_t)slot * 4U);
        if (value == XFS_NULLAGBLOCK || value == 0 || value >= xfs_ag_length(meta->g, agno)) {
            free(raw);
            xfs_set_error(error, "invalid live XFS AGFL entry");
            return -1;
        }
        xfs_u64_push(entries, value);
    }
    free(raw);
    return 0;
}

static int write_agfl(const RawMeta *meta, uint32_t agno, const uint64_t *entries,
                      size_t count, uint32_t capacity, char **error) {
    uint16_t ss = meta->g->sector_size;
    if (count > capacity) {
        xfs_set_error(error, "XFS AGFL capacity cannot hold unused allocation-tree reserve blocks");
        return -1;
    }
    uint8_t *raw = ld_xcalloc(ss, 1);
    memcpy(raw, MAGIC_AGFL, 4);
    xfs_put_be32(raw + 4, agno);
    memcpy(raw + 8, meta->g->meta_uuid, 16);
    xfs_put_be64(raw + 24, 0);
    for (uint32_t index = 0; index < capacity; ++index)
        xfs_put_be32(raw + 36U + (size_t)index * 4U, XFS_NULLAGBLOCK);
    for (size_t index = 0; index < count; ++index)
        xfs_put_be32(raw + 36U + index * 4U, (uint32_t)entries[index]);
    xfs_write_crc_le(raw, ss, XFS_AGFL_CRC_FIELD);
    int result = meta_write(meta, raw, ss,
                            xfs_ag_offset(meta->g, agno, 0) + 3U * ss,
                            "XFS AGFL", error);
    free(raw);
    return result;
}

static size_t tree_required_blocks(const TreeSpec *spec, uint32_t bs, size_t records) {
    size_t leafcap = (bs - XFS_META_HEADER) / spec->record_size;
    size_t nodecap = (bs - XFS_META_HEADER) / (spec->key_size + 4U);
    if (records == 0 || leafcap < 2 || nodecap < 2) return 0;
    size_t count = (records + leafcap - 1U) / leafcap;
    size_t total = count;
    while (count > 1U) {
        count = (count + nodecap - 1U) / nodecap;
        if (SIZE_MAX - total < count) return 0;
        total += count;
    }
    return total;
}

static int emit_tree_block(const RawMeta *meta, uint32_t agno, const TreeSpec *spec,
                           uint32_t block, uint16_t level, uint16_t nrecs,
                           uint32_t left, uint32_t right, const RawVec *leaf_records,
                           size_t leaf_start, const SummaryVec *children, size_t child_start,
                           char **error) {
    uint32_t bs = meta->g->block_size;
    uint8_t *raw = ld_xcalloc(bs, 1);
    memcpy(raw, spec->magic, 4);
    xfs_put_be16(raw + 4, level);
    xfs_put_be16(raw + 6, nrecs);
    xfs_put_be32(raw + 8, left);
    xfs_put_be32(raw + 12, right);
    uint64_t daddr = ((uint64_t)agno * meta->g->agblocks + block) * (bs / 512U);
    xfs_put_be64(raw + 16, daddr);
    xfs_put_be64(raw + 24, 0);
    memcpy(raw + 32, meta->g->meta_uuid, 16);
    xfs_put_be32(raw + 48, agno);
    if (level == 0) {
        for (uint16_t index = 0; index < nrecs; ++index)
            memcpy(raw + XFS_META_HEADER + (size_t)index * spec->record_size,
                   raw_at_const(leaf_records, leaf_start + index), spec->record_size);
    } else {
        size_t maxrecs = (bs - XFS_META_HEADER) / (spec->key_size + 4U);
        size_t ptrbase = XFS_META_HEADER + maxrecs * spec->key_size;
        for (uint16_t index = 0; index < nrecs; ++index) {
            const NodeSummary *child = &children->items[child_start + index];
            uint8_t *key = raw + XFS_META_HEADER + (size_t)index * spec->key_size;
            memcpy(key, child->low, spec->summary_size);
            if (spec->kind == TREE_RMAPBT)
                memcpy(key + spec->summary_size, child->high, spec->summary_size);
            xfs_put_be32(raw + ptrbase + (size_t)index * 4U, child->block);
        }
    }
    xfs_write_crc_le(raw, bs, XFS_BTREE_CRC_FIELD);
    int result = meta_write(meta, raw, bs, xfs_ag_offset(meta->g, agno, block),
                            "XFS allocation B+tree block", error);
    free(raw);
    return result;
}

static int build_tree(const RawMeta *meta, uint32_t agno, const TreeSpec *spec,
                      const RawVec *records, const uint64_t *blocks, size_t block_count,
                      uint32_t *root, uint32_t *levels, XfsU64Vec *used, char **error) {
    size_t required = tree_required_blocks(spec, meta->g->block_size, records->count);
    if (required == 0 || required > block_count) {
        xfs_set_error(error, "XFS metadata B+tree needs %zu block(s), only %zu are reserved",
                      required, block_count);
        return -1;
    }
    size_t leafcap = (meta->g->block_size - XFS_META_HEADER) / spec->record_size;
    size_t nodecap = (meta->g->block_size - XFS_META_HEADER) / (spec->key_size + 4U);
    size_t cursor = 0;
    size_t leaf_count = (records->count + leafcap - 1U) / leafcap;
    SummaryVec current = {0};
    for (size_t leaf = 0; leaf < leaf_count; ++leaf) {
        size_t start = leaf * leafcap;
        size_t count = records->count - start;
        if (count > leafcap) count = leafcap;
        uint32_t block = (uint32_t)blocks[cursor++];
        uint32_t left = leaf == 0 ? XFS_NULLAGBLOCK : (uint32_t)blocks[leaf - 1U];
        uint32_t right = leaf + 1U == leaf_count ? XFS_NULLAGBLOCK : (uint32_t)blocks[leaf + 1U];
        if (emit_tree_block(meta, agno, spec, block, 0, (uint16_t)count, left, right,
                            records, start, NULL, 0, error) != 0) {
            summary_free(&current);
            return -1;
        }
        NodeSummary summary = {.block = block};
        uint8_t unused[20];
        record_summary(spec, raw_at_const(records, start), summary.low, unused);
        record_summary(spec, raw_at_const(records, start + count - 1U), unused, summary.high);
        summary_push(&current, &summary);
        xfs_u64_push(used, block);
    }
    uint16_t level = 1;
    while (current.count > 1U) {
        size_t parent_count = (current.count + nodecap - 1U) / nodecap;
        SummaryVec parents = {0};
        size_t level_block_start = cursor;
        for (size_t parent = 0; parent < parent_count; ++parent) {
            size_t child_start = parent * nodecap;
            size_t count = current.count - child_start;
            if (count > nodecap) count = nodecap;
            uint32_t block = (uint32_t)blocks[cursor++];
            uint32_t left = parent == 0 ? XFS_NULLAGBLOCK : (uint32_t)blocks[level_block_start + parent - 1U];
            uint32_t right = parent + 1U == parent_count ? XFS_NULLAGBLOCK : (uint32_t)blocks[level_block_start + parent + 1U];
            if (emit_tree_block(meta, agno, spec, block, level, (uint16_t)count,
                                left, right, NULL, 0, &current, child_start, error) != 0) {
                summary_free(&current);
                summary_free(&parents);
                return -1;
            }
            NodeSummary summary = {.block = block};
            memcpy(summary.low, current.items[child_start].low, spec->summary_size);
            memcpy(summary.high, current.items[child_start + count - 1U].high, spec->summary_size);
            summary_push(&parents, &summary);
            xfs_u64_push(used, block);
        }
        summary_free(&current);
        current = parents;
        level++;
        if (level > XFS_NATIVE_MAX_BTREE_LEVEL) {
            summary_free(&current);
            xfs_set_error(error, "XFS allocation B+tree exceeds supported height");
            return -1;
        }
    }
    *root = current.items[0].block;
    *levels = level;
    summary_free(&current);
    return 0;
}

static void append_source_file_ranges(const XfsCatalogue *source, XfsRangeVec *ranges) {
    for (size_t index = 0; index < source->objects.count; ++index) {
        const XfsObject *object = &source->objects.items[index];
        if (!object->is_file) continue;
        for (size_t extent_index = 0; extent_index < object->extents.count; ++extent_index) {
            XfsExtent extent = object->extents.items[extent_index];
            xfs_range_push(ranges, extent.physical, extent.physical + extent.length);
        }
    }
}

static int target_ranges(sqlite3 *db, XfsRangeVec *ranges, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT target FROM blocks ORDER BY target", -1, &stmt, NULL) != SQLITE_OK) {
        xfs_set_error(error, "cannot query XFS target blocks: %s", sqlite3_errmsg(db));
        return -1;
    }
    bool have = false;
    uint64_t start = 0, previous = 0;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 value = sqlite3_column_int64(stmt, 0);
        if (value < 0) {
            sqlite3_finalize(stmt);
            xfs_set_error(error, "negative target block in XFS plan database");
            return -1;
        }
        uint64_t block = (uint64_t)value;
        if (!have) {
            start = previous = block;
            have = true;
        } else if (block == previous + 1U) {
            previous = block;
        } else {
            xfs_range_push(ranges, start, previous + 1U);
            start = previous = block;
        }
    }
    if (step != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        xfs_set_error(error, "cannot read XFS target blocks: %s", sqlite3_errmsg(db));
        return -1;
    }
    if (have) xfs_range_push(ranges, start, previous + 1U);
    sqlite3_finalize(stmt);
    return 0;
}

static int subtract_ranges(XfsRangeVec *base, const XfsRangeVec *cuts) {
    xfs_range_sort_merge(base);
    for (size_t cut_index = 0; cut_index < cuts->count; ++cut_index) {
        XfsRange cut = cuts->items[cut_index];
        XfsRangeVec next = {0};
        for (size_t range_index = 0; range_index < base->count; ++range_index) {
            XfsRange range = base->items[range_index];
            if (cut.end <= range.start || cut.start >= range.end) {
                xfs_range_push(&next, range.start, range.end);
                continue;
            }
            if (range.start < cut.start) xfs_range_push(&next, range.start, cut.start);
            if (cut.end < range.end) xfs_range_push(&next, cut.end, range.end);
        }
        xfs_range_free(base);
        *base = next;
    }
    return 0;
}

static int expected_free_ranges(const XfsCatalogue *source, sqlite3 *db,
                                XfsRangeVec *out, char **error) {
    for (size_t index = 0; index < source->free_ranges.count; ++index)
        xfs_range_push(out, source->free_ranges.items[index].start, source->free_ranges.items[index].end);
    append_source_file_ranges(source, out);
    xfs_range_sort_merge(out);
    XfsRangeVec targets = {0};
    if (target_ranges(db, &targets, error) != 0) {
        xfs_range_free(out);
        return -1;
    }
    xfs_range_sort_merge(&targets);
    subtract_ranges(out, &targets);
    xfs_range_free(&targets);
    return 0;
}

static void split_ag_ranges(const XfsRangeVec *global, const XfsGeometry *g,
                            uint32_t agno, XfsRangeVec *local) {
    uint64_t base = (uint64_t)agno * g->agblocks;
    uint64_t limit = base + xfs_ag_length(g, agno);
    for (size_t index = 0; index < global->count; ++index) {
        uint64_t left = global->items[index].start > base ? global->items[index].start : base;
        uint64_t right = global->items[index].end < limit ? global->items[index].end : limit;
        if (left < right) xfs_range_push(local, left - base, right - base);
    }
    xfs_range_sort_merge(local);
}

static void make_bno_records(const XfsRangeVec *free_ranges, RawVec *records) {
    raw_init(records, 8);
    uint8_t raw[8];
    for (size_t index = 0; index < free_ranges->count; ++index) {
        xfs_put_be32(raw, (uint32_t)free_ranges->items[index].start);
        xfs_put_be32(raw + 4, (uint32_t)(free_ranges->items[index].end - free_ranges->items[index].start));
        raw_push(records, raw);
    }
}

typedef struct { uint32_t start; uint32_t length; } FreeRecord;
static int free_record_count_compare(const void *left0, const void *right0) {
    const FreeRecord *left = left0, *right = right0;
    if (left->length < right->length) return -1;
    if (left->length > right->length) return 1;
    return left->start < right->start ? -1 : left->start > right->start ? 1 : 0;
}

static void make_cnt_records(const XfsRangeVec *free_ranges, RawVec *records) {
    raw_init(records, 8);
    FreeRecord *ordered = free_ranges->count ? ld_xcalloc(free_ranges->count, sizeof(*ordered)) : NULL;
    for (size_t index = 0; index < free_ranges->count; ++index) {
        ordered[index].start = (uint32_t)free_ranges->items[index].start;
        ordered[index].length = (uint32_t)(free_ranges->items[index].end - free_ranges->items[index].start);
    }
    qsort(ordered, free_ranges->count, sizeof(*ordered), free_record_count_compare);
    uint8_t raw[8];
    for (size_t index = 0; index < free_ranges->count; ++index) {
        xfs_put_be32(raw, ordered[index].start);
        xfs_put_be32(raw + 4, ordered[index].length);
        raw_push(records, raw);
    }
    free(ordered);
}

static void encode_rmap(const XfsRmapRecord *record, uint8_t out[24]) {
    xfs_put_be32(out, record->start);
    xfs_put_be32(out + 4, record->count);
    xfs_put_be64(out + 8, record->owner);
    xfs_put_be64(out + 16, record->offset_flags);
}

static void decode_rmaps(const RawVec *records, XfsRmapVec *out) {
    for (size_t index = 0; index < records->count; ++index) {
        const uint8_t *raw = raw_at_const(records, index);
        xfs_rmap_push(out, (XfsRmapRecord){
            xfs_be32(raw), xfs_be32(raw + 4), xfs_be64(raw + 8), xfs_be64(raw + 16)
        });
    }
}

static bool source_has_file_inode(const XfsCatalogue *source, uint64_t inode) {
    for (size_t index = 0; index < source->objects.count; ++index)
        if (source->objects.items[index].is_file && source->objects.items[index].inode == inode)
            return true;
    return false;
}

static void semantics_from_source(const XfsCatalogue *source, uint32_t agno, SemanticVec *out) {
    uint64_t base = (uint64_t)agno * source->geometry.agblocks;
    uint64_t limit = base + xfs_ag_length(&source->geometry, agno);
    for (size_t index = 0; index < source->objects.count; ++index) {
        const XfsObject *object = &source->objects.items[index];
        if (!object->is_file) continue;
        for (size_t e = 0; e < object->extents.count; ++e) {
            XfsExtent extent = object->extents.items[e];
            uint64_t left = extent.physical > base ? extent.physical : base;
            uint64_t right0 = extent.physical + extent.length;
            uint64_t right = right0 < limit ? right0 : limit;
            if (left >= right) continue;
            uint64_t delta = left - extent.physical;
            semantic_push(out, (SemanticRmap){
                object->inode, extent.logical + delta, (uint32_t)(left - base),
                (uint32_t)(right - left), extent.unwritten
            });
        }
    }
    semantic_sort_coalesce(out);
}

static void semantics_from_rmaps(const XfsRmapVec *records, bool only_files,
                                 const XfsCatalogue *source, SemanticVec *out) {
    for (size_t index = 0; index < records->count; ++index) {
        XfsRmapRecord record = records->items[index];
        if ((record.offset_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK)) != 0) continue;
        if (only_files && !source_has_file_inode(source, record.owner)) continue;
        if (!only_files && source_has_file_inode(source, record.owner)) continue;
        semantic_push(out, (SemanticRmap){
            record.owner, rmap_offset(&record), record.start, record.count,
            (record.offset_flags & XFS_RMAP_UNWRITTEN) != 0
        });
    }
    semantic_sort_coalesce(out);
}

static int check_source_rmaps(const XfsCatalogue *source, const XfsRmapVec *records,
                              uint32_t agno, XfsRmapVec *kept, char **error) {
    SemanticVec actual = {0}, expected = {0};
    semantics_from_rmaps(records, true, source, &actual);
    semantics_from_source(source, agno, &expected);
    if (!semantic_equal(&actual, &expected)) {
        semantic_free(&actual);
        semantic_free(&expected);
        xfs_set_error(error, "XFS rmap ownership for movable files is incomplete in AG %u", agno);
        return -1;
    }
    semantic_free(&actual);
    semantic_free(&expected);
    for (size_t index = 0; index < records->count; ++index) {
        XfsRmapRecord record = records->items[index];
        bool movable_data = source_has_file_inode(source, record.owner) &&
            (record.offset_flags & (XFS_RMAP_ATTR_FORK | XFS_RMAP_BMBT_BLOCK)) == 0;
        if (!movable_data) xfs_rmap_push(kept, record);
    }
    return 0;
}

/* Correct target-rmap builder with explicit first-row state. */
static int target_rmaps(sqlite3 *db, const XfsGeometry *g, AgRmapVec *out, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT inode,logical,target,unwritten FROM blocks ORDER BY inode,logical",
            -1, &stmt, NULL) != SQLITE_OK) {
        xfs_set_error(error, "cannot query XFS target reverse mappings: %s", sqlite3_errmsg(db));
        return -1;
    }
    bool have = false;
    uint64_t inode = 0, logical = 0, target = 0;
    bool unwritten = false;
    uint32_t count = 0;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 i0 = sqlite3_column_int64(stmt, 0);
        sqlite3_int64 l0 = sqlite3_column_int64(stmt, 1);
        sqlite3_int64 t0 = sqlite3_column_int64(stmt, 2);
        if (i0 < 0 || l0 < 0 || t0 < 0) {
            sqlite3_finalize(stmt);
            xfs_set_error(error, "negative value in XFS target reverse mapping plan");
            return -1;
        }
        uint64_t ni = (uint64_t)i0, nl = (uint64_t)l0, nt = (uint64_t)t0;
        bool nu = sqlite3_column_int(stmt, 3) != 0;
        uint64_t nag = nt / g->agblocks;
        if (!have) {
            inode = ni; logical = nl; target = nt; unwritten = nu; count = 1; have = true;
            continue;
        }
        if (inode == ni && logical + count == nl && target + count == nt &&
            unwritten == nu && target / g->agblocks == nag && count < UINT32_MAX) {
            count++;
            continue;
        }
        ag_rmap_push(out, (AgRmap){
            (uint32_t)(target / g->agblocks),
            {(uint32_t)(target % g->agblocks), count, inode,
             logical | (unwritten ? XFS_RMAP_UNWRITTEN : 0)}
        });
        inode = ni; logical = nl; target = nt; unwritten = nu; count = 1;
    }
    if (step != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        xfs_set_error(error, "cannot read XFS target reverse mappings: %s", sqlite3_errmsg(db));
        return -1;
    }
    if (have) {
        ag_rmap_push(out, (AgRmap){
            (uint32_t)(target / g->agblocks),
            {(uint32_t)(target % g->agblocks), count, inode,
             logical | (unwritten ? XFS_RMAP_UNWRITTEN : 0)}
        });
    }
    sqlite3_finalize(stmt);
    return 0;
}

static void append_pool(XfsU64Vec *pool, const XfsU64Vec *from) {
    for (size_t index = 0; index < from->count; ++index) xfs_u64_push(pool, from->items[index]);
}

static int unique_pool(XfsU64Vec *pool, size_t expected, char **error) {
    if (pool->count > 1) qsort(pool->items, pool->count, sizeof(*pool->items), u64_compare);
    for (size_t index = 1; index < pool->count; ++index) {
        if (pool->items[index] == pool->items[index - 1]) {
            xfs_set_error(error, "XFS AG metadata reserve contains duplicate blocks");
            return -1;
        }
    }
    if (pool->count != expected) {
        xfs_set_error(error, "XFS AG metadata reserve block count is inconsistent");
        return -1;
    }
    return 0;
}

static uint64_t free_block_count(const XfsRangeVec *ranges) {
    uint64_t total = 0;
    for (size_t index = 0; index < ranges->count; ++index) total += ranges->items[index].end - ranges->items[index].start;
    return total;
}

static uint64_t longest_free(const XfsRangeVec *ranges) {
    uint64_t longest = 0;
    for (size_t index = 0; index < ranges->count; ++index) {
        uint64_t length = ranges->items[index].end - ranges->items[index].start;
        if (length > longest) longest = length;
    }
    return longest;
}

int xfs_rebuild_allocation_metadata(const char *path, const XfsCatalogue *source,
                                    sqlite3 *db, char **error) {
    const XfsGeometry *g = &source->geometry;
    if (!g->v5) {
        xfs_set_error(error, "raw XFS mutation currently requires the CRC-enabled v5 format");
        return -1;
    }
    XfsRangeVec expected_free = {0};
    if (expected_free_ranges(source, db, &expected_free, error) != 0) return -1;
    AgRmapVec targets = {0};
    if (target_rmaps(db, g, &targets, error) != 0) {
        xfs_range_free(&expected_free);
        return -1;
    }
    RawMeta meta;
    if (meta_open(&meta, path, g, true, error) != 0) {
        xfs_range_free(&expected_free); ag_rmap_free(&targets); return -1;
    }
    int result = 0;
    for (uint32_t agno = 0; agno < g->agcount && result == 0; ++agno) {
        uint8_t *agf = ld_xmalloc(g->sector_size);
        XfsU64Vec bno_blocks = {0}, cnt_blocks = {0}, rmap_blocks = {0}, agfl = {0}, pool = {0};
        RawVec bno_old = {0}, cnt_old = {0}, rmap_old_raw = {0};
        XfsRmapVec old_rmaps = {0}, new_rmaps = {0};
        XfsRangeVec free_local = {0};
        RawVec bno_records = {0}, cnt_records = {0}, rmap_records = {0};
        XfsU64Vec bno_used = {0}, cnt_used = {0}, rmap_used = {0};
        uint32_t capacity = 0;
        if (read_agf(&meta, agno, agf, error) != 0) { result = -1; goto ag_done; }
        uint32_t bnoroot = xfs_be32(agf + 16), cntroot = xfs_be32(agf + 20), rmaproot = xfs_be32(agf + 24);
        if (read_tree(&meta, agno, bnoroot, &SPEC_BNOBT, &bno_blocks, &bno_old, error) != 0 ||
            read_tree(&meta, agno, cntroot, &SPEC_CNTBT, &cnt_blocks, &cnt_old, error) != 0) {
            result = -1; goto ag_done;
        }
        bool has_rmap = (g->features_ro_compat & XFS_SB_FEAT_RO_COMPAT_RMAPBT) != 0;
        if (has_rmap) {
            if (rmaproot == 0) { xfs_set_error(error, "XFS rmapbt feature is enabled but AG %u has no rmap root", agno); result = -1; goto ag_done; }
            if (read_tree(&meta, agno, rmaproot, &SPEC_RMAPBT, &rmap_blocks, &rmap_old_raw, error) != 0) { result = -1; goto ag_done; }
            decode_rmaps(&rmap_old_raw, &old_rmaps);
        }
        if (read_agfl(&meta, agno, agf, &agfl, &capacity, error) != 0) { result = -1; goto ag_done; }
        append_pool(&pool, &bno_blocks); append_pool(&pool, &cnt_blocks); append_pool(&pool, &rmap_blocks); append_pool(&pool, &agfl);
        if (unique_pool(&pool, bno_blocks.count + cnt_blocks.count + rmap_blocks.count + agfl.count, error) != 0) { result = -1; goto ag_done; }
        split_ag_ranges(&expected_free, g, agno, &free_local);
        if (free_local.count == 0) { xfs_set_error(error, "XFS allocation group %u has no free extent for its free-space trees", agno); result = -1; goto ag_done; }
        make_bno_records(&free_local, &bno_records);
        make_cnt_records(&free_local, &cnt_records);
        size_t nb = tree_required_blocks(&SPEC_BNOBT, g->block_size, bno_records.count);
        size_t nc = tree_required_blocks(&SPEC_CNTBT, g->block_size, cnt_records.count);
        size_t nr = 0;
        if (has_rmap) {
            if (check_source_rmaps(source, &old_rmaps, agno, &new_rmaps, error) != 0) { result = -1; goto ag_done; }
            for (size_t index = 0; index < targets.count; ++index)
                if (targets.items[index].agno == agno) xfs_rmap_push(&new_rmaps, targets.items[index].record);
            if (new_rmaps.count == 0) { xfs_set_error(error, "XFS rmapbt in AG %u would become empty", agno); result = -1; goto ag_done; }
            qsort(new_rmaps.items, new_rmaps.count, sizeof(*new_rmaps.items), rmap_compare);
            raw_init(&rmap_records, 24);
            for (size_t index = 0; index < new_rmaps.count; ++index) {
                uint8_t encoded[24]; encode_rmap(&new_rmaps.items[index], encoded); raw_push(&rmap_records, encoded);
            }
            nr = tree_required_blocks(&SPEC_RMAPBT, g->block_size, rmap_records.count);
        }
        size_t required = nb + nc + nr;
        if (required > pool.count) {
            xfs_set_error(error, "XFS AG %u needs %zu allocation-tree blocks but has only %zu existing tree/AGFL reserve blocks", agno, required, pool.count);
            result = -1; goto ag_done;
        }
        size_t cursor = 0;
        uint32_t bno_root_new = 0, bno_levels = 0, cnt_root_new = 0, cnt_levels = 0, rmap_root_new = 0, rmap_levels = 0;
        if (build_tree(&meta, agno, &SPEC_BNOBT, &bno_records, pool.items + cursor, nb,
                       &bno_root_new, &bno_levels, &bno_used, error) != 0) { result = -1; goto ag_done; }
        cursor += nb;
        if (build_tree(&meta, agno, &SPEC_CNTBT, &cnt_records, pool.items + cursor, nc,
                       &cnt_root_new, &cnt_levels, &cnt_used, error) != 0) { result = -1; goto ag_done; }
        cursor += nc;
        if (has_rmap) {
            if (build_tree(&meta, agno, &SPEC_RMAPBT, &rmap_records, pool.items + cursor, nr,
                           &rmap_root_new, &rmap_levels, &rmap_used, error) != 0) { result = -1; goto ag_done; }
            cursor += nr;
        }
        size_t leftover = pool.count - cursor;
        if (leftover > capacity || write_agfl(&meta, agno, pool.items + cursor, leftover, capacity, error) != 0) { result = -1; goto ag_done; }
        xfs_put_be32(agf + 16, bno_root_new); xfs_put_be32(agf + 20, cnt_root_new); xfs_put_be32(agf + 24, rmap_root_new);
        xfs_put_be32(agf + 28, bno_levels); xfs_put_be32(agf + 32, cnt_levels); xfs_put_be32(agf + 36, rmap_levels);
        xfs_put_be32(agf + 40, 0); xfs_put_be32(agf + 44, leftover ? (uint32_t)(leftover - 1U) : capacity - 1U); xfs_put_be32(agf + 48, (uint32_t)leftover);
        uint64_t free_blocks = free_block_count(&free_local), longest = longest_free(&free_local);
        if (free_blocks > UINT32_MAX || longest > UINT32_MAX) { xfs_set_error(error, "XFS AG free-space counter exceeds on-disk width"); result = -1; goto ag_done; }
        xfs_put_be32(agf + 52, (uint32_t)free_blocks); xfs_put_be32(agf + 56, (uint32_t)longest);
        size_t nonroots = (bno_used.count ? bno_used.count - 1U : 0) + (cnt_used.count ? cnt_used.count - 1U : 0) + (rmap_used.count ? rmap_used.count - 1U : 0);
        if (nonroots > UINT32_MAX || rmap_used.count > UINT32_MAX) { xfs_set_error(error, "XFS AG B+tree counter exceeds on-disk width"); result = -1; goto ag_done; }
        xfs_put_be32(agf + 60, (uint32_t)nonroots); xfs_put_be32(agf + 80, (uint32_t)rmap_used.count);
        xfs_put_be64(agf + 208, 0); xfs_write_crc_le(agf, g->sector_size, XFS_AGF_CRC_FIELD);
        if (meta_write(&meta, agf, g->sector_size, xfs_ag_offset(g, agno, 0) + g->sector_size, "XFS AGF", error) != 0) result = -1;
ag_done:
        free(agf); xfs_u64_free(&bno_blocks); xfs_u64_free(&cnt_blocks); xfs_u64_free(&rmap_blocks); xfs_u64_free(&agfl); xfs_u64_free(&pool);
        raw_free(&bno_old); raw_free(&cnt_old); raw_free(&rmap_old_raw); xfs_rmap_free(&old_rmaps); xfs_rmap_free(&new_rmaps); xfs_range_free(&free_local);
        raw_free(&bno_records); raw_free(&cnt_records); raw_free(&rmap_records); xfs_u64_free(&bno_used); xfs_u64_free(&cnt_used); xfs_u64_free(&rmap_used);
    }
    if (result == 0 && fsync(meta.fd) != 0) { xfs_set_error(error, "cannot sync rebuilt XFS allocation metadata: %s", strerror(errno)); result = -1; }
    meta_close(&meta); xfs_range_free(&expected_free); ag_rmap_free(&targets);
    return result;
}

static bool range_vec_equal(const XfsRangeVec *left, const XfsRangeVec *right) {
    if (left->count != right->count) return false;
    for (size_t index = 0; index < left->count; ++index)
        if (left->items[index].start != right->items[index].start || left->items[index].end != right->items[index].end) return false;
    return true;
}

static int verify_bno_order(const RawVec *records, XfsRangeVec *out, char **error) {
    uint64_t previous_end = 0;
    bool have = false;
    for (size_t index = 0; index < records->count; ++index) {
        const uint8_t *raw = raw_at_const(records, index);
        uint32_t start = xfs_be32(raw), length = xfs_be32(raw + 4);
        if (length == 0 || (have && start < previous_end)) {
            xfs_set_error(error, "XFS bnobt records are not strictly ordered");
            return -1;
        }
        xfs_range_push(out, start, (uint64_t)start + length);
        previous_end = (uint64_t)start + length;
        have = true;
    }
    return 0;
}

static int verify_cnt_order(const RawVec *records, char **error) {
    uint32_t previous_length = 0, previous_start = 0;
    bool have = false;
    for (size_t index = 0; index < records->count; ++index) {
        const uint8_t *raw = raw_at_const(records, index);
        uint32_t start = xfs_be32(raw), length = xfs_be32(raw + 4);
        if (length == 0 || (have && (length < previous_length || (length == previous_length && start < previous_start)))) {
            xfs_set_error(error, "XFS cntbt records are not sorted by length/start");
            return -1;
        }
        previous_length = length; previous_start = start; have = true;
    }
    return 0;
}

static void semantics_from_target(const AgRmapVec *targets, uint32_t agno, SemanticVec *out) {
    for (size_t index = 0; index < targets->count; ++index) {
        if (targets->items[index].agno != agno) continue;
        XfsRmapRecord r = targets->items[index].record;
        semantic_push(out, (SemanticRmap){r.owner, rmap_offset(&r), r.start, r.count,
                                          (r.offset_flags & XFS_RMAP_UNWRITTEN) != 0});
    }
    semantic_sort_coalesce(out);
}

int xfs_verify_allocation_metadata(const char *path, const XfsCatalogue *source,
                                   sqlite3 *db, char **error) {
    XfsRangeVec expected_free = {0}, actual_global = {0};
    if (expected_free_ranges(source, db, &expected_free, error) != 0) return -1;
    AgRmapVec targets = {0};
    if (target_rmaps(db, &source->geometry, &targets, error) != 0) { xfs_range_free(&expected_free); return -1; }
    RawMeta meta;
    if (meta_open(&meta, path, &source->geometry, false, error) != 0) { xfs_range_free(&expected_free); ag_rmap_free(&targets); return -1; }
    int result = 0;
    for (uint32_t agno = 0; agno < source->geometry.agcount && result == 0; ++agno) {
        uint8_t *agf = ld_xmalloc(source->geometry.sector_size);
        XfsU64Vec bno_blocks = {0}, cnt_blocks = {0}, rmap_blocks = {0}, agfl = {0};
        RawVec bno_raw = {0}, cnt_raw = {0}, rmap_raw = {0};
        XfsRangeVec bno = {0}, expected_ag = {0};
        uint32_t capacity = 0;
        if (read_agf(&meta, agno, agf, error) != 0) { result = -1; goto verify_done; }
        if (le32(agf + XFS_AGF_CRC_FIELD) != xfs_crc_field(agf, source->geometry.sector_size, XFS_AGF_CRC_FIELD)) {
            xfs_set_error(error, "XFS AGF CRC mismatch in allocation group %u", agno); result = -1; goto verify_done;
        }
        if (read_tree(&meta, agno, xfs_be32(agf + 16), &SPEC_BNOBT, &bno_blocks, &bno_raw, error) != 0 ||
            read_tree(&meta, agno, xfs_be32(agf + 20), &SPEC_CNTBT, &cnt_blocks, &cnt_raw, error) != 0) { result = -1; goto verify_done; }
        if (verify_bno_order(&bno_raw, &bno, error) != 0 || verify_cnt_order(&cnt_raw, error) != 0) { result = -1; goto verify_done; }
        split_ag_ranges(&expected_free, &source->geometry, agno, &expected_ag);
        if (!range_vec_equal(&bno, &expected_ag)) { xfs_set_error(error, "XFS bnobt does not describe the planned free map in AG %u", agno); result = -1; goto verify_done; }
        uint64_t base = (uint64_t)agno * source->geometry.agblocks;
        for (size_t index = 0; index < bno.count; ++index) xfs_range_push(&actual_global, base + bno.items[index].start, base + bno.items[index].end);
        uint64_t free_count = free_block_count(&bno), longest = longest_free(&bno);
        if (free_count != xfs_be32(agf + 52) || longest != xfs_be32(agf + 56)) { xfs_set_error(error, "XFS AGF free-space counters disagree with bnobt"); result = -1; goto verify_done; }
        if (read_agfl(&meta, agno, agf, &agfl, &capacity, error) != 0) { result = -1; goto verify_done; }
        if ((source->geometry.features_ro_compat & XFS_SB_FEAT_RO_COMPAT_RMAPBT) != 0) {
            if (read_tree(&meta, agno, xfs_be32(agf + 24), &SPEC_RMAPBT, &rmap_blocks, &rmap_raw, error) != 0) { result = -1; goto verify_done; }
            XfsRmapVec rmaps = {0}; decode_rmaps(&rmap_raw, &rmaps);
            for (size_t index = 1; index < rmaps.count; ++index) if (rmap_compare(&rmaps.items[index - 1], &rmaps.items[index]) > 0) {
                xfs_set_error(error, "XFS rmapbt leaf records are not sorted"); result = -1; break;
            }
            if (result == 0) {
                SemanticVec actual = {0}, wanted = {0};
                semantics_from_rmaps(&rmaps, true, source, &actual);
                semantics_from_target(&targets, agno, &wanted);
                if (!semantic_equal(&actual, &wanted)) { xfs_set_error(error, "XFS rmapbt file ownership differs from target inode mappings in AG %u", agno); result = -1; }
                semantic_free(&actual); semantic_free(&wanted);
            }
            xfs_rmap_free(&rmaps);
        }
verify_done:
        free(agf); xfs_u64_free(&bno_blocks); xfs_u64_free(&cnt_blocks); xfs_u64_free(&rmap_blocks); xfs_u64_free(&agfl);
        raw_free(&bno_raw); raw_free(&cnt_raw); raw_free(&rmap_raw); xfs_range_free(&bno); xfs_range_free(&expected_ag);
    }
    xfs_range_sort_merge(&actual_global);
    if (result == 0 && !range_vec_equal(&actual_global, &expected_free)) {
        xfs_set_error(error, "rebuilt XFS free-space trees do not match the planned global free map"); result = -1;
    }
    meta_close(&meta); xfs_range_free(&expected_free); xfs_range_free(&actual_global); ag_rmap_free(&targets);
    return result;
}

typedef struct {
    uint64_t block;
    uint32_t cycle;
    uint32_t version;
    uint32_t length;
    uint32_t numops;
    uint32_t header_size;
    uint64_t lsn;
    uint64_t end_block;
} XfsLogRecord;

/*
 * XFS stamps the first word of each 512-byte log basic block with the write
 * cycle.  Record-header blocks are the exception: their first word is the
 * header magic and h_cycle carries the stamp.  This mirrors xlog_get_cycle()
 * closely enough for the read-only clean-log proof below.
 */
static uint32_t xfs_log_block_cycle(const uint8_t raw[512]) {
    if (xfs_be32(raw) == XFS_LOG_MAGIC)
        return xfs_be32(raw + 4);
    return xfs_be32(raw);
}

static uint64_t xfs_log_wrap(uint64_t block, uint64_t log_bbs) {
    return block % log_bbs;
}

static uint64_t xfs_log_header_bbs(const uint8_t raw[512]) {
    uint32_t h_size = xfs_be32(raw + 320);
    uint32_t version = xfs_be32(raw + 8);
    if ((version & 2U) != 0 && h_size > 32U * 1024U)
        return ((uint64_t)h_size + 32U * 1024U - 1U) / (32U * 1024U);
    return 1U;
}

static bool xfs_log_header_decode(const uint8_t raw[512], uint64_t block,
                                  uint64_t log_bbs, const uint8_t uuid[16],
                                  XfsLogRecord *record) {
    if (xfs_be32(raw) != XFS_LOG_MAGIC)
        return false;
    uint32_t cycle = xfs_be32(raw + 4);
    uint32_t version = xfs_be32(raw + 8);
    uint32_t length = xfs_be32(raw + 12);
    uint64_t lsn = xfs_be64(raw + 16);
    uint32_t numops = xfs_be32(raw + 40);
    if (cycle == 0 || (version & 3U) == 0 || length == 0 || numops == 0)
        return false;
    if ((uint64_t)length > log_bbs * 512U)
        return false;
    if (memcmp(raw + 304, uuid, 16) != 0)
        return false;
    if ((lsn >> 32) != cycle || (lsn & UINT64_C(0xffffffff)) != block)
        return false;
    uint64_t hblks = xfs_log_header_bbs(raw);
    uint64_t data_bbs = ((uint64_t)length + 511U) / 512U;
    if (hblks == 0 || hblks >= log_bbs || data_bbs == 0 || data_bbs >= log_bbs)
        return false;
    record->block = block;
    record->cycle = cycle;
    record->version = version;
    record->length = length;
    record->numops = numops;
    record->header_size = xfs_be32(raw + 320);
    record->lsn = lsn;
    record->end_block = xfs_log_wrap(block + hblks + data_bbs, log_bbs);
    return true;
}

/*
 * Determine the physical XFS log head from cycle stamps, then trim an
 * estimate that lands in the middle of (or just after) a partial log record.
 *
 * This deliberately follows the recovery algorithm's semantics rather than
 * trying to infer recency from the numerically largest LSN.  The real XFS
 * recovery code first derives a cycle-based head, scans a bounded region for
 * stale cycle holes, and finally backs the head over an incomplete record.
 * Linear scans are preferable here: the offline defragger runs rarely and a
 * simple, auditable implementation is more valuable than the kernel's binary
 * search optimisation.
 */
static int xfs_log_read_cycles(int fd, uint64_t start, uint64_t log_bbs,
                               uint32_t **cycles_out, bool *all_zero,
                               char **error) {
    uint32_t *cycles = calloc((size_t)log_bbs, sizeof(*cycles));
    if (cycles == NULL) {
        xfs_set_error(error, "cannot allocate XFS log cycle map");
        return -1;
    }
    uint8_t raw[512];
    *all_zero = true;
    for (uint64_t bb = 0; bb < log_bbs; ++bb) {
        ssize_t got = ld_pread_full(fd, raw, sizeof(raw), start + bb * 512U);
        if (got < 0 || (size_t)got != sizeof(raw)) {
            free(cycles);
            xfs_set_error(error, "cannot read XFS internal log basic block %" PRIu64, bb);
            return -1;
        }
        cycles[bb] = xfs_log_block_cycle(raw);
        if (cycles[bb] != 0)
            *all_zero = false;
    }
    *cycles_out = cycles;
    return 0;
}

static uint64_t xfs_log_distance(uint64_t from, uint64_t to, uint64_t log_bbs) {
    if (to >= from)
        return to - from;
    return log_bbs - from + to;
}

/*
 * Search physically backwards from head, wrapping at block zero, exactly as
 * xlog_rseek_logrec_hdr() does.  The first record-header magic is authoritative:
 * if that header cannot be decoded for this filesystem, we fail closed instead
 * of skipping over corruption to an older stale header.
 */
static int xfs_log_rseek_previous_header(int fd, uint64_t start,
                                         uint64_t log_bbs, uint64_t head,
                                         uint64_t max_scan,
                                         const uint8_t uuid[16],
                                         XfsLogRecord *record,
                                         uint8_t header[512],
                                         char **error) {
    uint8_t raw[512];
    uint64_t limit = max_scan == 0 || max_scan > log_bbs ? log_bbs : max_scan;
    uint64_t normalized = head % log_bbs;
    for (uint64_t step = 1; step <= limit; ++step) {
        uint64_t bb = (normalized + log_bbs - step) % log_bbs;
        ssize_t got = ld_pread_full(fd, raw, sizeof(raw), start + bb * 512U);
        if (got < 0 || (size_t)got != sizeof(raw)) {
            xfs_set_error(error, "cannot read XFS log while seeking previous record header at block %" PRIu64, bb);
            return -1;
        }
        if (xfs_be32(raw) != XFS_LOG_MAGIC)
            continue;
        if (!xfs_log_header_decode(raw, bb, log_bbs, uuid, record)) {
            xfs_set_error(error,
                          "XFS log record header at block %" PRIu64 " is malformed or belongs to another filesystem",
                          bb);
            return -1;
        }
        memcpy(header, raw, 512U);
        return 0;
    }
    xfs_set_error(error,
                  "XFS internal log has no record header within %" PRIu64 " blocks before head %" PRIu64,
                  limit, normalized);
    return -1;
}

/*
 * Kernel XFS validates its cycle-derived head by looking backwards at the
 * nearest possible log record.  If the candidate does not sit exactly after
 * that record, the candidate is in a stale/partial area and is backed up to
 * the record header.  XLOG_MAX_ICLOGS * maximum v2 record size is 4096 BBs;
 * a single maximum v2 record is 512 BBs.  Using those v2 maxima is safe for
 * older log formats and keeps the implementation independent of kernel state.
 */
static int xfs_log_find_head(int fd, uint64_t start, uint64_t log_bbs,
                             const uint8_t uuid[16], uint64_t *head,
                             bool *all_zero, char **error) {
    uint32_t *cycles = NULL;
    if (xfs_log_read_cycles(fd, start, log_bbs, &cycles, all_zero, error) != 0)
        return -1;
    if (*all_zero) {
        *head = 0;
        free(cycles);
        return 0;
    }

    /* A zeroed tail is the never-wrapped case used by freshly initialised logs. */
    uint64_t first_zero = log_bbs;
    for (uint64_t bb = 0; bb < log_bbs; ++bb) {
        if (cycles[bb] == 0U) {
            first_zero = bb;
            break;
        }
    }
    uint64_t candidate;
    uint32_t stop_cycle = 0;
    if (first_zero != log_bbs) {
        for (uint64_t bb = first_zero; bb < log_bbs; ++bb) {
            if (cycles[bb] != 0U) {
                free(cycles);
                xfs_set_error(error,
                              "XFS internal log has nonzero data beyond its zeroed tail at block %" PRIu64,
                              bb);
                return -1;
            }
        }
        candidate = first_zero;
    } else {
        uint32_t first = cycles[0];
        uint32_t last = cycles[log_bbs - 1U];
        if (first == 0U || last == 0U) {
            free(cycles);
            xfs_set_error(error, "XFS internal log cycle map has an invalid zero-cycle boundary");
            return -1;
        }

        if (first == last) {
            /* log_bbs is the kernel's temporary sentinel for physical block 0. */
            candidate = log_bbs;
            stop_cycle = first == 1U ? UINT32_MAX : first - 1U;
        } else {
            /* Find the first occurrence of the older tail cycle. */
            candidate = log_bbs;
            for (uint64_t bb = 0; bb < log_bbs; ++bb) {
                if (cycles[bb] == last) {
                    candidate = bb;
                    break;
                }
            }
            if (candidate == log_bbs) {
                free(cycles);
                xfs_set_error(error, "XFS internal log has no physical boundary for its tail cycle");
                return -1;
            }
            stop_cycle = last;
        }

        /* Mirror XFS's bounded stale-cycle verification around the candidate. */
        uint64_t verify = log_bbs < 4096U ? log_bbs : 4096U;
        if (candidate >= verify) {
            uint64_t begin = candidate - verify;
            for (uint64_t bb = begin; bb < candidate; ++bb) {
                if (cycles[bb] == stop_cycle) {
                    candidate = bb;
                    break;
                }
            }
        } else {
            uint64_t tail_count = verify - candidate;
            uint64_t begin = log_bbs - tail_count;
            uint32_t previous = stop_cycle == 1U ? UINT32_MAX : stop_cycle - 1U;
            bool moved = false;
            for (uint64_t bb = begin; bb < log_bbs; ++bb) {
                if (cycles[bb] == previous) {
                    candidate = bb;
                    moved = true;
                    break;
                }
            }
            if (!moved) {
                for (uint64_t bb = 0; bb < candidate; ++bb) {
                    if (cycles[bb] == stop_cycle) {
                        candidate = bb;
                        break;
                    }
                }
            }
        }
    }
    free(cycles);

    /* Back an estimate out of a partial/stale log record, like
     * xlog_find_verify_log_record(). */
    uint64_t normalized = candidate % log_bbs;
    XfsLogRecord previous;
    uint8_t previous_header[512];
    char *seek_error = NULL;
    if (xfs_log_rseek_previous_header(fd, start, log_bbs, normalized,
                                      log_bbs < 512U ? log_bbs : 512U,
                                      uuid, &previous, previous_header,
                                      &seek_error) == 0) {
        uint64_t distance;
        if (candidate == log_bbs && normalized == 0U)
            distance = log_bbs - previous.block;
        else
            distance = xfs_log_distance(previous.block, normalized, log_bbs);
        uint64_t record_bbs = xfs_log_header_bbs(previous_header) +
                              ((uint64_t)previous.length + 511U) / 512U;
        if (distance != record_bbs)
            normalized = previous.block;
    } else {
        /* A valid cycle map with no nearby header is not safe to mutate. */
        xfs_set_error(error, "%s", seek_error != NULL ? seek_error :
                      "XFS internal log head cannot be validated against a preceding record");
        xfs_clear_error(&seek_error);
        return -1;
    }
    xfs_clear_error(&seek_error);
    *head = normalized;
    return 0;
}

int xfs_verify_clean_log(const char *path, const XfsCatalogue *catalogue, char **error) {
    const XfsGeometry *g = &catalogue->geometry;
    if (g->logstart == 0 || g->logblocks == 0) {
        xfs_set_error(error, "XFS raw writing requires a valid internal log");
        return -1;
    }
    if (g->log_incompat != 0) {
        xfs_set_error(error, "XFS log-incompat features are active; raw mutation is not supported");
        return -1;
    }
    uint64_t start = g->logstart * g->block_size;
    uint64_t size = (uint64_t)g->logblocks * g->block_size;
    uint64_t log_bbs = size / 512U;
    if (log_bbs == 0 || log_bbs > SIZE_MAX / sizeof(uint32_t)) {
        xfs_set_error(error, "XFS internal log geometry is invalid");
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        xfs_set_error(error, "cannot open XFS internal log: %s", strerror(errno));
        return -1;
    }

    uint64_t head = 0;
    bool all_zero = false;
    int result = xfs_log_find_head(fd, start, log_bbs, g->uuid, &head, &all_zero, error);
    if (result != 0)
        goto done;
    if (all_zero) {
        /* A wholly zero log contains nothing requiring replay. */
        result = 0;
        goto done;
    }

    XfsLogRecord record;
    memset(&record, 0, sizeof(record));
    uint8_t header[512];
    if (xfs_log_rseek_previous_header(fd, start, log_bbs, head, log_bbs,
                                      g->uuid, &record, header, error) != 0) {
        result = -1;
        goto done;
    }
    if (record.end_block != head) {
        xfs_set_error(error,
                      "XFS internal log is not clean: head=%" PRIu64
                      " previous_header=%" PRIu64 " record_end=%" PRIu64
                      " cycle=%u len=%u h_size=%u numops=%u",
                      head, record.block, record.end_block, record.cycle,
                      record.length, record.header_size, record.numops);
        result = -1;
        goto done;
    }
    if (record.numops != 1U) {
        xfs_set_error(error,
                      "XFS internal log is dirty: head record has %u operations, not one clean-unmount operation",
                      record.numops);
        result = -1;
        goto done;
    }

    uint64_t hblks = xfs_log_header_bbs(header);
    uint64_t data_bb = xfs_log_wrap(record.block + hblks, log_bbs);
    uint8_t raw[512];
    ssize_t got = ld_pread_full(fd, raw, sizeof(raw), start + data_bb * 512U);
    if (got < 12) {
        xfs_set_error(error, "XFS internal log unmount record is truncated");
        result = -1;
        goto done;
    }
    uint32_t oh_len = xfs_be32(raw + 4);
    uint8_t oh_client = raw[8];
    uint8_t oh_flags = raw[9];
    /* This is deliberately the same cleanliness criterion used by
     * xlog_check_unmount_rec(): once the previous record is proven complete,
     * ends exactly at the physical head, and contains one operation, the
     * unmount transaction flag is authoritative.  Client/length are reported
     * for diagnostics but are not extra invented cleanliness requirements. */
    if ((oh_flags & XFS_LOG_UNMOUNT_TRANS) == 0) {
        xfs_set_error(error,
                      "XFS internal log is dirty: head=%" PRIu64
                      " previous_header=%" PRIu64 " data=%" PRIu64
                      " cycle=%u len=%u h_size=%u numops=%u client=0x%02x flags=0x%02x oh_len=%u",
                      head, record.block, data_bb, record.cycle, record.length,
                      record.header_size, record.numops, (unsigned)oh_client,
                      (unsigned)oh_flags, oh_len);
        result = -1;
        goto done;
    }
    result = 0;

done:
    close(fd);
    return result;
}
