// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfs_native.h"
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

#define SFS_ROOT_ID 0x53465300U
#define SFS_BITMAP_ID 0x42544d50U
#define SFS_TRFA_ID 0x54524641U
#define SFS_STRUCTURE_VERSION 3U
#define SFS_MIN_BLOCK 512U
#define SFS_MAX_BLOCK 65536U
#define SFS_HEADER_BYTES 12U

static void set_error(char *error, size_t size, const char *text) {
    if (error != NULL && size != 0U) (void)snprintf(error, size, "%s", text);
}
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
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
static int scan_bitmap(int fd, const Root *root, SfsAnalysis *analysis,
                       SfsMapCell *cells, uint64_t cell_count, uint64_t total_units,
                       char *error, size_t error_size) {
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
            cells[i].free_count=cells[i].used_count=cells[i].outside_count=0U;
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
    if (validate_transaction(fd,&root,&analysis->transaction_pending,error,error_size)!=0 ||
        scan_bitmap(fd,&root,analysis,cells,cell_count,total_units,error,error_size)!=0) { close(fd); return -1; }
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
