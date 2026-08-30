// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfs_native.h"
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

#define SFS_ROOT_ID 0x53465300U
#define SFS_BITMAP_ID 0x42544d50U
#define SFS_TRFA_ID 0x54524641U
#define SFS_BNODE_ID 0x424e4443U
#define SFS_OBJECT_ID 0x4f424a43U
#define SFS_STRUCTURE_VERSION 3U
#define SFS_BNODE_HEADER_BYTES 16U
#define SFS_INTERNAL_NODE_BYTES 8U
#define SFS_EXTENT_NODE_BYTES 14U
#define SFS_OBJECT_CONTAINER_BYTES 24U
#define SFS_OBJECT_FIXED_BYTES 25U
#define SFS_OTYPE_HARDLINK 32U
#define SFS_OTYPE_LINK 64U
#define SFS_OTYPE_DIR 128U
#define SFS_MIN_BLOCK 512U
#define SFS_MAX_BLOCK 65536U
#define SFS_HEADER_BYTES 12U

static void set_error(char *error, size_t size, const char *text) {
    if (error != NULL && size != 0U) (void)snprintf(error, size, "%s", text);
}
static uint16_t be16(const uint8_t *p) {
    return infiltratr_load_be16(p);
}
static uint32_t be32(const uint8_t *p) {
    return infiltratr_load_be32(p);
}
static int size_bytes(int fd, const char *path, uint64_t *bytes) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0) { errno = EINVAL; return -1; }
        *bytes = (uint64_t)st.st_size;
        return 0;
    }
    if (!S_ISBLK(st.st_mode)) { (void)path; errno = EINVAL; return -1; }
    return ioctl(fd, BLKGETSIZE64, bytes) == 0 ? 0 : -1;
}
static bool valid_block_size(uint32_t value) {
    return value >= SFS_MIN_BLOCK && value <= SFS_MAX_BLOCK &&
           (value & (value - 1U)) == 0U && value % 512U == 0U;
}
static bool checksum_ok(const uint8_t *block, uint32_t block_size) {
    if (block_size % 4U != 0U) return false;
    uint32_t sum = 1U;
    for (uint32_t off = 0U; off < block_size; off += 4U)
        sum += be32(block + off);
    return sum == 0U;
}
typedef struct {
    uint32_t block_size, total_blocks, bitmap_base, root_object_container;
    uint32_t admin_space_container, extent_bnode_root, object_node_root;
    uint16_t version, sequence;
    uint8_t bits;
    uint32_t own_block;
} Root;

typedef struct {
    uint32_t key;
    uint32_t next;
    uint32_t prev;
    uint16_t blocks;
} SfsExtent;

typedef struct {
    SfsExtent *items;
    size_t count;
    size_t capacity;
} SfsExtentVec;

typedef struct {
    size_t *items;
    size_t count;
    size_t capacity;
} SfsSizeVec;

typedef struct {
    uint32_t *items;
    size_t count;
    size_t capacity;
} SfsU32Vec;

static int reserve_array(void **items, size_t *capacity, size_t need,
                         size_t item_size, char *error, size_t error_size) {
    if (need <= *capacity) return 0;
    size_t next = *capacity == 0U ? 16U : *capacity;
    while (next < need) {
        if (next > SIZE_MAX / 2U) {
            set_error(error, error_size, "SFS catalogue is too large");
            return -1;
        }
        next *= 2U;
    }
    if (next > SIZE_MAX / item_size) {
        set_error(error, error_size, "SFS catalogue allocation overflows");
        return -1;
    }
    void *grown = realloc(*items, next * item_size);
    if (grown == NULL) {
        set_error(error, error_size, "out of memory building SFS catalogue");
        return -1;
    }
    *items = grown;
    *capacity = next;
    return 0;
}

static int extent_push(SfsExtentVec *vec, SfsExtent value,
                       char *error, size_t error_size) {
    if (reserve_array((void **)&vec->items, &vec->capacity, vec->count + 1U,
                      sizeof(*vec->items), error, error_size) != 0) return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static int size_push(SfsSizeVec *vec, size_t value,
                     char *error, size_t error_size) {
    if (reserve_array((void **)&vec->items, &vec->capacity, vec->count + 1U,
                      sizeof(*vec->items), error, error_size) != 0) return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static int u32_push(SfsU32Vec *vec, uint32_t value,
                    char *error, size_t error_size) {
    if (reserve_array((void **)&vec->items, &vec->capacity, vec->count + 1U,
                      sizeof(*vec->items), error, error_size) != 0) return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static bool parse_root(const uint8_t *block, uint32_t bytes, uint32_t expected_own,
                       Root *root) {
    if (bytes < 116U || be32(block) != SFS_ROOT_ID || be32(block + 8U) != expected_own ||
        !checksum_ok(block, bytes)) return false;
    Root r = {0};
    r.own_block = expected_own;
    r.version = be16(block + 12U);
    r.sequence = be16(block + 14U);
    r.bits = block[20U];
    r.total_blocks = be32(block + 48U);
    r.block_size = be32(block + 52U);
    r.bitmap_base = be32(block + 96U);
    r.admin_space_container = be32(block + 100U);
    r.root_object_container = be32(block + 104U);
    r.extent_bnode_root = be32(block + 108U);
    r.object_node_root = be32(block + 112U);
    if (r.version != SFS_STRUCTURE_VERSION || !valid_block_size(r.block_size) ||
        r.block_size != bytes || r.total_blocks < 4U || r.bitmap_base == 0U ||
        r.bitmap_base >= r.total_blocks || r.root_object_container >= r.total_blocks ||
        r.admin_space_container >= r.total_blocks || r.extent_bnode_root >= r.total_blocks ||
        r.object_node_root >= r.total_blocks) return false;
    *root = r;
    return true;
}
static int load_root_at(int fd, uint32_t block_size, uint32_t block_no, Root *root) {
    uint8_t *buf = malloc(block_size);
    if (buf == NULL) return -1;
    const ssize_t rr = ld_pread_full(fd, buf, block_size, (uint64_t)block_no * block_size);
    const bool ok = rr == (ssize_t)block_size && parse_root(buf, block_size, block_no, root);
    free(buf);
    return ok ? 0 : 1;
}
static int discover_roots(int fd, uint64_t physical, Root *selected,
                          bool *primary_valid, bool *backup_valid,
                          char *error, size_t error_size) {
    uint8_t probe[512];
    if (ld_pread_full(fd, probe, sizeof(probe), 0U) != (ssize_t)sizeof(probe)) {
        set_error(error, error_size, "SFS volume is shorter than its root block"); return -1;
    }
    Root primary = {0}, backup = {0};
    bool pvalid = false, bvalid = false;
    uint32_t bs = be32(probe + 52U);
    uint32_t total = be32(probe + 48U);
    if (be32(probe) == SFS_ROOT_ID && valid_block_size(bs) && total >= 4U &&
        (uint64_t)total * bs <= physical) {
        pvalid = load_root_at(fd, bs, 0U, &primary) == 0;
        bvalid = load_root_at(fd, bs, total - 1U, &backup) == 0;
    }
    if (!pvalid && !bvalid) {
        for (uint32_t candidate = SFS_MIN_BLOCK; candidate <= SFS_MAX_BLOCK; candidate <<= 1U) {
            if (physical < candidate * 4ULL || physical % candidate != 0U) continue;
            const uint64_t blocks64 = physical / candidate;
            if (blocks64 > UINT32_MAX) continue;
            Root recovered = {0};
            if (load_root_at(fd, candidate, (uint32_t)blocks64 - 1U, &recovered) == 0 &&
                recovered.total_blocks == blocks64) {
                backup = recovered; bvalid = true; bs = candidate; total = (uint32_t)blocks64; break;
            }
        }
    }
    if (!pvalid && !bvalid) {
        set_error(error, error_size, "no valid SFS root block found"); return -1;
    }
    if (pvalid && (uint64_t)primary.total_blocks * primary.block_size > physical) pvalid = false;
    if (bvalid && (uint64_t)backup.total_blocks * backup.block_size > physical) bvalid = false;
    if (!pvalid && !bvalid) { set_error(error, error_size, "SFS root geometry exceeds target"); return -1; }
    Root chosen = pvalid ? primary : backup;
    if (bvalid && (!pvalid || backup.sequence > primary.sequence)) chosen = backup;
    if (pvalid && bvalid && (primary.block_size != backup.block_size ||
        primary.total_blocks != backup.total_blocks)) {
        set_error(error, error_size, "SFS redundant roots disagree on filesystem geometry"); return -1;
    }
    *selected = chosen; *primary_valid = pvalid; *backup_valid = bvalid;
    return 0;
}
static int validate_transaction(int fd, const Root *root, bool *pending,
                                char *error, size_t error_size) {
    *pending = false;
    if (root->root_object_container > root->total_blocks - 3U) return 0;
    const uint32_t block_no = root->root_object_container + 2U;
    uint8_t *buf = malloc(root->block_size);
    if (buf == NULL) { set_error(error, error_size, "out of memory reading SFS transaction state"); return -1; }
    const ssize_t rr = ld_pread_full(fd, buf, root->block_size, (uint64_t)block_no * root->block_size);
    if (rr != (ssize_t)root->block_size) { free(buf); set_error(error, error_size, "cannot read SFS transaction state"); return -1; }
    if (be32(buf) == SFS_TRFA_ID) {
        if (be32(buf + 8U) != block_no || !checksum_ok(buf, root->block_size)) {
            free(buf); set_error(error, error_size, "corrupt SFS unfinished-transaction marker"); return -1;
        }
        *pending = true;
    }
    free(buf); return 0;
}

static int read_metadata_block(int fd, const Root *root, uint32_t block_no,
                               uint32_t expected_id, uint8_t *buffer,
                               char *error, size_t error_size) {
    if (block_no >= root->total_blocks ||
        ld_pread_full(fd, buffer, root->block_size,
                      (uint64_t)block_no * root->block_size) !=
            (ssize_t)root->block_size) {
        set_error(error, error_size, "cannot read SFS metadata block");
        return -1;
    }
    if (be32(buffer) != expected_id || be32(buffer + 8U) != block_no ||
        !checksum_ok(buffer, root->block_size)) {
        set_error(error, error_size, "invalid SFS metadata block");
        return -1;
    }
    return 0;
}

static int scan_extent_container(int fd, const Root *root, uint32_t block_no,
                                 uint8_t *visited, unsigned depth,
                                 SfsExtentVec *extents,
                                 uint32_t *first_key, bool *has_key,
                                 char *error, size_t error_size) {
    if (depth > 64U || block_no >= root->total_blocks || visited[block_no] != 0U) {
        set_error(error, error_size, "SFS extent B-tree contains a cycle or invalid depth");
        return -1;
    }
    visited[block_no] = 1U;
    uint8_t *buffer = malloc(root->block_size);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory reading SFS extent B-tree");
        return -1;
    }
    if (read_metadata_block(fd, root, block_no, SFS_BNODE_ID, buffer,
                            error, error_size) != 0) {
        free(buffer);
        return -1;
    }

    const uint16_t count = be16(buffer + 12U);
    const bool leaf = buffer[14U] != 0U;
    const uint8_t node_size = buffer[15U];
    const uint8_t expected_size = leaf ? SFS_EXTENT_NODE_BYTES : SFS_INTERNAL_NODE_BYTES;
    if (node_size != expected_size ||
        (uint64_t)count * node_size > root->block_size - SFS_BNODE_HEADER_BYTES) {
        free(buffer);
        set_error(error, error_size, "invalid SFS extent B-tree node geometry");
        return -1;
    }

    *has_key = false;
    uint32_t previous_key = 0U;
    for (uint16_t index = 0U; index < count; ++index) {
        const uint8_t *node = buffer + SFS_BNODE_HEADER_BYTES +
                              (size_t)index * node_size;
        const uint32_t key = be32(node);
        if (index != 0U && key <= previous_key) {
            free(buffer);
            set_error(error, error_size, "SFS extent B-tree keys are not strictly ordered");
            return -1;
        }
        previous_key = key;
        if (leaf) {
            SfsExtent extent = {
                .key = key,
                .next = be32(node + 4U),
                .prev = be32(node + 8U),
                .blocks = be16(node + 12U),
            };
            if (extent.key == 0U || extent.blocks == 0U ||
                extent.key >= root->total_blocks ||
                (uint64_t)extent.key + extent.blocks > root->total_blocks ||
                extent.next >= root->total_blocks ||
                extent.prev >= root->total_blocks) {
                free(buffer);
                set_error(error, error_size, "SFS extent B-tree contains an invalid extent");
                return -1;
            }
            if (extent_push(extents, extent, error, error_size) != 0) {
                free(buffer);
                return -1;
            }
            if (!*has_key) {
                *first_key = key;
                *has_key = true;
            }
        } else {
            const uint32_t child = be32(node + 4U);
            uint32_t child_first = 0U;
            bool child_has_key = false;
            if (child == 0U || child >= root->total_blocks ||
                scan_extent_container(fd, root, child, visited, depth + 1U,
                                      extents, &child_first, &child_has_key,
                                      error, error_size) != 0) {
                free(buffer);
                if (error != NULL && error_size != 0U && error[0] == '\0')
                    set_error(error, error_size, "SFS extent B-tree has an invalid child");
                return -1;
            }
            if (!child_has_key || (index == 0U ? key > child_first : key != child_first)) {
                free(buffer);
                set_error(error, error_size, "SFS extent B-tree separator key is inconsistent");
                return -1;
            }
            if (!*has_key) {
                *first_key = child_first;
                *has_key = true;
            }
        }
    }
    free(buffer);
    return 0;
}

static ssize_t extent_find(const SfsExtentVec *extents, uint32_t key) {
    size_t lo = 0U;
    size_t hi = extents->count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        if (extents->items[mid].key < key) lo = mid + 1U;
        else hi = mid;
    }
    if (lo < extents->count && extents->items[lo].key == key)
        return (ssize_t)lo;
    return -1;
}

static void add_fragmented_cells(SfsMapCell *cells, uint64_t cell_count,
                                 uint32_t start, uint32_t blocks) {
    if (cells == NULL || cell_count == 0U || blocks == 0U) return;
    const uint64_t end = (uint64_t)start + blocks;
    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t cell_start = cells[index].start;
        const uint64_t cell_end = cells[index].end + 1U;
        const uint64_t overlap_start = cell_start > start ? cell_start : start;
        const uint64_t overlap_end = cell_end < end ? cell_end : end;
        if (overlap_end > overlap_start)
            cells[index].fragmented_count += overlap_end - overlap_start;
    }
}

static int evaluate_file(const Root *root, const SfsExtentVec *extents,
                         uint8_t *extent_owned, const uint8_t *free_map,
                         uint32_t first, uint32_t size,
                         SfsAnalysis *analysis, SfsMapCell *cells,
                         uint64_t cell_count, char *error, size_t error_size) {
    const uint64_t expected =
        ((uint64_t)size + root->block_size - 1U) / root->block_size;
    analysis->regular_files++;
    if (expected == 0U) {
        if (first != 0U) {
            set_error(error, error_size, "empty SFS file unexpectedly references data");
            return -1;
        }
        return 0;
    }
    if (first == 0U) {
        set_error(error, error_size, "non-empty SFS file has no first extent");
        return -1;
    }

    SfsSizeVec chain = {0};
    uint32_t key = first;
    uint32_t previous_key = 0U;
    uint16_t previous_blocks = 0U;
    uint64_t blocks = 0U;
    bool fragmented = false;
    while (key != 0U) {
        const ssize_t found = extent_find(extents, key);
        if (found < 0 || extent_owned[(size_t)found] != 0U) {
            free(chain.items);
            set_error(error, error_size, "SFS file extent chain is missing, cyclic or multiply referenced");
            return -1;
        }
        const SfsExtent *extent = &extents->items[(size_t)found];
        if (extent->prev != previous_key) {
            free(chain.items);
            set_error(error, error_size, "SFS file extent backward link is inconsistent");
            return -1;
        }
        if (previous_key != 0U &&
            extent->key != previous_key + (uint32_t)previous_blocks)
            fragmented = true;
        extent_owned[(size_t)found] = 1U;
        if (size_push(&chain, (size_t)found, error, error_size) != 0) {
            free(chain.items);
            return -1;
        }
        blocks += extent->blocks;
        if (blocks > expected) {
            free(chain.items);
            set_error(error, error_size, "SFS file extent chain exceeds file size");
            return -1;
        }
        previous_key = extent->key;
        previous_blocks = extent->blocks;
        key = extent->next;
    }
    if (blocks != expected) {
        free(chain.items);
        set_error(error, error_size, "SFS file extent chain does not cover file size");
        return -1;
    }

    analysis->data_blocks += blocks;
    if (fragmented) {
        analysis->fragmented_files++;
        for (size_t i = 0U; i < chain.count; ++i) {
            const SfsExtent *extent = &extents->items[chain.items[i]];
            add_fragmented_cells(cells, cell_count, extent->key, extent->blocks);
        }
    }

    const uint32_t end = previous_key + (uint32_t)previous_blocks;
    const uint64_t reserve = (blocks * 10U + 99U) / 100U;
    if (fragmented || (uint64_t)end + reserve > root->total_blocks) {
        analysis->growth_10_satisfied = false;
    } else {
        for (uint64_t i = 0U; i < reserve; ++i) {
            if (free_map[end + i] == 0U) {
                analysis->growth_10_satisfied = false;
                break;
            }
        }
    }
    free(chain.items);
    return 0;
}

static int scan_object_catalogue(int fd, const Root *root,
                                 const SfsExtentVec *extents,
                                 uint8_t *extent_owned, const uint8_t *free_map,
                                 SfsAnalysis *analysis, SfsMapCell *cells,
                                 uint64_t cell_count,
                                 char *error, size_t error_size) {
    uint8_t *visited = calloc(root->total_blocks, 1U);
    if (visited == NULL) {
        set_error(error, error_size, "out of memory tracking SFS object containers");
        return -1;
    }
    SfsU32Vec pending = {0};
    if (u32_push(&pending, root->root_object_container, error, error_size) != 0) {
        free(visited);
        return -1;
    }

    int rc = 0;
    for (size_t queue = 0U; queue < pending.count && rc == 0; ++queue) {
        uint32_t block_no = pending.items[queue];
        uint32_t expected_previous = 0U;
        while (block_no != 0U) {
            if (block_no >= root->total_blocks || visited[block_no] != 0U) {
                set_error(error, error_size, "SFS object-container graph contains a cycle");
                rc = -1;
                break;
            }
            visited[block_no] = 1U;
            uint8_t *buffer = malloc(root->block_size);
            if (buffer == NULL) {
                set_error(error, error_size, "out of memory reading SFS object container");
                rc = -1;
                break;
            }
            if (read_metadata_block(fd, root, block_no, SFS_OBJECT_ID, buffer,
                                    error, error_size) != 0) {
                free(buffer);
                rc = -1;
                break;
            }
            const uint32_t next_block = be32(buffer + 16U);
            const uint32_t previous_block = be32(buffer + 20U);
            if (previous_block != expected_previous ||
                next_block >= root->total_blocks) {
                free(buffer);
                set_error(error, error_size, "SFS object-container links are inconsistent");
                rc = -1;
                break;
            }

            size_t offset = SFS_OBJECT_CONTAINER_BYTES;
            while (offset + SFS_OBJECT_FIXED_BYTES + 2U <= root->block_size) {
                const size_t name_offset = offset + SFS_OBJECT_FIXED_BYTES;
                if (buffer[name_offset] == 0U) break;
                const uint8_t *limit = buffer + root->block_size;
                const uint8_t *name_end =
                    memchr(buffer + name_offset, 0, (size_t)(limit - (buffer + name_offset)));
                if (name_end == NULL) {
                    free(buffer);
                    set_error(error, error_size, "unterminated SFS object name");
                    rc = -1;
                    break;
                }
                const uint8_t *comment = name_end + 1U;
                const uint8_t *comment_end =
                    memchr(comment, 0, (size_t)(limit - comment));
                if (comment_end == NULL) {
                    free(buffer);
                    set_error(error, error_size, "unterminated SFS object comment");
                    rc = -1;
                    break;
                }
                const uint32_t object_node = be32(buffer + offset + 4U);
                const uint32_t data = be32(buffer + offset + 12U);
                const uint32_t auxiliary = be32(buffer + offset + 16U);
                const uint8_t bits = buffer[offset + 24U];
                if (object_node == 0U) {
                    free(buffer);
                    set_error(error, error_size, "SFS object has an invalid node number");
                    rc = -1;
                    break;
                }

                if ((bits & SFS_OTYPE_DIR) != 0U) {
                    analysis->directories++;
                    if (auxiliary != 0U &&
                        u32_push(&pending, auxiliary, error, error_size) != 0) {
                        free(buffer);
                        rc = -1;
                        break;
                    }
                } else if ((bits & (SFS_OTYPE_LINK | SFS_OTYPE_HARDLINK)) == 0U) {
                    if (evaluate_file(root, extents, extent_owned, free_map,
                                      data, auxiliary, analysis, cells, cell_count,
                                      error, error_size) != 0) {
                        free(buffer);
                        rc = -1;
                        break;
                    }
                }

                size_t next_offset = (size_t)(comment_end - buffer) + 1U;
                if ((next_offset & 1U) != 0U) ++next_offset;
                if (next_offset <= offset || next_offset > root->block_size) {
                    free(buffer);
                    set_error(error, error_size, "invalid SFS object-container packing");
                    rc = -1;
                    break;
                }
                offset = next_offset;
            }
            if (rc != 0) break;
            free(buffer);
            expected_previous = block_no;
            block_no = next_block;
        }
    }

    free(pending.items);
    free(visited);
    return rc;
}

static int scan_catalogue(int fd, const Root *root, const uint8_t *free_map,
                          SfsAnalysis *analysis, SfsMapCell *cells,
                          uint64_t cell_count, char *error, size_t error_size) {
    uint8_t *tree_visited = calloc(root->total_blocks, 1U);
    if (tree_visited == NULL) {
        set_error(error, error_size, "out of memory tracking SFS extent B-tree");
        return -1;
    }
    SfsExtentVec extents = {0};
    uint32_t first_key = 0U;
    bool has_key = false;
    int rc = scan_extent_container(fd, root, root->extent_bnode_root,
                                   tree_visited, 0U, &extents,
                                   &first_key, &has_key, error, error_size);
    free(tree_visited);
    if (rc != 0) {
        free(extents.items);
        return -1;
    }

    for (size_t index = 0U; index < extents.count; ++index) {
        const SfsExtent *extent = &extents.items[index];
        if (index != 0U) {
            const SfsExtent *previous = &extents.items[index - 1U];
            if ((uint64_t)previous->key + previous->blocks > extent->key) {
                free(extents.items);
                set_error(error, error_size, "SFS data extents overlap");
                return -1;
            }
        }
        for (uint32_t block = 0U; block < extent->blocks; ++block) {
            if (free_map[extent->key + block] != 0U) {
                free(extents.items);
                set_error(error, error_size, "SFS extent B-tree references bitmap-free data");
                return -1;
            }
        }
    }

    uint8_t *owned = calloc(extents.count == 0U ? 1U : extents.count, 1U);
    if (owned == NULL) {
        free(extents.items);
        set_error(error, error_size, "out of memory tracking SFS extent ownership");
        return -1;
    }
    analysis->growth_10_satisfied = true;
    rc = scan_object_catalogue(fd, root, &extents, owned, free_map,
                               analysis, cells, cell_count, error, error_size);
    if (rc == 0) {
        for (size_t index = 0U; index < extents.count; ++index) {
            if (owned[index] == 0U) {
                set_error(error, error_size, "SFS extent B-tree contains unreferenced data");
                rc = -1;
                break;
            }
        }
    }
    if (analysis->regular_files == 0U)
        analysis->growth_10_satisfied = false;
    free(owned);
    free(extents.items);
    (void)first_key;
    (void)has_key;
    return rc;
}

static int scan_bitmap(int fd, const Root *root, SfsAnalysis *analysis,
                       SfsMapCell *cells, uint64_t cell_count, uint64_t total_units,
                       uint8_t *free_map, char *error, size_t error_size) {
    const uint64_t bits_per = (uint64_t)(root->block_size - SFS_HEADER_BYTES) * 8U;
    const uint64_t bitmap_blocks = ((uint64_t)root->total_blocks + bits_per - 1U) / bits_per;
    if ((uint64_t)root->bitmap_base + bitmap_blocks > root->total_blocks) {
        set_error(error, error_size, "SFS bitmap extends beyond filesystem"); return -1;
    }
    analysis->bitmap_blocks = (uint32_t)bitmap_blocks;
    uint8_t *buf = malloc(root->block_size);
    if (buf == NULL) { set_error(error, error_size, "out of memory reading SFS bitmap"); return -1; }
    uint64_t free_blocks = 0U, used_blocks = 0U;
    uint64_t cell_index = 0U;
    for (uint64_t bi = 0U; bi < bitmap_blocks; ++bi) {
        const uint32_t block_no = root->bitmap_base + (uint32_t)bi;
        if (ld_pread_full(fd, buf, root->block_size, (uint64_t)block_no * root->block_size) != (ssize_t)root->block_size) {
            free(buf); set_error(error, error_size, "cannot read SFS bitmap block"); return -1;
        }
        if (be32(buf) != SFS_BITMAP_ID || be32(buf + 8U) != block_no ||
            !checksum_ok(buf, root->block_size)) {
            free(buf); set_error(error, error_size, "invalid SFS bitmap block"); return -1;
        }
        const uint64_t first = bi * bits_per;
        uint64_t count = bits_per;
        if (first + count > root->total_blocks) count = root->total_blocks - first;
        for (uint64_t bit = 0U; bit < count; ++bit) {
            const uint64_t fs_block = first + bit;
            const uint8_t byte = buf[SFS_HEADER_BYTES + (size_t)(bit >> 3U)];
            const bool is_free = (byte & (uint8_t)(0x80U >> (bit & 7U))) != 0U;
            if (free_map != NULL) free_map[fs_block] = is_free ? 1U : 0U;
            if (is_free) free_blocks++; else used_blocks++;
            if (cells != NULL && cell_count != 0U) {
                while (cell_index + 1U < cell_count && fs_block > cells[cell_index].end) cell_index++;
                if (fs_block >= cells[cell_index].start && fs_block <= cells[cell_index].end) {
                    if (is_free) cells[cell_index].free_count++; else cells[cell_index].used_count++;
                }
            }
        }
    }
    free(buf);
    analysis->free_blocks = free_blocks; analysis->used_blocks = used_blocks;
    if (free_blocks + used_blocks != root->total_blocks) {
        set_error(error, error_size, "SFS bitmap accounting does not cover filesystem"); return -1;
    }
    (void)total_units;
    return 0;
}
int sfs_analyse(const char *path, SfsAnalysis *analysis, SfsMapCell *cells,
                uint64_t cell_count, char *error, size_t error_size) {
    if (path == NULL || analysis == NULL) { set_error(error, error_size, "invalid SFS analysis request"); return -1; }
    memset(analysis, 0, sizeof(*analysis));
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (error && error_size) (void)snprintf(error,error_size,"open: %s",strerror(errno)); return -1; }
    uint64_t physical = 0U;
    if (size_bytes(fd, path, &physical) != 0) { if (error&&error_size)(void)snprintf(error,error_size,"size: %s",strerror(errno)); close(fd); return -1; }
    Root root = {0}; bool pv=false,bv=false;
    if (discover_roots(fd, physical, &root, &pv, &bv, error, error_size) != 0) { close(fd); return -1; }
    const uint64_t fs_bytes = (uint64_t)root.total_blocks * root.block_size;
    const uint64_t physical_units = (physical + root.block_size - 1U) / root.block_size;
    const uint64_t total_units = physical_units > root.total_blocks ? physical_units : root.total_blocks;
    if (cells != NULL && cell_count != 0U) {
        for (uint64_t i=0U;i<cell_count;i++) {
            cells[i].start = i * total_units / cell_count;
            uint64_t endex = (i+1U) * total_units / cell_count;
            if (endex <= cells[i].start) endex = cells[i].start + 1U;
            cells[i].end = endex - 1U;
            cells[i].free_count=cells[i].used_count=cells[i].fragmented_count=0U;
            cells[i].outside_count=0U;
            const uint64_t outside_start = cells[i].start > root.total_blocks ? cells[i].start : root.total_blocks;
            if (endex > outside_start) cells[i].outside_count = endex - outside_start;
        }
    }
    analysis->block_size=root.block_size; analysis->total_blocks=root.total_blocks;
    analysis->bitmap_base=root.bitmap_base; analysis->root_object_container=root.root_object_container;
    analysis->admin_space_container=root.admin_space_container; analysis->extent_bnode_root=root.extent_bnode_root;
    analysis->object_node_root=root.object_node_root; analysis->structure_version=root.version;
    analysis->sequence_number=root.sequence; analysis->root_bits=root.bits; analysis->filesystem_bytes=fs_bytes;
    analysis->physical_bytes=physical; analysis->primary_root_valid=pv; analysis->backup_root_valid=bv;
    uint8_t *free_map = calloc(root.total_blocks, 1U);
    if (free_map == NULL) {
        close(fd);
        set_error(error, error_size, "out of memory tracking SFS allocation state");
        return -1;
    }
    if (validate_transaction(fd,&root,&analysis->transaction_pending,error,error_size)!=0 ||
        scan_bitmap(fd,&root,analysis,cells,cell_count,total_units,free_map,error,error_size)!=0 ||
        scan_catalogue(fd,&root,free_map,analysis,cells,cell_count,error,error_size)!=0) {
        free(free_map);
        close(fd);
        return -1;
    }
    free(free_map);
    close(fd); if(error&&error_size)error[0]='\0'; return 0;
}
bool sfs_probe(const char *path)
{
    if (path == NULL) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint64_t physical = 0U;
    Root root = {0};
    bool primary_valid = false, backup_valid = false;
    const bool ok = size_bytes(fd, path, &physical) == 0 &&
        discover_roots(fd, physical, &root, &primary_valid, &backup_valid, NULL, 0U) == 0;
    (void)close(fd);
    return ok;
}
