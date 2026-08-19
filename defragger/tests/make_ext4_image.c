// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <com_err.h>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>

#define IMAGE_BYTES (512LL * 1024LL * 1024LL)
#define FILESYSTEM_BLOCKS (IMAGE_BYTES / BLOCK_SIZE)
#define BLOCK_SIZE 4096U

static void fail_ext(const char *what, errcode_t err) {
    fprintf(stderr, "%s: %s (%ld)\n", what, error_message(err), (long)err);
    exit(1);
}

static void fail_errno(const char *what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}


typedef struct {
    blk64_t cursor;
    blk64_t limit;
} HighAllocContext;

static errcode_t high_alloc_block(ext2_filsys fs, blk64_t goal, blk64_t *ret) {
    (void)goal;
    HighAllocContext *ctx = fs->priv_data;
    while (ctx->cursor < ctx->limit &&
           ext2fs_test_block_bitmap2(fs->block_map, ctx->cursor))
        ctx->cursor++;
    if (ctx->cursor >= ctx->limit) return EXT2_ET_BLOCK_ALLOC_FAIL;
    *ret = ctx->cursor++;
    return 0;
}

typedef struct {
    ext2_ino_t ino;
    void *buffer;
    errcode_t error;
} DirectoryChecksumContext;

static int refresh_directory_block(ext2_filsys fs, blk64_t *blocknr,
                                   e2_blkcnt_t blockcnt, blk64_t ref_blk,
                                   int ref_offset, void *priv) {
    (void)ref_blk;
    (void)ref_offset;
    DirectoryChecksumContext *ctx = priv;
    if (blockcnt < 0 || !*blocknr || ctx->error) return 0;
    ctx->error = io_channel_read_blk64(fs->io, *blocknr, 1, ctx->buffer);
    if (!ctx->error)
        ctx->error = ext2fs_write_dir_block4(fs, *blocknr, ctx->buffer, 0, ctx->ino);
    return ctx->error ? BLOCK_ABORT : 0;
}

static void set_directory_generation(ext2_filsys fs, ext2_ino_t ino,
                                     __u32 generation, int refresh_checksum) {
    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    errcode_t err = ext2fs_read_inode_full(
        fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (err) fail_ext("reading test directory inode", err);
    inode.i_generation = generation;
    err = ext2fs_write_inode_full(
        fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (err) fail_ext("writing test directory generation", err);
    if (!refresh_checksum) return;

    DirectoryChecksumContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ino = ino;
    ctx.buffer = calloc(1, fs->blocksize);
    if (!ctx.buffer) fail_errno("allocating directory checksum buffer");
    err = ext2fs_block_iterate3(
        fs, ino, BLOCK_FLAG_READ_ONLY | BLOCK_FLAG_DATA_ONLY,
        NULL, refresh_directory_block, &ctx);
    free(ctx.buffer);
    if (ctx.error) fail_ext("refreshing test directory checksum", ctx.error);
    if (err) fail_ext("walking test directory blocks", err);
}

static void link_with_expand(ext2_filsys fs, ext2_ino_t parent,
                             const char *name, ext2_ino_t ino, int filetype) {
    errcode_t err = ext2fs_link(fs, parent, name, ino, filetype);
    if (err == EXT2_ET_DIR_NO_SPACE) {
        err = ext2fs_expand_dir(fs, parent);
        if (!err) err = ext2fs_link(fs, parent, name, ino, filetype);
    }
    if (err) fail_ext("linking test object", err);
}

static ext2_ino_t create_file(ext2_filsys fs, ext2_ino_t parent,
                              const char *name, size_t size, unsigned char fill) {
    ext2_ino_t ino = 0;
    errcode_t err = ext2fs_new_inode(
        fs, parent, LINUX_S_IFREG | 0644, fs->inode_map, &ino);
    if (err) fail_ext("allocating test file inode", err);

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = LINUX_S_IFREG | 0644;
    inode.i_links_count = 1;
    inode.i_flags = EXT4_EXTENTS_FL;
    err = ext2fs_write_new_inode(fs, ino, (struct ext2_inode *)&inode);
    if (err) fail_ext("writing test file inode", err);
    ext2fs_inode_alloc_stats2(fs, ino, +1, 0);
    link_with_expand(fs, parent, name, ino, EXT2_FT_REG_FILE);

    ext2_file_t file = NULL;
    err = ext2fs_file_open(fs, ino, EXT2_FILE_WRITE, &file);
    if (err) fail_ext("opening test file", err);
    unsigned char buffer[BLOCK_SIZE];
    memset(buffer, fill, sizeof(buffer));
    size_t remaining = size;
    while (remaining) {
        unsigned request = remaining > sizeof(buffer) ? sizeof(buffer) : (unsigned)remaining;
        unsigned written = 0;
        err = ext2fs_file_write(file, buffer, request, &written);
        if (err || written != request)
            fail_ext("writing test file", err ? err : EIO);
        remaining -= request;
    }
    err = ext2fs_file_close(file);
    if (err) fail_ext("closing test file", err);
    return ino;
}

static ext2_ino_t create_directory(ext2_filsys fs, ext2_ino_t parent,
                                   const char *name) {
    ext2_ino_t ino = 0;
    errcode_t err = ext2fs_new_inode(
        fs, parent, LINUX_S_IFDIR | 0755, fs->inode_map, &ino);
    if (err) fail_ext("allocating test directory inode", err);
    err = ext2fs_mkdir(fs, parent, ino, name);
    if (err) fail_ext("creating test directory", err);
    return ino;
}


static void refresh_inode_checksums(ext2_filsys fs) {
    size_t inode_bytes = fs->super->s_inode_size;
    unsigned char *buffer = calloc(1, inode_bytes);
    if (!buffer) fail_errno("allocating inode checksum buffer");
    for (ext2_ino_t ino = 1; ino <= fs->super->s_inodes_count; ++ino) {
        if (!ext2fs_test_inode_bitmap2(fs->inode_map, ino))
            continue;
        memset(buffer, 0, inode_bytes);
        errcode_t err = ext2fs_read_inode_full(
            fs, ino, (struct ext2_inode *)buffer, (int)inode_bytes);
        if (err) fail_ext("reading inode for checksum refresh", err);
        err = ext2fs_inode_csum_set(
            fs, ino, (struct ext2_inode_large *)buffer);
        if (err) fail_ext("setting inode checksum", err);
        err = ext2fs_write_inode_full(
            fs, ino, (struct ext2_inode *)buffer, (int)inode_bytes);
        if (err) fail_ext("writing inode checksum", err);
    }
    free(buffer);
}

static void prepare_image_file(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) fail_errno("creating test image");
    if (ftruncate(fd, IMAGE_BYTES)) fail_errno("sizing test image");
    if (close(fd)) fail_errno("closing test image");
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s IMAGE [--corrupt-directory-checksum]\n", argv[0]);
        return 2;
    }
    int corrupt_directory_checksum =
        argc == 3 && !strcmp(argv[2], "--corrupt-directory-checksum");
    if (argc == 3 && !corrupt_directory_checksum) {
        fprintf(stderr, "unknown option: %s\n", argv[2]);
        return 2;
    }
    prepare_image_file(argv[1]);

    struct ext2_super_block super;
    memset(&super, 0, sizeof(super));
    super.s_rev_level = EXT2_DYNAMIC_REV;
    super.s_log_block_size = 2;
    super.s_log_cluster_size = 2;
    super.s_blocks_per_group = 8192;
    super.s_clusters_per_group = 8192;
    super.s_inodes_per_group = 4096;
    super.s_inodes_count = 65536;
    super.s_inode_size = 256;
    super.s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
    super.s_desc_size = 64;
    super.s_log_groups_per_flex = 4;
    super.s_checksum_type = EXT2_CRC32C_CHKSUM;
    super.s_backup_bgs[0] = 1;
    super.s_backup_bgs[1] = 2;
    for (unsigned index = 0; index < sizeof(super.s_uuid); ++index)
        super.s_uuid[index] = (unsigned char)(index + 1);
    super.s_feature_compat = EXT2_FEATURE_COMPAT_DIR_INDEX |
                             EXT4_FEATURE_COMPAT_SPARSE_SUPER2;
    super.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
                               EXT3_FEATURE_INCOMPAT_EXTENTS |
                               EXT4_FEATURE_INCOMPAT_64BIT |
                               EXT4_FEATURE_INCOMPAT_FLEX_BG;
    super.s_feature_ro_compat = EXT2_FEATURE_RO_COMPAT_LARGE_FILE |
                                EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
                                EXT4_FEATURE_RO_COMPAT_DIR_NLINK |
                                EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
                                EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
    ext2fs_blocks_count_set(&super, FILESYSTEM_BLOCKS);

    ext2_filsys fs = NULL;
    errcode_t err = ext2fs_initialize(
        argv[1], EXT2_FLAG_RW | EXT2_FLAG_64BITS | EXT2_FLAG_EXCLUSIVE,
        &super, unix_io_manager, &fs);
    if (err) fail_ext("initializing test EXT4 filesystem", err);
    memcpy(fs->super->s_uuid, super.s_uuid, sizeof(super.s_uuid));
    fs->super->s_checksum_type = EXT2_CRC32C_CHKSUM;
    ext2fs_init_csum_seed(fs);
    err = ext2fs_allocate_tables(fs);
    if (err) fail_ext("allocating test EXT4 tables", err);
    for (ext2_ino_t ino = 1; ino < EXT2_GOOD_OLD_FIRST_INO; ++ino)
        ext2fs_inode_alloc_stats2(fs, ino, +1, ino == EXT2_ROOT_INO);
    err = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, NULL);
    if (err) fail_ext("creating test root directory", err);
    err = ext2fs_add_journal_inode2(
        fs, 2048, 0, EXT2_MKJOURNAL_NO_MNT_CHECK);
    if (err) fail_ext("creating test journal", err);

    ext2_ino_t music = create_directory(fs, EXT2_ROOT_INO, "Music");
    ext2_ino_t sub = create_directory(fs, music, "Sub");
    create_file(fs, music, "alpha.bin", 3U * 1024U * 1024U + 17U, 'A');
    create_file(fs, sub, "beta.bin", 5U * 1024U * 1024U + 9U, 'B');
    create_file(fs, music, "gamma.bin", 2U * 1024U * 1024U + 3U, 'C');
    for (unsigned index = 0; index < 400; ++index) {
        char name[64];
        snprintf(name, sizeof(name), "small-%03u.bin", index);
        create_file(fs, music, name, 1000U + index, (unsigned char)(index & 0xffU));
    }

    /* Keep one referenced file allocation close to the filesystem end.  The
       volume still has ample total free space, but no payload-sized contiguous
       free tail.  This exercises the native core's anonymous memory staging
       fallback instead of relying on an oversized image outside the filesystem. */
    HighAllocContext high = {
        .cursor = FILESYSTEM_BLOCKS - 64,
        .limit = FILESYSTEM_BLOCKS,
    };
    fs->priv_data = &high;
    ext2fs_set_alloc_block_callback(fs, high_alloc_block, NULL);
    create_file(fs, music, "high-pin.bin", BLOCK_SIZE, 'P');
    ext2fs_set_alloc_block_callback(fs, NULL, NULL);
    fs->priv_data = NULL;

    /* Real EXT4 directories normally have non-zero generations.  Directory
       block checksums include that generation, which is exactly the condition
       that exposed the revision-42 rewrite bug on Shannon's physical volume. */
    set_directory_generation(fs, EXT2_ROOT_INO, 0x10203040U, 1);
    set_directory_generation(fs, music, 0x20304050U, 1);
    set_directory_generation(fs, sub, 0x30405060U, 1);
    if (corrupt_directory_checksum)
        set_directory_generation(fs, music, 0x55667788U, 0);

    refresh_inode_checksums(fs);
    fs->super->s_state = EXT2_VALID_FS;
    ext2fs_mark_super_dirty(fs);
    err = ext2fs_close(fs);
    if (err) fail_ext("closing test EXT4 filesystem", err);
    return 0;
}
