// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"

#include "ld_runtime.h"

#include "infiltratr/posix_io.h"

#include <com_err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <unistd.h>

#define EXT3_INCOMPAT_MASK UINT32_C(0x001f)
#define EXT3_RO_COMPAT_MASK UINT32_C(0x0003)

void ext_set_error(char **error, const char *format, ...) {
    if (error == NULL || *error != NULL) return;
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int required = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (required < 0) {
        va_end(arguments);
        *error = ld_xstrdup("EXT engine error");
        return;
    }
    *error = ld_xmalloc((size_t)required + 1U);
    (void)vsnprintf(*error, (size_t)required + 1U, format, arguments);
    va_end(arguments);
}

static int compare_range(const void *left, const void *right) {
    const ExtRange *a = left;
    const ExtRange *b = right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

void ext_range_push(ExtRangeVec *vec, uint64_t start, uint64_t end) {
    if (end <= start) return;
    if (vec->count == vec->capacity) {
        size_t next = vec->capacity == 0 ? 32U : vec->capacity * 2U;
        vec->items = ld_xrealloc(vec->items, next * sizeof(*vec->items));
        vec->capacity = next;
    }
    vec->items[vec->count++] = (ExtRange){.start = start, .end = end};
}

void ext_range_free(ExtRangeVec *vec) {
    if (vec == NULL) return;
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

void ext_range_sort_merge(ExtRangeVec *vec) {
    if (vec == NULL || vec->count < 2) return;
    qsort(vec->items, vec->count, sizeof(*vec->items), compare_range);
    size_t write_index = 0;
    for (size_t index = 1; index < vec->count; ++index) {
        ExtRange *current = &vec->items[write_index];
        ExtRange next = vec->items[index];
        if (next.start <= current->end) {
            if (next.end > current->end) current->end = next.end;
        } else {
            vec->items[++write_index] = next;
        }
    }
    vec->count = write_index + 1U;
}

static int physical_size(const char *path, uint64_t *size, char **error) {
    struct stat status;
    if (stat(path, &status) != 0) {
        ext_set_error(error, "cannot stat EXT target %s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISREG(status.st_mode)) {
        *size = (uint64_t)status.st_size;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        ext_set_error(error, "EXT target must be a block device or regular filesystem image");
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ext_set_error(error, "cannot open EXT target %s: %s", path, strerror(errno));
        return -1;
    }
    uint64_t found = 0;
    int result = ioctl(fd, BLKGETSIZE64, &found);
    int saved = errno;
    (void)close(fd);
    if (result != 0) {
        ext_set_error(error, "cannot read EXT target size: %s", strerror(saved));
        return -1;
    }
    *size = found;
    return 0;
}

int ext_read_geometry(const char *path, ExtGeometry *geometry, char **error) {
    if (geometry == NULL) return -1;
    memset(geometry, 0, sizeof(*geometry));
    uint64_t physical_bytes = 0;
    if (physical_size(path, &physical_bytes, error) != 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ext_set_error(error, "cannot open EXT target %s: %s", path, strerror(errno));
        return -1;
    }
    uint8_t sb[1024];
    int got = infiltratr_pread_full(fd, sb, sizeof(sb), 1024U);
    int saved = errno;
    (void)close(fd);
    if (got != 0) {
        ext_set_error(error, "cannot read EXT superblock from %s: %s", path,
                      strerror(saved));
        return -1;
    }
    if (ld_read_le16(sb + 56) != EXT2_SUPER_MAGIC) {
        ext_set_error(error, "not an EXT2/EXT3/EXT4 filesystem");
        return -1;
    }
    uint32_t log_block = ld_read_le32(sb + 24);
    if (log_block > 6U) {
        ext_set_error(error, "unsupported EXT block size exponent %u", log_block);
        return -1;
    }
    uint32_t block_size = 1024U << log_block;
    uint32_t incompat = ld_read_le32(sb + 96);
    uint32_t compat = ld_read_le32(sb + 92);
    uint32_t ro_compat = ld_read_le32(sb + 100);
    uint64_t blocks = ld_read_le32(sb + 4);
    uint64_t free_blocks = ld_read_le32(sb + 12);
    if ((incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0U) {
        blocks |= (uint64_t)ld_read_le32(sb + 0x150) << 32;
        free_blocks |= (uint64_t)ld_read_le32(sb + 0x158) << 32;
    }
    if (blocks == 0 || free_blocks > blocks || blocks > physical_bytes / block_size) {
        ext_set_error(error, "EXT filesystem geometry exceeds the target device");
        return -1;
    }
    uint64_t physical_blocks = physical_bytes / block_size;
    geometry->block_size = block_size;
    geometry->total_blocks = blocks;
    geometry->free_blocks = free_blocks;
    geometry->physical_blocks = physical_blocks;
    geometry->physical_bytes = physical_bytes;
    geometry->first_data_block = ld_read_le32(sb + 20);
    geometry->ro_compat = ro_compat;
    geometry->incompat = incompat;
    geometry->compat = compat;
    memcpy(geometry->uuid, sb + 104, sizeof(geometry->uuid));
    if ((incompat & ~EXT3_INCOMPAT_MASK) != 0U ||
        (ro_compat & ~EXT3_RO_COMPAT_MASK) != 0U) {
        memcpy(geometry->filesystem, "ext4", 5U);
    } else if ((compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) != 0U) {
        memcpy(geometry->filesystem, "ext3", 5U);
    } else {
        memcpy(geometry->filesystem, "ext2", 5U);
    }
    return 0;
}

int ext_open_fs(const char *path, bool writable, ext2_filsys *fs, char **error) {
    int flags = EXT2_FLAG_64BITS | EXT2_FLAG_SOFTSUPP_FEATURES;
    if (writable) flags |= EXT2_FLAG_RW | EXT2_FLAG_EXCLUSIVE;
    errcode_t code = ext2fs_open(path, flags, 0, 0, unix_io_manager, fs);
    if (code != 0) {
        ext_set_error(error, "opening EXT filesystem %s: %s", path, error_message(code));
        return -1;
    }
    code = ext2fs_read_bitmaps(*fs);
    if (code != 0) {
        ext_set_error(error, "reading EXT allocation bitmaps: %s", error_message(code));
        (void)ext2fs_close(*fs);
        *fs = NULL;
        return -1;
    }
    return 0;
}

int ext_validate_metadata(ext2_filsys fs, bool verify_inodes, char **error) {
    if (!ext2fs_verify_csum_type(fs, fs->super) ||
        !ext2fs_superblock_csum_verify(fs, fs->super)) {
        ext_set_error(error, "EXT superblock checksum verification failed");
        return -1;
    }
    errcode_t code = ext2fs_check_desc(fs);
    if (code != 0) {
        ext_set_error(error, "EXT group-descriptor validation failed: %s", error_message(code));
        return -1;
    }
    char *bitmap = ld_xmalloc(fs->blocksize);
    for (dgrp_t group = 0; group < fs->group_desc_count; ++group) {
        if (!ext2fs_group_desc_csum_verify(fs, group)) {
            ext_set_error(error, "EXT group %u descriptor checksum failed", (unsigned)group);
            free(bitmap);
            return -1;
        }
        if (!ext2fs_bg_flags_test(fs, group, EXT2_BG_BLOCK_UNINIT)) {
            blk64_t block_bitmap = ext2fs_block_bitmap_loc(fs, group);
            errcode_t bitmap_code = io_channel_read_blk64(fs->io, block_bitmap, 1, bitmap);
            int block_bitmap_bytes = (int)((EXT2_CLUSTERS_PER_GROUP(fs->super) + 7U) / 8U);
            if (bitmap_code != 0 || !ext2fs_block_bitmap_csum_verify(
                    fs, group, bitmap, block_bitmap_bytes)) {
                ext_set_error(error, "EXT group %u block-bitmap checksum failed", (unsigned)group);
                free(bitmap);
                return -1;
            }
        }
        if (!ext2fs_bg_flags_test(fs, group, EXT2_BG_INODE_UNINIT)) {
            blk64_t inode_bitmap = ext2fs_inode_bitmap_loc(fs, group);
            errcode_t bitmap_code = io_channel_read_blk64(fs->io, inode_bitmap, 1, bitmap);
            int inode_bitmap_bytes = (int)((EXT2_INODES_PER_GROUP(fs->super) + 7U) / 8U);
            if (bitmap_code != 0 || !ext2fs_inode_bitmap_csum_verify(
                    fs, group, bitmap, inode_bitmap_bytes)) {
                ext_set_error(error, "EXT group %u inode-bitmap checksum failed", (unsigned)group);
                free(bitmap);
                return -1;
            }
        }
    }
    free(bitmap);
    if (!verify_inodes) return 0;
    ext2_inode_scan scan = NULL;
    code = ext2fs_open_inode_scan(fs, 0, &scan);
    if (code != 0) {
        ext_set_error(error, "opening EXT inode checksum scan: %s", error_message(code));
        return -1;
    }
    int result = 0;
    size_t inode_bytes = fs->super->s_inode_size;
    unsigned char *inode_buffer = ld_xmalloc(inode_bytes);
    ext2_ino_t ino = 0;
    while ((code = ext2fs_get_next_inode_full(scan, &ino,
             (struct ext2_inode *)inode_buffer, (int)inode_bytes)) == 0 && ino != 0) {
        struct ext2_inode_large *inode = (struct ext2_inode_large *)inode_buffer;
        if (inode->i_mode == 0 || inode->i_links_count == 0) continue;
        if (!ext2fs_inode_csum_verify(fs, ino, inode)) {
            ext_set_error(error, "EXT inode %u checksum failed", (unsigned)ino);
            result = -1;
            break;
        }
    }
    free(inode_buffer);
    if (code != 0 && result == 0) {
        ext_set_error(error, "reading EXT inode during checksum scan: %s", error_message(code));
        result = -1;
    }
    ext2fs_close_inode_scan(scan);
    return result;
}

int ext_validate_writer_support(ext2_filsys fs, const ExtGeometry *geometry,
                                char **error) {
    if ((geometry->ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) != 0U) {
        ext_set_error(error, "EXT bigalloc filesystems are not yet supported by the native raw writer");
        return -1;
    }
    if ((geometry->incompat & EXT2_FEATURE_INCOMPAT_META_BG) != 0U) {
        ext_set_error(error, "EXT meta_bg filesystems are not yet supported by the native raw writer");
        return -1;
    }
    if ((fs->super->s_state & EXT2_VALID_FS) == 0U) {
        ext_set_error(error, "EXT filesystem is not marked clean; refusing raw mutation");
        return -1;
    }
    if (geometry->physical_blocks > geometry->total_blocks + 255U) {
        ext_set_error(error,
            "EXT active filesystem does not span the target; Linux Defragger no longer shells out to resize tools and will not resize it implicitly");
        return -1;
    }
    return ext_validate_metadata(fs, true, error);
}
