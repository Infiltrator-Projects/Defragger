// SPDX-License-Identifier: GPL-3.0-or-later
#include <com_err.h>
#include <ext2fs/ext2fs.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_NAME "linux-defragger-ext-metadata-worker"

typedef struct {
    ext2fs_block_bitmap bitmap;
    uint64_t total_blocks;
} MarkContext;

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s analyse-json DEVICE\n",
                  PROGRAM_NAME);
}

static void mark_block(MarkContext *context, uint64_t block)
{
    if (block < context->total_blocks)
        ext2fs_mark_block_bitmap2(context->bitmap, (blk64_t)block);
}

static int mark_inode_block(ext2_filsys fs, blk64_t *blocknr,
                            e2_blkcnt_t blockcnt, blk64_t ref_blk,
                            int ref_offset, void *private_data)
{
    (void)fs;
    (void)blockcnt;
    (void)ref_blk;
    (void)ref_offset;
    MarkContext *context = private_data;
    if (*blocknr != 0)
        mark_block(context, (uint64_t)*blocknr);
    return 0;
}

static int mark_system_inode(ext2_filsys fs, ext2_ino_t ino,
                             MarkContext *context)
{
    if (ino == 0U || ino > fs->super->s_inodes_count || ino == EXT2_ROOT_INO)
        return 0;

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    errcode_t code = ext2fs_read_inode_full(
        fs, ino, (struct ext2_inode *)&inode, (int)sizeof(inode));
    if (code != 0)
        return (int)code;
    if (inode.i_mode == 0U || inode.i_links_count == 0U)
        return 0;

    code = ext2fs_block_iterate3(fs, ino, BLOCK_FLAG_READ_ONLY, NULL,
                                 mark_inode_block, context);
    return (int)code;
}

static int build_metadata_bitmap(ext2_filsys fs, ext2fs_block_bitmap *metadata)
{
    errcode_t code = ext2fs_allocate_block_bitmap(
        fs, "Linux Defragger EXT metadata map", metadata);
    if (code != 0)
        return (int)code;

    MarkContext context = {
        .bitmap = *metadata,
        .total_blocks = (uint64_t)ext2fs_blocks_count(fs->super),
    };

    for (uint64_t block = 0U; block < (uint64_t)fs->super->s_first_data_block;
         ++block) {
        mark_block(&context, block);
    }

    for (dgrp_t group = 0; group < fs->group_desc_count; ++group) {
        (void)ext2fs_reserve_super_and_bgd(fs, group, *metadata);

        mark_block(&context, (uint64_t)ext2fs_block_bitmap_loc(fs, group));
        mark_block(&context, (uint64_t)ext2fs_inode_bitmap_loc(fs, group));

        const uint64_t table = (uint64_t)ext2fs_inode_table_loc(fs, group);
        for (uint64_t index = 0U;
             index < (uint64_t)fs->inode_blocks_per_group; ++index) {
            if (table > UINT64_MAX - index)
                break;
            mark_block(&context, table + index);
        }
    }

    ext2_ino_t first_normal = fs->super->s_first_ino;
    if (first_normal == 0U)
        first_normal = EXT2_GOOD_OLD_FIRST_INO;
    for (ext2_ino_t ino = 1U; ino < first_normal; ++ino) {
        if (ino == EXT2_ROOT_INO)
            continue;
        int result = mark_system_inode(fs, ino, &context);
        if (result != 0)
            return result;
    }

    const ext2_ino_t special_inodes[] = {
        (ext2_ino_t)fs->super->s_journal_inum,
        (ext2_ino_t)fs->super->s_snapshot_inum,
        (ext2_ino_t)fs->super->s_usr_quota_inum,
        (ext2_ino_t)fs->super->s_grp_quota_inum,
        (ext2_ino_t)fs->super->s_prj_quota_inum,
        (ext2_ino_t)fs->super->s_orphan_file_inum,
    };
    for (size_t index = 0U;
         index < sizeof(special_inodes) / sizeof(special_inodes[0]); ++index) {
        const ext2_ino_t ino = special_inodes[index];
        if (ino < first_normal)
            continue;
        int result = mark_system_inode(fs, ino, &context);
        if (result != 0)
            return result;
    }

    return 0;
}

static bool is_allocated(ext2_filsys fs, uint64_t block)
{
    if (block < (uint64_t)fs->super->s_first_data_block)
        return true;
    return ext2fs_test_block_bitmap2(fs->block_map, (blk64_t)block) != 0;
}

static void emit_metadata_ranges(ext2_filsys fs,
                                 ext2fs_block_bitmap metadata)
{
    const uint64_t total_blocks = (uint64_t)ext2fs_blocks_count(fs->super);
    bool first = true;
    bool in_run = false;
    uint64_t run_start = 0U;

    (void)putchar('[');
    for (uint64_t block = 0U; block < total_blocks; ++block) {
        const bool marked =
            ext2fs_test_block_bitmap2(metadata, (blk64_t)block) != 0 &&
            is_allocated(fs, block);
        if (marked && !in_run) {
            run_start = block;
            in_run = true;
        }
        if (in_run && (!marked || block + 1U == total_blocks)) {
            const uint64_t run_end = marked ? block + 1U : block;
            if (!first)
                (void)putchar(',');
            first = false;
            (void)printf("[%" PRIu64 ",%" PRIu64 "]", run_start, run_end);
            in_run = false;
        }
    }
    (void)putchar(']');
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "analyse-json") != 0) {
        usage(stderr);
        return 2;
    }

    ext2_filsys fs = NULL;
    const int flags = EXT2_FLAG_64BITS | EXT2_FLAG_SOFTSUPP_FEATURES;
    errcode_t code = ext2fs_open(argv[2], flags, 0, 0, unix_io_manager, &fs);
    if (code != 0) {
        (void)fprintf(stderr, "%s: opening EXT filesystem: %s\n",
                      PROGRAM_NAME, error_message(code));
        return 1;
    }
    code = ext2fs_read_bitmaps(fs);
    if (code != 0) {
        (void)fprintf(stderr, "%s: reading EXT allocation bitmaps: %s\n",
                      PROGRAM_NAME, error_message(code));
        (void)ext2fs_close(fs);
        return 1;
    }

    ext2fs_block_bitmap metadata = NULL;
    const int metadata_result = build_metadata_bitmap(fs, &metadata);
    if (metadata_result != 0) {
        (void)fprintf(stderr, "%s: classifying EXT metadata: %s\n",
                      PROGRAM_NAME, error_message((errcode_t)metadata_result));
        if (metadata != NULL)
            ext2fs_free_block_bitmap(metadata);
        (void)ext2fs_close(fs);
        return 1;
    }

    (void)printf("{\"block_size\":%u,\"total_blocks\":%" PRIu64
                 ",\"metadata_ranges\":",
                 fs->blocksize, (uint64_t)ext2fs_blocks_count(fs->super));
    emit_metadata_ranges(fs, metadata);
    (void)puts("}");

    ext2fs_free_block_bitmap(metadata);
    (void)ext2fs_close(fs);
    return 0;
}
