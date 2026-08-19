// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"

#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include <com_err.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef LINUX_S_IFMT
#define LINUX_S_IFMT 0170000
#endif
#ifndef LINUX_S_IFREG
#define LINUX_S_IFREG 0100000
#endif

static int sql_error(sqlite3 *db, char **error, const char *action) {
    ext_set_error(error, "%s: %s", action, sqlite3_errmsg(db));
    return -1;
}

static int sql_exec(sqlite3 *db, const char *sql, char **error) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code != SQLITE_OK) {
        ext_set_error(error, "EXT plan database: %s", message != NULL ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return -1;
    }
    return 0;
}

static int insert_space(sqlite3_stmt *insert, uint64_t start, uint64_t length,
                        char **error) {
    if (length == 0) return 0;
    if (start > INT64_MAX || length > INT64_MAX) {
        ext_set_error(error, "EXT free-space range exceeds SQLite integer limits");
        return -1;
    }
    sqlite3_reset(insert);
    sqlite3_clear_bindings(insert);
    sqlite3_bind_int64(insert, 1, (sqlite3_int64)start);
    sqlite3_bind_int64(insert, 2, (sqlite3_int64)length);
    if (sqlite3_step(insert) != SQLITE_DONE) {
        ext_set_error(error, "cataloguing EXT legal space: %s", sqlite3_errmsg(sqlite3_db_handle(insert)));
        return -1;
    }
    return 0;
}

static int catalog_spaces(ext2_filsys fs, sqlite3 *db,
                          const ExtGeometry *geometry, char **error) {
    sqlite3_stmt *old_stmt = NULL, *insert = NULL;
    if (sqlite3_prepare_v2(db, "SELECT old FROM blocks ORDER BY old", -1, &old_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "INSERT INTO spaces VALUES (?,?)", -1, &insert, NULL) != SQLITE_OK) {
        ext_set_error(error, "preparing EXT free-space catalogue: %s", sqlite3_errmsg(db));
        goto fail;
    }
    int old_state = sqlite3_step(old_stmt);
    uint64_t next_old = old_state == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(old_stmt, 0) : UINT64_MAX;
    bool have_run = false;
    uint64_t run_start = 0, previous = 0;
    if (sql_exec(db, "DELETE FROM spaces; BEGIN IMMEDIATE", error) != 0) goto fail;
    for (uint64_t block = geometry->first_data_block; block < geometry->total_blocks; ++block) {
        while (next_old < block) {
            old_state = sqlite3_step(old_stmt);
            next_old = old_state == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(old_stmt, 0) : UINT64_MAX;
        }
        bool movable = next_old == block;
        bool allocated = ext2fs_test_block_bitmap2(fs->block_map, (blk64_t)block) != 0;
        bool legal = movable || !allocated;
        if (legal) {
            if (!have_run) {
                run_start = block;
                previous = block;
                have_run = true;
            } else if (block == previous + 1U) {
                previous = block;
            } else {
                if (insert_space(insert, run_start, previous - run_start + 1U, error) != 0) goto rollback;
                run_start = previous = block;
            }
        } else if (have_run) {
            if (insert_space(insert, run_start, previous - run_start + 1U, error) != 0) goto rollback;
            have_run = false;
        }
        if (movable) {
            old_state = sqlite3_step(old_stmt);
            next_old = old_state == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(old_stmt, 0) : UINT64_MAX;
        }
    }
    if (have_run && insert_space(insert, run_start, previous - run_start + 1U, error) != 0)
        goto rollback;
    if (old_state != SQLITE_DONE && old_state != SQLITE_ROW) {
        ext_set_error(error, "reading EXT movable-block catalogue: %s", sqlite3_errmsg(db));
        goto rollback;
    }
    if (sql_exec(db, "COMMIT", error) != 0) goto fail;
    sqlite3_finalize(old_stmt);
    sqlite3_finalize(insert);
    return 0;
rollback:
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
fail:
    sqlite3_finalize(old_stmt);
    sqlite3_finalize(insert);
    return -1;
}

int ext_assign_targets(ext2_filsys fs, sqlite3 *db,
                       const ExtGeometry *geometry, bool growth,
                       char **error) {
    if (catalog_spaces(fs, db, geometry, error) != 0) return -1;
    sqlite3_stmt *objects = NULL, *find_space = NULL, *delete_space = NULL,
                 *insert_space_stmt = NULL, *blocks = NULL, *update = NULL,
                 *reserve = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT inode,kind,data_block_count,allocation_block_count FROM objects ORDER BY sort_class,inode",
        -1, &objects, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT start,length FROM spaces WHERE length>=? ORDER BY start LIMIT 1",
        -1, &find_space, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "DELETE FROM spaces WHERE start=?", -1, &delete_space, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "INSERT INTO spaces VALUES (?,?)", -1, &insert_space_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT old FROM blocks WHERE inode=? ORDER BY sequence", -1, &blocks, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "UPDATE blocks SET target=? WHERE old=?", -1, &update, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "INSERT INTO reserves VALUES (?,?,?)", -1, &reserve, NULL) != SQLITE_OK) {
        ext_set_error(error, "preparing EXT target planner: %s", sqlite3_errmsg(db));
        goto fail;
    }
    if (sql_exec(db, "DELETE FROM reserves; UPDATE blocks SET target=NULL,placed=0; BEGIN IMMEDIATE", error) != 0)
        goto fail;
    int state = SQLITE_ROW;
    while ((state = sqlite3_step(objects)) == SQLITE_ROW) {
        if (ld_stop_requested()) {
            ext_set_error(error, "stop requested before EXT source commit");
            goto rollback;
        }
        sqlite3_int64 inode = sqlite3_column_int64(objects, 0);
        const char *kind = (const char *)sqlite3_column_text(objects, 1);
        uint64_t data_count = (uint64_t)sqlite3_column_int64(objects, 2);
        uint64_t allocation_count = (uint64_t)sqlite3_column_int64(objects, 3);
        if (allocation_count == 0) continue;
        bool ordinary_file = kind != NULL && strcmp(kind, "file") == 0 && inode >= EXT2_GOOD_OLD_FIRST_INO;
        uint64_t reserve_count = growth && ordinary_file && data_count != 0 ? (data_count + 9U) / 10U : 0U;
        uint64_t required = allocation_count + reserve_count;
        if (required > INT64_MAX) { ext_set_error(error, "EXT object allocation is too large"); goto rollback; }
        sqlite3_reset(find_space); sqlite3_clear_bindings(find_space);
        sqlite3_bind_int64(find_space, 1, (sqlite3_int64)required);
        if (sqlite3_step(find_space) != SQLITE_ROW) {
            ext_set_error(error,
                "inode %lld needs %llu physically contiguous blocks, but no legal EXT allocation run is large enough",
                (long long)inode, (unsigned long long)required);
            goto rollback;
        }
        uint64_t start = (uint64_t)sqlite3_column_int64(find_space, 0);
        uint64_t length = (uint64_t)sqlite3_column_int64(find_space, 1);
        sqlite3_reset(delete_space); sqlite3_clear_bindings(delete_space);
        sqlite3_bind_int64(delete_space, 1, (sqlite3_int64)start);
        if (sqlite3_step(delete_space) != SQLITE_DONE) { sql_error(db, error, "updating EXT free-space planner"); goto rollback; }
        if (length > required) {
            sqlite3_reset(insert_space_stmt); sqlite3_clear_bindings(insert_space_stmt);
            sqlite3_bind_int64(insert_space_stmt, 1, (sqlite3_int64)(start + required));
            sqlite3_bind_int64(insert_space_stmt, 2, (sqlite3_int64)(length - required));
            if (sqlite3_step(insert_space_stmt) != SQLITE_DONE) { sql_error(db, error, "splitting EXT free-space run"); goto rollback; }
        }
        sqlite3_reset(blocks); sqlite3_clear_bindings(blocks);
        sqlite3_bind_int64(blocks, 1, inode);
        uint64_t target = start, seen = 0;
        int block_state = SQLITE_ROW;
        while ((block_state = sqlite3_step(blocks)) == SQLITE_ROW) {
            sqlite3_int64 old = sqlite3_column_int64(blocks, 0);
            sqlite3_reset(update); sqlite3_clear_bindings(update);
            sqlite3_bind_int64(update, 1, (sqlite3_int64)target);
            sqlite3_bind_int64(update, 2, old);
            if (sqlite3_step(update) != SQLITE_DONE) { sql_error(db, error, "assigning EXT target block"); goto rollback; }
            target++; seen++;
        }
        if (block_state != SQLITE_DONE || seen != allocation_count) {
            ext_set_error(error, "inode %lld block catalogue changed during planning", (long long)inode);
            goto rollback;
        }
        if (reserve_count != 0) {
            sqlite3_reset(reserve); sqlite3_clear_bindings(reserve);
            sqlite3_bind_int64(reserve, 1, inode);
            sqlite3_bind_int64(reserve, 2, (sqlite3_int64)target);
            sqlite3_bind_int64(reserve, 3, (sqlite3_int64)reserve_count);
            if (sqlite3_step(reserve) != SQLITE_DONE) { sql_error(db, error, "recording EXT growth reserve"); goto rollback; }
        }
    }
    if (state != SQLITE_DONE) { sql_error(db, error, "reading EXT object planner"); goto rollback; }
    if (sql_exec(db,
        "CREATE UNIQUE INDEX IF NOT EXISTS blocks_target ON blocks(target);"
        "COMMIT", error) != 0) goto fail;
    sqlite3_stmt *check = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM blocks WHERE target IS NULL", -1, &check, NULL) != SQLITE_OK ||
        sqlite3_step(check) != SQLITE_ROW || sqlite3_column_int64(check, 0) != 0) {
        sqlite3_finalize(check);
        ext_set_error(error, "the EXT target planner left unassigned blocks");
        goto fail;
    }
    sqlite3_finalize(check);
    sqlite3_finalize(objects); sqlite3_finalize(find_space); sqlite3_finalize(delete_space);
    sqlite3_finalize(insert_space_stmt); sqlite3_finalize(blocks); sqlite3_finalize(update); sqlite3_finalize(reserve);
    return 0;
rollback:
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
fail:
    sqlite3_finalize(objects); sqlite3_finalize(find_space); sqlite3_finalize(delete_space);
    sqlite3_finalize(insert_space_stmt); sqlite3_finalize(blocks); sqlite3_finalize(update); sqlite3_finalize(reserve);
    return -1;
}

int ext_plan_move_count(sqlite3 *db, uint64_t *count, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM blocks WHERE old<>target", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return sql_error(db, error, "counting EXT relocations");
    }
    *count = (uint64_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

static int copy_block(int fd, uint32_t block_size, uint64_t source, uint64_t target, char **error) {
    uint8_t *payload = ld_xmalloc(block_size);
    ssize_t got = ld_pread_full(fd, payload, block_size, source * block_size);
    if (got < 0 || (size_t)got != block_size) {
        ext_set_error(error, "short source read while arranging EXT working image");
        free(payload); return -1;
    }
    ssize_t wrote = ld_pwrite_full(fd, payload, block_size, target * block_size);
    free(payload);
    if (wrote < 0 || (size_t)wrote != block_size) {
        ext_set_error(error, "short destination write while arranging EXT working image");
        return -1;
    }
    return 0;
}

int ext_permute_payloads(const char *stage, sqlite3 *db, uint32_t block_size,
                         uint64_t move_count, char **error) {
    int fd = open(stage, O_RDWR | O_CLOEXEC);
    if (fd < 0) { ext_set_error(error, "cannot open EXT working image for relocation: %s", strerror(errno)); return -1; }
    sqlite3_stmt *terminals = NULL, *predecessor = NULL, *mark = NULL, *unplaced = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT b.target FROM blocks b LEFT JOIN blocks source ON source.old=b.target "
        "WHERE b.old<>b.target AND source.old IS NULL ORDER BY b.target",
        -1, &terminals, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
        "SELECT old FROM blocks WHERE target=? AND old<>target AND placed=0",
        -1, &predecessor, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "UPDATE blocks SET placed=1 WHERE old=?", -1, &mark, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT old FROM blocks WHERE old<>target AND placed=0 LIMIT 1", -1, &unplaced, NULL) != SQLITE_OK) {
        close(fd); return sql_error(db, error, "preparing EXT block permutation");
    }
    uint64_t placed = 0;
    int state;
    while ((state = sqlite3_step(terminals)) == SQLITE_ROW) {
        uint64_t free_block = (uint64_t)sqlite3_column_int64(terminals, 0);
        while (true) {
            sqlite3_reset(predecessor); sqlite3_clear_bindings(predecessor);
            sqlite3_bind_int64(predecessor, 1, (sqlite3_int64)free_block);
            int pstate = sqlite3_step(predecessor);
            if (pstate == SQLITE_DONE) break;
            if (pstate != SQLITE_ROW) { sql_error(db, error, "reading EXT block predecessor"); goto fail; }
            uint64_t old = (uint64_t)sqlite3_column_int64(predecessor, 0);
            if (copy_block(fd, block_size, old, free_block, error) != 0) goto fail;
            sqlite3_reset(mark); sqlite3_clear_bindings(mark); sqlite3_bind_int64(mark, 1, (sqlite3_int64)old);
            if (sqlite3_step(mark) != SQLITE_DONE) { sql_error(db, error, "marking EXT block placement"); goto fail; }
            placed++; free_block = old;
            if ((placed % 8192U) == 0U) {
                if (ld_stop_requested()) { ext_set_error(error, "stop requested before EXT source commit"); goto fail; }
                printf("EXT working-image placement: %llu of %llu blocks.\n",
                       (unsigned long long)placed, (unsigned long long)move_count); fflush(stdout);
            }
        }
    }
    if (state != SQLITE_DONE) { sql_error(db, error, "reading EXT terminal relocation blocks"); goto fail; }
    while (true) {
        sqlite3_reset(unplaced);
        int ustate = sqlite3_step(unplaced);
        if (ustate == SQLITE_DONE) break;
        if (ustate != SQLITE_ROW) { sql_error(db, error, "reading EXT relocation cycle"); goto fail; }
        uint64_t start = (uint64_t)sqlite3_column_int64(unplaced, 0);
        uint8_t *saved = ld_xmalloc(block_size);
        ssize_t got = ld_pread_full(fd, saved, block_size, start * block_size);
        if (got < 0 || (size_t)got != block_size) { free(saved); ext_set_error(error, "short read while staging an EXT block cycle"); goto fail; }
        uint64_t free_block = start;
        while (true) {
            sqlite3_reset(predecessor); sqlite3_clear_bindings(predecessor);
            sqlite3_bind_int64(predecessor, 1, (sqlite3_int64)free_block);
            int pstate = sqlite3_step(predecessor);
            if (pstate != SQLITE_ROW) { free(saved); ext_set_error(error, "broken EXT block-permutation cycle"); goto fail; }
            uint64_t old = (uint64_t)sqlite3_column_int64(predecessor, 0);
            if (old == start) {
                ssize_t wrote = ld_pwrite_full(fd, saved, block_size, free_block * block_size);
                free(saved);
                if (wrote < 0 || (size_t)wrote != block_size) { ext_set_error(error, "short write while closing an EXT block cycle"); goto fail; }
                sqlite3_reset(mark); sqlite3_clear_bindings(mark); sqlite3_bind_int64(mark, 1, (sqlite3_int64)start);
                if (sqlite3_step(mark) != SQLITE_DONE) { sql_error(db, error, "marking EXT cycle completion"); goto fail; }
                placed++; break;
            }
            if (copy_block(fd, block_size, old, free_block, error) != 0) { free(saved); goto fail; }
            sqlite3_reset(mark); sqlite3_clear_bindings(mark); sqlite3_bind_int64(mark, 1, (sqlite3_int64)old);
            if (sqlite3_step(mark) != SQLITE_DONE) { free(saved); sql_error(db, error, "marking EXT cycle placement"); goto fail; }
            placed++; free_block = old;
            if ((placed % 8192U) == 0U && ld_stop_requested()) {
                free(saved); ext_set_error(error, "stop requested before EXT source commit"); goto fail;
            }
        }
    }
    if (fsync(fd) != 0) { ext_set_error(error, "syncing arranged EXT working image: %s", strerror(errno)); goto fail; }
    sqlite3_stmt *check = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM blocks WHERE old<>target AND placed=1", -1, &check, NULL) != SQLITE_OK ||
        sqlite3_step(check) != SQLITE_ROW || (uint64_t)sqlite3_column_int64(check, 0) != move_count) {
        sqlite3_finalize(check); ext_set_error(error, "not every EXT relocation was placed"); goto fail;
    }
    sqlite3_finalize(check);
    sqlite3_finalize(terminals); sqlite3_finalize(predecessor); sqlite3_finalize(mark); sqlite3_finalize(unplaced);
    close(fd);
    return 0;
fail:
    sqlite3_finalize(terminals); sqlite3_finalize(predecessor); sqlite3_finalize(mark); sqlite3_finalize(unplaced);
    close(fd);
    return -1;
}

typedef struct {
    int64_t logical;
    uint64_t old_block;
    uint64_t target;
    bool changed;
} Mapping;

typedef struct {
    Mapping *items;
    size_t count;
    size_t position;
    size_t changed;
    bool mismatch;
} MappingContext;

static int replace_mapping(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                           blk64_t ref_blk, int ref_offset, void *private_data) {
    (void)fs; (void)ref_blk; (void)ref_offset;
    MappingContext *context = private_data;
    if (*blocknr == 0) return 0;
    if (context->position >= context->count) { context->mismatch = true; return BLOCK_ABORT; }
    Mapping *mapping = &context->items[context->position++];
    if (mapping->logical != (int64_t)blockcnt || mapping->old_block != (uint64_t)*blocknr) {
        context->mismatch = true;
        return BLOCK_ABORT;
    }
    if (!mapping->changed) return 0;
    *blocknr = (blk64_t)mapping->target;
    context->changed++;
    return BLOCK_CHANGED;
}

static int reserve_blocks(ext2_filsys fs, sqlite3 *db, bool allocate, char **error) {
    sqlite3_stmt *stmt = NULL, *is_old = NULL;
    if (sqlite3_prepare_v2(db, "SELECT start,length FROM reserves", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT 1 FROM blocks WHERE old=?", -1, &is_old, NULL) != SQLITE_OK)
        return sql_error(db, error, "preparing EXT reserve updates");
    int state;
    while ((state = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t start = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t length = (uint64_t)sqlite3_column_int64(stmt, 1);
        for (uint64_t offset = 0; offset < length; ++offset) {
            uint64_t block = start + offset;
            sqlite3_reset(is_old); sqlite3_clear_bindings(is_old); sqlite3_bind_int64(is_old, 1, (sqlite3_int64)block);
            int old_state = sqlite3_step(is_old);
            if (old_state == SQLITE_DONE)
                ext2fs_block_alloc_stats2(fs, (blk64_t)block, allocate ? +1 : -1);
            else if (old_state != SQLITE_ROW) { sqlite3_finalize(stmt); sqlite3_finalize(is_old); return sql_error(db, error, "checking EXT reserve overlap"); }
        }
    }
    sqlite3_finalize(stmt); sqlite3_finalize(is_old);
    if (state != SQLITE_DONE) return sql_error(db, error, "reading EXT reserves");
    return 0;
}

int ext_apply_mappings(const char *stage, sqlite3 *db, bool allow_stop,
                       char **error) {
    ext2_filsys fs = NULL;
    if (ext_open_fs(stage, true, &fs, error) != 0) return -1;
    int result = -1;
    sqlite3_stmt *new_targets = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT b.target FROM blocks b LEFT JOIN blocks source ON source.old=b.target WHERE source.old IS NULL",
        -1, &new_targets, NULL) != SQLITE_OK) { sql_error(db, error, "preparing EXT target allocations"); goto done; }
    int state;
    while ((state = sqlite3_step(new_targets)) == SQLITE_ROW)
        ext2fs_block_alloc_stats2(fs, (blk64_t)sqlite3_column_int64(new_targets, 0), +1);
    if (state != SQLITE_DONE) { sql_error(db, error, "reading EXT new allocations"); goto done; }
    if (reserve_blocks(fs, db, true, error) != 0) goto done;
    errcode_t code = ext2fs_write_bitmaps(fs);
    if (code != 0) { ext_set_error(error, "allocating EXT target blocks: %s", error_message(code)); goto done; }
    code = ext2fs_flush(fs);
    if (code != 0) { ext_set_error(error, "flushing EXT target allocations: %s", error_message(code)); goto done; }

    sqlite3_stmt *inodes = NULL, *mappings = NULL;
    if (sqlite3_prepare_v2(db, "SELECT DISTINCT inode FROM blocks WHERE old<>target ORDER BY inode", -1, &inodes, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT logical,old,target FROM blocks WHERE inode=? ORDER BY sequence", -1, &mappings, NULL) != SQLITE_OK) {
        sql_error(db, error, "preparing EXT inode remapping"); sqlite3_finalize(inodes); sqlite3_finalize(mappings); goto done;
    }
    size_t inode_index = 0;
    while ((state = sqlite3_step(inodes)) == SQLITE_ROW) {
        if (allow_stop && ld_stop_requested()) { ext_set_error(error, "stop requested before EXT source commit"); sqlite3_finalize(inodes); sqlite3_finalize(mappings); goto done; }
        ext2_ino_t ino = (ext2_ino_t)sqlite3_column_int64(inodes, 0);
        sqlite3_reset(mappings); sqlite3_clear_bindings(mappings); sqlite3_bind_int64(mappings, 1, (sqlite3_int64)ino);
        MappingContext context = {0};
        int mstate;
        while ((mstate = sqlite3_step(mappings)) == SQLITE_ROW) {
            context.items = ld_xrealloc(context.items, (context.count + 1U) * sizeof(*context.items));
            Mapping *item = &context.items[context.count++];
            item->logical = sqlite3_column_int64(mappings, 0);
            item->old_block = (uint64_t)sqlite3_column_int64(mappings, 1);
            item->target = (uint64_t)sqlite3_column_int64(mappings, 2);
            item->changed = item->old_block != item->target;
        }
        if (mstate != SQLITE_DONE) { free(context.items); sql_error(db, error, "reading EXT inode mappings"); sqlite3_finalize(inodes); sqlite3_finalize(mappings); goto done; }
        code = ext2fs_block_iterate3(fs, ino, BLOCK_FLAG_DATA_ONLY, NULL,
                                      replace_mapping, &context);
        size_t expected_changed = 0;
        for (size_t index = 0; index < context.count; ++index) if (context.items[index].changed) expected_changed++;
        if (code != 0 || context.mismatch || context.position != context.count || context.changed != expected_changed) {
            free(context.items);
            ext_set_error(error, "EXT inode %u block mapping changed unexpectedly during native remap", (unsigned)ino);
            sqlite3_finalize(inodes); sqlite3_finalize(mappings); goto done;
        }
        free(context.items);
        inode_index++;
        if ((inode_index % 128U) == 0U) {
            code = ext2fs_flush(fs);
            if (code != 0) { ext_set_error(error, "flushing EXT inode mappings: %s", error_message(code)); sqlite3_finalize(inodes); sqlite3_finalize(mappings); goto done; }
        }
    }
    sqlite3_finalize(inodes); sqlite3_finalize(mappings);
    if (state != SQLITE_DONE) { sql_error(db, error, "reading EXT remapped inode list"); goto done; }
    code = ext2fs_flush(fs);
    if (code != 0) { ext_set_error(error, "flushing all EXT inode mappings: %s", error_message(code)); goto done; }

    sqlite3_stmt *old_blocks = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT b.old FROM blocks b LEFT JOIN blocks target ON target.target=b.old WHERE target.target IS NULL",
        -1, &old_blocks, NULL) != SQLITE_OK) { sql_error(db, error, "preparing EXT old-allocation release"); goto done; }
    while ((state = sqlite3_step(old_blocks)) == SQLITE_ROW)
        ext2fs_block_alloc_stats2(fs, (blk64_t)sqlite3_column_int64(old_blocks, 0), -1);
    sqlite3_finalize(old_blocks);
    if (state != SQLITE_DONE) { sql_error(db, error, "reading EXT old allocations"); goto done; }
    if (reserve_blocks(fs, db, false, error) != 0) goto done;
    code = ext2fs_write_bitmaps(fs);
    if (code != 0) { ext_set_error(error, "freeing old EXT data blocks: %s", error_message(code)); goto done; }
    code = ext2fs_flush(fs);
    if (code != 0) { ext_set_error(error, "flushing final EXT allocation bitmaps: %s", error_message(code)); goto done; }
    result = 0;
done:
    sqlite3_finalize(new_targets);
    if (ext2fs_close(fs) != 0 && result == 0) { ext_set_error(error, "closing modified EXT working image failed"); result = -1; }
    return result;
}

typedef struct {
    uint64_t physical;
    int64_t logical;
} ExtDigestBlock;

typedef struct {
    ExtDigestBlock *items;
    size_t count;
} ExtDigestContext;

static int collect_digest_block(ext2_filsys fs, blk64_t *blocknr,
                                e2_blkcnt_t blockcnt, blk64_t ref_blk,
                                int ref_offset, void *private_data) {
    (void)fs;
    (void)ref_blk;
    (void)ref_offset;
    ExtDigestContext *context = private_data;
    if (blockcnt >= 0 && *blocknr != 0) {
        context->items = ld_xrealloc(
            context->items, (context->count + 1U) * sizeof(*context->items));
        context->items[context->count++] = (ExtDigestBlock){
            .physical = (uint64_t)*blocknr,
            .logical = (int64_t)blockcnt,
        };
    }
    return 0;
}

static int digest_inode(ext2_filsys fs, int fd, ext2_ino_t ino,
                        uint32_t block_size,
                        uint8_t output[SHA256_DIGEST_LENGTH], char **error) {
    ExtDigestContext context = {0};
    errcode_t code = ext2fs_block_iterate3(
        fs, ino, BLOCK_FLAG_READ_ONLY | BLOCK_FLAG_DATA_ONLY,
        NULL, collect_digest_block, &context);
    if (code != 0) {
        free(context.items);
        ext_set_error(error, "reading EXT inode payload for verification: %s",
                      error_message(code));
        return -1;
    }
    SHA256_CTX digest;
    if (SHA256_Init(&digest) != 1) {
        free(context.items);
        ext_set_error(error, "initializing EXT verification digest failed");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc(block_size);
    for (size_t index = 0; index < context.count; ++index) {
        ssize_t got = ld_pread_full(
            fd, buffer, block_size, context.items[index].physical * block_size);
        if (got < 0 || (size_t)got != block_size) {
            free(buffer);
            free(context.items);
            ext_set_error(error, "short read verifying EXT payload");
            return -1;
        }
        uint8_t logical[8];
        uint64_t value = (uint64_t)context.items[index].logical;
        for (unsigned byte = 0; byte < 8U; ++byte)
            logical[byte] = (uint8_t)(value >> (byte * 8U));
        if (SHA256_Update(&digest, logical, sizeof(logical)) != 1 ||
            SHA256_Update(&digest, buffer, block_size) != 1) {
            free(buffer);
            free(context.items);
            ext_set_error(error, "updating EXT verification digest failed");
            return -1;
        }
    }
    if (SHA256_Final(output, &digest) != 1) {
        free(buffer);
        free(context.items);
        ext_set_error(error, "finalizing EXT verification digest failed");
        return -1;
    }
    free(buffer);
    free(context.items);
    return 0;
}

typedef struct {
    sqlite3_stmt *expected;
    size_t seen;
    bool mismatch;
} VerifyMapContext;

static int verify_mapping(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                          blk64_t ref_blk, int ref_offset, void *private_data) {
    (void)fs; (void)ref_blk; (void)ref_offset;
    if (*blocknr == 0) return 0;
    VerifyMapContext *context = private_data;
    int state = sqlite3_step(context->expected);
    if (state != SQLITE_ROW ||
        sqlite3_column_int64(context->expected, 0) != (sqlite3_int64)blockcnt ||
        (uint64_t)sqlite3_column_int64(context->expected, 1) != (uint64_t)*blocknr) {
        context->mismatch = true;
        return BLOCK_ABORT;
    }
    context->seen++;
    return 0;
}

int ext_verify_stage(const char *stage, sqlite3 *db, const ExtGeometry *geometry,
                     bool growth, ExtCatalogue *verified, char **error) {
    ext2_filsys fs = NULL;
    if (ext_open_fs(stage, false, &fs, error) != 0) return -1;
    int result = -1;
    if (ext_validate_metadata(fs, true, error) != 0) goto done;
    int fd = open(stage, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { ext_set_error(error, "cannot open EXT working image for verification: %s", strerror(errno)); goto done; }
    sqlite3_stmt *objects = NULL, *expected = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT inode,mode,links,size,data_block_count,allocation_block_count,payload_sha256 FROM objects ORDER BY inode",
        -1, &objects, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT logical,target FROM blocks WHERE inode=? ORDER BY sequence", -1, &expected, NULL) != SQLITE_OK) {
        sql_error(db, error, "preparing EXT verification"); goto close_fd;
    }
    int state;
    while ((state = sqlite3_step(objects)) == SQLITE_ROW) {
        ext2_ino_t ino = (ext2_ino_t)sqlite3_column_int64(objects, 0);
        struct ext2_inode_large inode;
        errcode_t code = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
        if (code != 0) { ext_set_error(error, "reading EXT inode %u during verification: %s", (unsigned)ino, error_message(code)); goto finalize; }
        uint16_t mode = (uint16_t)sqlite3_column_int(objects, 1);
        uint16_t links = (uint16_t)sqlite3_column_int(objects, 2);
        uint64_t size = (uint64_t)sqlite3_column_int64(objects, 3);
        uint64_t expected_allocations = (uint64_t)sqlite3_column_int64(objects, 5);
        uint64_t actual_size = ((unsigned)inode.i_mode & LINUX_S_IFMT) == LINUX_S_IFREG ?
                               EXT2_I_SIZE((struct ext2_inode *)&inode) : inode.i_size;
        if (inode.i_mode != mode || inode.i_links_count != links || actual_size != size) {
            ext_set_error(error, "EXT inode %u metadata verification failed", (unsigned)ino); goto finalize;
        }
        sqlite3_reset(expected); sqlite3_clear_bindings(expected); sqlite3_bind_int64(expected, 1, (sqlite3_int64)ino);
        VerifyMapContext context = {.expected = expected};
        code = ext2fs_block_iterate3(fs, ino,
                                      BLOCK_FLAG_READ_ONLY | BLOCK_FLAG_DATA_ONLY,
                                      NULL, verify_mapping, &context);
        int trailing = sqlite3_step(expected);
        if (code != 0 || context.mismatch || context.seen != expected_allocations || trailing != SQLITE_DONE) {
            ext_set_error(error, "EXT inode %u canonical allocation verification failed", (unsigned)ino); goto finalize;
        }
        uint8_t digest[SHA256_DIGEST_LENGTH];
        if (digest_inode(fs, fd, ino, geometry->block_size, digest, error) != 0) goto finalize;
        const void *stored = sqlite3_column_blob(objects, 6);
        int stored_size = sqlite3_column_bytes(objects, 6);
        if (stored == NULL || stored_size != SHA256_DIGEST_LENGTH || memcmp(stored, digest, SHA256_DIGEST_LENGTH) != 0) {
            ext_set_error(error, "EXT inode %u payload checksum changed", (unsigned)ino); goto finalize;
        }
    }
    if (state != SQLITE_DONE) { sql_error(db, error, "reading EXT verification objects"); goto finalize; }
    if (growth) {
        sqlite3_stmt *reserves = NULL;
        if (sqlite3_prepare_v2(db, "SELECT start,length FROM reserves", -1, &reserves, NULL) != SQLITE_OK) { sql_error(db, error, "preparing EXT reserve verification"); goto finalize; }
        int rstate;
        while ((rstate = sqlite3_step(reserves)) == SQLITE_ROW) {
            uint64_t start = (uint64_t)sqlite3_column_int64(reserves, 0);
            uint64_t length = (uint64_t)sqlite3_column_int64(reserves, 1);
            for (uint64_t offset = 0; offset < length; ++offset) {
                if (ext2fs_test_block_bitmap2(fs->block_map, (blk64_t)(start + offset))) {
                    sqlite3_finalize(reserves); ext_set_error(error, "EXT Growth Defrag reserve block is allocated"); goto finalize;
                }
            }
        }
        sqlite3_finalize(reserves);
        if (rstate != SQLITE_DONE) { sql_error(db, error, "reading EXT reserve verification"); goto finalize; }
    }
    {
        ExtGeometry rescanned_geometry;
        if (ext_scan_catalogue(stage, &rescanned_geometry, verified, error) != 0) goto finalize;
        if (verified->malformed_inodes != 0 || verified->fragmented_files != 0 || verified->fragmented_directories != 0) {
            ext_set_error(error, "native EXT catalogue still reports malformed or fragmented inodes after relocation");
            ext_catalogue_free(verified); goto finalize;
        }
        if (growth && !verified->growth_10_satisfied) {
            ext_set_error(error, "native EXT catalogue did not verify every 10%% growth reserve");
            ext_catalogue_free(verified); goto finalize;
        }
    }
    result = 0;
finalize:
    sqlite3_finalize(objects); sqlite3_finalize(expected);
close_fd:
    close(fd);
done:
    (void)ext2fs_close(fs);
    return result;
}
