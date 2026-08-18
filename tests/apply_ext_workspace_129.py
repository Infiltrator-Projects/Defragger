#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: anchor count {count}, expected 1\nANCHOR:\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


workspace_h = r'''// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_EXT_WORKSPACE_H
#define LINUX_DEFRAGGER_EXT_WORKSPACE_H

#include "ext_native.h"

#include <stdint.h>

#define EXT_WORKSPACE_UNAVAILABLE 1

typedef struct {
    uint64_t start;
    uint64_t blocks;
    uint64_t batch_blocks;
    uint32_t block_size;
} ExtWorkspace;

int ext_workspace_prepare(ext2_filsys fs, sqlite3 *db,
                          const ExtGeometry *geometry,
                          uint64_t requested_batch_blocks,
                          ExtWorkspace *workspace, char **error);
int ext_workspace_load(sqlite3 *db, ExtWorkspace *workspace, char **error);
int ext_workspace_stage(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error);
int ext_workspace_place(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error);
int ext_workspace_restore(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                          char **error);

#endif
'''

workspace_c = r'''// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_workspace.h"

#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include <errno.h>
#include <inttypes.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXT_WORKSPACE_COPY_CHUNK (4U * 1024U * 1024U)
#define EXT_WORKSPACE_TRANSACTION_BYTES (64U * 1024U * 1024U)

static int workspace_sql_error(sqlite3 *db, char **error, const char *action) {
    ext_set_error(error, "%s: %s", action, sqlite3_errmsg(db));
    return -1;
}

static int workspace_sql_exec(sqlite3 *db, const char *sql, char **error) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code != SQLITE_OK) {
        ext_set_error(error, "EXT workspace database: %s",
                      message != NULL ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return -1;
    }
    return 0;
}

static bool original_allocated(ext2_filsys fs, const ExtGeometry *geometry,
                               uint64_t block) {
    if (block == 0 || block < geometry->first_data_block) return true;
    return ext2fs_test_block_bitmap2(fs->block_map, (blk64_t)block) != 0;
}

static uint64_t allocated_block_count(ext2_filsys fs,
                                      const ExtGeometry *geometry) {
    uint64_t count = 0;
    for (uint64_t block = 0; block < geometry->total_blocks; ++block) {
        if (original_allocated(fs, geometry, block)) count++;
    }
    return count;
}

static int plan_range_is_unused(sqlite3 *db, uint64_t start, uint64_t end,
                                bool *unused, char **error) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "(SELECT COUNT(*) FROM blocks WHERE target>=?1 AND target<?2) + "
        "(SELECT COUNT(*) FROM reserves WHERE start<?2 AND start+length>?1)";
    if (start > INT64_MAX || end > INT64_MAX ||
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "checking EXT workspace target overlap");
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)end);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "reading EXT workspace target overlap");
    }
    *unused = sqlite3_column_int64(stmt, 0) == 0;
    sqlite3_finalize(stmt);
    return 0;
}

static int find_high_workspace(ext2_filsys fs, sqlite3 *db,
                               const ExtGeometry *geometry, uint64_t needed,
                               uint64_t *start_out, char **error) {
    if (needed == 0 || needed > geometry->free_blocks) return EXT_WORKSPACE_UNAVAILABLE;
    uint64_t run_end = geometry->total_blocks;
    uint64_t cursor = geometry->total_blocks;
    bool in_run = false;
    while (cursor > geometry->first_data_block) {
        cursor--;
        bool free_block = !original_allocated(fs, geometry, cursor);
        if (free_block) {
            if (!in_run) {
                run_end = cursor + 1U;
                in_run = true;
            }
        } else if (in_run) {
            uint64_t run_start = cursor + 1U;
            uint64_t length = run_end - run_start;
            if (length >= needed) {
                uint64_t candidate = run_end - needed;
                bool unused = false;
                if (plan_range_is_unused(db, candidate, run_end, &unused, error) != 0)
                    return -1;
                if (unused) {
                    *start_out = candidate;
                    return 0;
                }
            }
            in_run = false;
        }
    }
    if (in_run) {
        uint64_t run_start = geometry->first_data_block;
        uint64_t length = run_end - run_start;
        if (length >= needed) {
            uint64_t candidate = run_end - needed;
            bool unused = false;
            if (plan_range_is_unused(db, candidate, run_end, &unused, error) != 0)
                return -1;
            if (unused) {
                *start_out = candidate;
                return 0;
            }
        }
    }
    return EXT_WORKSPACE_UNAVAILABLE;
}

static uint64_t bounded_batch_blocks(uint32_t block_size,
                                     uint64_t requested) {
    uint64_t cap = EXT_WORKSPACE_TRANSACTION_BYTES / (uint64_t)block_size;
    if (cap == 0) cap = 1;
    if (requested == 0 || requested > cap) return cap;
    return requested;
}

static int set_state_u64(sqlite3 *db, const char *key, uint64_t value,
                         char **error) {
    sqlite3_stmt *stmt = NULL;
    if (value > INT64_MAX ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO direct_state(key,value) VALUES (?,?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "preparing EXT workspace state");
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)value);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "recording EXT workspace state");
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int get_state_u64(sqlite3 *db, const char *key, uint64_t *value,
                         char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM direct_state WHERE key=?", -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "preparing EXT workspace state read");
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace state is incomplete (%s)", key);
        return -1;
    }
    sqlite3_int64 raw = sqlite3_column_int64(stmt, 0);
    if (raw < 0) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace state is invalid (%s)", key);
        return -1;
    }
    *value = (uint64_t)raw;
    sqlite3_finalize(stmt);
    return 0;
}

int ext_workspace_prepare(ext2_filsys fs, sqlite3 *db,
                          const ExtGeometry *geometry,
                          uint64_t requested_batch_blocks,
                          ExtWorkspace *workspace, char **error) {
    memset(workspace, 0, sizeof(*workspace));
    uint64_t needed = allocated_block_count(fs, geometry);
    uint64_t start = 0;
    int found = find_high_workspace(fs, db, geometry, needed, &start, error);
    if (found != 0) return found;

    if (workspace_sql_exec(db,
        "CREATE TABLE IF NOT EXISTS direct_workspace("
        "original INTEGER PRIMARY KEY,slot INTEGER UNIQUE NOT NULL);"
        "CREATE TABLE IF NOT EXISTS direct_state("
        "key TEXT PRIMARY KEY,value BLOB NOT NULL);"
        "DELETE FROM direct_workspace;DELETE FROM direct_state;BEGIN IMMEDIATE;",
        error) != 0) return -1;

    sqlite3_stmt *insert = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO direct_workspace(original,slot) VALUES (?,?)",
            -1, &insert, NULL) != SQLITE_OK) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return workspace_sql_error(db, error, "preparing EXT workspace map");
    }
    uint64_t index = 0;
    for (uint64_t block = 0; block < geometry->total_blocks; ++block) {
        if (!original_allocated(fs, geometry, block)) continue;
        uint64_t slot = start + index;
        if (block > INT64_MAX || slot > INT64_MAX) {
            sqlite3_finalize(insert);
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            ext_set_error(error, "EXT workspace block number exceeds SQLite limits");
            return -1;
        }
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        sqlite3_bind_int64(insert, 1, (sqlite3_int64)block);
        sqlite3_bind_int64(insert, 2, (sqlite3_int64)slot);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            sqlite3_finalize(insert);
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return workspace_sql_error(db, error, "recording EXT workspace map");
        }
        index++;
    }
    sqlite3_finalize(insert);
    if (index != needed || workspace_sql_exec(db, "COMMIT", error) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (error != NULL && *error == NULL)
            ext_set_error(error, "EXT workspace allocation count changed while planning");
        return -1;
    }

    workspace->start = start;
    workspace->blocks = needed;
    workspace->block_size = geometry->block_size;
    workspace->batch_blocks = bounded_batch_blocks(geometry->block_size,
                                                    requested_batch_blocks);
    if (set_state_u64(db, "workspace_start", workspace->start, error) != 0 ||
        set_state_u64(db, "workspace_blocks", workspace->blocks, error) != 0 ||
        set_state_u64(db, "block_size", workspace->block_size, error) != 0 ||
        set_state_u64(db, "batch_blocks", workspace->batch_blocks, error) != 0)
        return -1;
    return 0;
}

int ext_workspace_load(sqlite3 *db, ExtWorkspace *workspace, char **error) {
    uint64_t block_size = 0;
    memset(workspace, 0, sizeof(*workspace));
    if (get_state_u64(db, "workspace_start", &workspace->start, error) != 0 ||
        get_state_u64(db, "workspace_blocks", &workspace->blocks, error) != 0 ||
        get_state_u64(db, "block_size", &block_size, error) != 0 ||
        get_state_u64(db, "batch_blocks", &workspace->batch_blocks, error) != 0)
        return -1;
    if (block_size == 0 || block_size > UINT32_MAX || workspace->batch_blocks == 0) {
        ext_set_error(error, "EXT workspace geometry is invalid");
        return -1;
    }
    workspace->block_size = (uint32_t)block_size;
    return 0;
}

static int copy_bytes(int fd, uint64_t source_offset, uint64_t destination_offset,
                      uint64_t length, uint8_t *buffer, bool allow_stop,
                      char **error) {
    uint64_t done = 0;
    while (done < length) {
        size_t amount = (size_t)((length - done) > EXT_WORKSPACE_COPY_CHUNK
                                     ? EXT_WORKSPACE_COPY_CHUNK
                                     : (length - done));
        ssize_t got = ld_pread_full(fd, buffer, amount, source_offset + done);
        if (got < 0 || (size_t)got != amount) {
            ext_set_error(error, "short read while copying the EXT safety workspace");
            return -1;
        }
        ssize_t wrote = ld_pwrite_full(fd, buffer, amount, destination_offset + done);
        if (wrote < 0 || (size_t)wrote != amount) {
            ext_set_error(error, "short write while copying the EXT safety workspace");
            return -1;
        }
        done += amount;
        if (allow_stop && ld_stop_requested()) return -2;
    }
    return 0;
}

static int copy_run(int fd, uint32_t block_size, uint64_t source,
                    uint64_t destination, uint64_t blocks,
                    uint64_t batch_blocks, uint8_t *buffer,
                    bool allow_stop, uint64_t *completed,
                    uint64_t total, const char *label, char **error) {
    uint64_t offset = 0;
    while (offset < blocks) {
        uint64_t amount = blocks - offset;
        if (amount > batch_blocks) amount = batch_blocks;
        uint64_t bytes = amount * (uint64_t)block_size;
        int copied = copy_bytes(fd,
            (source + offset) * (uint64_t)block_size,
            (destination + offset) * (uint64_t)block_size,
            bytes, buffer, allow_stop, error);
        if (copied != 0) return copied;
        if (fsync(fd) != 0) {
            ext_set_error(error, "syncing EXT %s failed: %s", label, strerror(errno));
            return -1;
        }
        offset += amount;
        *completed += amount;
        printf("EXT %s: %" PRIu64 " of %" PRIu64 " blocks durable.\n",
               label, *completed, total);
        fflush(stdout);
        if (allow_stop && ld_stop_requested()) return -2;
    }
    return 0;
}

static int copy_workspace_runs(int fd, sqlite3 *db,
                               const ExtWorkspace *workspace,
                               bool restore, bool allow_stop,
                               const char *label, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT original,slot FROM direct_workspace ORDER BY original",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "reading EXT workspace runs");
    uint8_t *buffer = ld_xmalloc(EXT_WORKSPACE_COPY_CHUNK);
    bool have_run = false;
    uint64_t run_source = 0, run_destination = 0, run_blocks = 0;
    uint64_t completed = 0;
    int result = 0;
    int state;
    while ((state = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t original = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t slot = (uint64_t)sqlite3_column_int64(stmt, 1);
        uint64_t source = restore ? slot : original;
        uint64_t destination = restore ? original : slot;
        if (!have_run) {
            run_source = source;
            run_destination = destination;
            run_blocks = 1;
            have_run = true;
        } else if (source == run_source + run_blocks &&
                   destination == run_destination + run_blocks) {
            run_blocks++;
        } else {
            result = copy_run(fd, workspace->block_size, run_source,
                              run_destination, run_blocks,
                              workspace->batch_blocks, buffer, allow_stop,
                              &completed, workspace->blocks, label, error);
            if (result != 0) break;
            run_source = source;
            run_destination = destination;
            run_blocks = 1;
        }
    }
    if (result == 0 && state != SQLITE_DONE)
        result = workspace_sql_error(db, error, "iterating EXT workspace runs");
    if (result == 0 && have_run)
        result = copy_run(fd, workspace->block_size, run_source,
                          run_destination, run_blocks,
                          workspace->batch_blocks, buffer, allow_stop,
                          &completed, workspace->blocks, label, error);
    free(buffer);
    sqlite3_finalize(stmt);
    return result;
}

static int workspace_digest(int fd, sqlite3 *db, uint32_t block_size,
                            bool use_workspace,
                            uint8_t output[SHA256_DIGEST_LENGTH], char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT original,slot FROM direct_workspace ORDER BY original",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT workspace checksum");
    SHA256_CTX digest;
    if (SHA256_Init(&digest) != 1) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "initializing EXT workspace checksum failed");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc(block_size);
    int state;
    while ((state = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t original = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t slot = (uint64_t)sqlite3_column_int64(stmt, 1);
        uint64_t block = use_workspace ? slot : original;
        ssize_t got = ld_pread_full(fd, buffer, block_size,
                                    block * (uint64_t)block_size);
        if (got < 0 || (size_t)got != block_size) {
            free(buffer);
            sqlite3_finalize(stmt);
            ext_set_error(error, "short read while checksumming the EXT workspace");
            return -1;
        }
        uint8_t identity[8];
        for (unsigned byte = 0; byte < 8U; ++byte)
            identity[byte] = (uint8_t)(original >> (byte * 8U));
        if (SHA256_Update(&digest, identity, sizeof(identity)) != 1 ||
            SHA256_Update(&digest, buffer, block_size) != 1) {
            free(buffer);
            sqlite3_finalize(stmt);
            ext_set_error(error, "updating EXT workspace checksum failed");
            return -1;
        }
    }
    free(buffer);
    sqlite3_finalize(stmt);
    if (state != SQLITE_DONE || SHA256_Final(output, &digest) != 1) {
        if (state != SQLITE_DONE)
            return workspace_sql_error(db, error, "reading EXT workspace checksum map");
        ext_set_error(error, "finalizing EXT workspace checksum failed");
        return -1;
    }
    return 0;
}

static int store_workspace_digest(sqlite3 *db,
                                  const uint8_t digest[SHA256_DIGEST_LENGTH],
                                  char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO direct_state(key,value) VALUES ('workspace_sha256',?)",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT workspace checksum state");
    sqlite3_bind_blob(stmt, 1, digest, SHA256_DIGEST_LENGTH, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "recording EXT workspace checksum");
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int verify_stored_workspace_digest(int fd, sqlite3 *db,
                                          uint32_t block_size, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM direct_state WHERE key='workspace_sha256'",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT workspace checksum recovery");
    if (sqlite3_step(stmt) != SQLITE_ROW ||
        sqlite3_column_bytes(stmt, 0) != SHA256_DIGEST_LENGTH) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace recovery checksum is missing");
        return -1;
    }
    uint8_t expected[SHA256_DIGEST_LENGTH];
    memcpy(expected, sqlite3_column_blob(stmt, 0), sizeof(expected));
    sqlite3_finalize(stmt);
    uint8_t actual[SHA256_DIGEST_LENGTH];
    if (workspace_digest(fd, db, block_size, true, actual, error) != 0)
        return -1;
    if (memcmp(expected, actual, sizeof(expected)) != 0) {
        ext_set_error(error, "EXT durable workspace checksum changed; refusing unsafe recovery");
        return -1;
    }
    return 0;
}

int ext_workspace_stage(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error) {
    int result = copy_workspace_runs(fd, db, workspace, false, true,
                                     "workspace staging", error);
    if (result != 0) return result;
    uint8_t original[SHA256_DIGEST_LENGTH];
    uint8_t staged[SHA256_DIGEST_LENGTH];
    if (workspace_digest(fd, db, workspace->block_size, false, original, error) != 0 ||
        workspace_digest(fd, db, workspace->block_size, true, staged, error) != 0)
        return -1;
    if (memcmp(original, staged, sizeof(original)) != 0) {
        ext_set_error(error, "EXT safety workspace verification failed");
        return -1;
    }
    if (store_workspace_digest(db, staged, error) != 0) return -1;
    printf("EXT workspace staging complete: %" PRIu64
           " original allocated blocks durably copied and checksummed; source metadata is still unchanged.\n",
           workspace->blocks);
    fflush(stdout);
    return 0;
}

static int flush_target_run(int fd, const ExtWorkspace *workspace,
                            uint8_t *buffer, uint64_t start,
                            uint64_t count, uint64_t *completed,
                            uint64_t total, char **error) {
    if (count == 0) return 0;
    uint64_t bytes = count * (uint64_t)workspace->block_size;
    ssize_t wrote = ld_pwrite_full(fd, buffer, (size_t)bytes,
                                   start * (uint64_t)workspace->block_size);
    if (wrote < 0 || (uint64_t)wrote != bytes) {
        ext_set_error(error, "short write placing EXT canonical target blocks");
        return -1;
    }
    if (fsync(fd) != 0) {
        ext_set_error(error, "syncing EXT canonical placement failed: %s", strerror(errno));
        return -1;
    }
    *completed += count;
    printf("EXT canonical placement: %" PRIu64 " of %" PRIu64 " moved blocks durable.\n",
           *completed, total);
    fflush(stdout);
    if (ld_stop_requested()) return -2;
    return 0;
}

int ext_workspace_place(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT b.target,w.slot FROM blocks b "
        "JOIN direct_workspace w ON w.original=b.old "
        "WHERE b.old<>b.target ORDER BY b.target";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT canonical workspace placement");
    uint64_t buffer_bytes = workspace->batch_blocks * (uint64_t)workspace->block_size;
    if (buffer_bytes == 0 || buffer_bytes > SIZE_MAX) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace RAM buffer size is invalid");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc((size_t)buffer_bytes);
    uint64_t run_start = 0, run_count = 0, previous_target = 0;
    uint64_t completed = 0, total = 0;
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM blocks WHERE old<>target",
            -1, &count_stmt, NULL) != SQLITE_OK ||
        sqlite3_step(count_stmt) != SQLITE_ROW) {
        sqlite3_finalize(count_stmt);
        free(buffer);
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "counting EXT workspace placements");
    }
    total = (uint64_t)sqlite3_column_int64(count_stmt, 0);
    sqlite3_finalize(count_stmt);

    int result = 0;
    int state;
    while ((state = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t target = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t slot = (uint64_t)sqlite3_column_int64(stmt, 1);
        if (run_count != 0 &&
            (target != previous_target + 1U || run_count == workspace->batch_blocks)) {
            result = flush_target_run(fd, workspace, buffer, run_start, run_count,
                                      &completed, total, error);
            if (result != 0) break;
            run_count = 0;
        }
        if (run_count == 0) run_start = target;
        ssize_t got = ld_pread_full(fd,
            buffer + (size_t)(run_count * (uint64_t)workspace->block_size),
            workspace->block_size,
            slot * (uint64_t)workspace->block_size);
        if (got < 0 || (size_t)got != workspace->block_size) {
            ext_set_error(error, "short read from the EXT durable workspace");
            result = -1;
            break;
        }
        run_count++;
        previous_target = target;
    }
    if (result == 0 && state != SQLITE_DONE)
        result = workspace_sql_error(db, error, "reading EXT canonical workspace placement");
    if (result == 0 && run_count != 0)
        result = flush_target_run(fd, workspace, buffer, run_start, run_count,
                                  &completed, total, error);
    free(buffer);
    sqlite3_finalize(stmt);
    return result;
}

int ext_workspace_restore(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                          char **error) {
    if (verify_stored_workspace_digest(fd, db, workspace->block_size, error) != 0)
        return -1;
    int result = copy_workspace_runs(fd, db, workspace, true, false,
                                     "workspace restore", error);
    if (result == 0) {
        puts("EXT durable workspace restore completed; original allocated blocks are back in place.");
        fflush(stdout);
    }
    return result;
}
'''

Path("gui/filesystems/ext4/native/ext_workspace.h").write_text(workspace_h, encoding="utf-8")
Path("gui/filesystems/ext4/native/ext_workspace.c").write_text(workspace_c, encoding="utf-8")

replace_once(
    "gui/filesystems/ext4/native/ext_native.h",
    '''int ext_apply_mappings(const char *stage, sqlite3 *db, char **error);''',
    '''int ext_apply_mappings(const char *stage, sqlite3 *db, bool allow_stop,
                       char **error);''',
)

replace_once(
    "gui/filesystems/ext4/native/ext_plan.c",
    '''int ext_apply_mappings(const char *stage, sqlite3 *db, char **error) {''',
    '''int ext_apply_mappings(const char *stage, sqlite3 *db, bool allow_stop,
                       char **error) {''',
)
replace_once(
    "gui/filesystems/ext4/native/ext_plan.c",
    '''        if (ld_stop_requested()) { ext_set_error(error, "stop requested before EXT source commit"); sqlite3_finalize(inodes); sqlite3_finalize(mappings); goto done; }''',
    '''        if (allow_stop && ld_stop_requested()) { ext_set_error(error, "stop requested before EXT source commit"); sqlite3_finalize(inodes); sqlite3_finalize(mappings); goto done; }''',
)

replace_once(
    "cmake/project.cmake",
    '''    gui/filesystems/ext4/native/ext_catalog.c\n    gui/filesystems/ext4/native/ext_plan.c)''',
    '''    gui/filesystems/ext4/native/ext_catalog.c\n    gui/filesystems/ext4/native/ext_plan.c\n    gui/filesystems/ext4/native/ext_workspace.c)''',
)

worker = Path("gui/filesystems/ext4/native/ext_worker.c")
text = worker.read_text(encoding="utf-8")
text = text.replace('#include "ext_native.h"\n', '#include "ext_native.h"\n#include "ext_workspace.h"\n', 1)

anchor = '''static int build_and_commit(const char *device, const char *operation,\n                            const char *journal_path, bool live_updates,\n                            char **error) {'''
helper = r'''
static void discard_plan_files(const char *plan) {
    unlink_if_exists(plan);
    if (plan != NULL) {
        char *wal = ld_path_append_suffix(plan, "-wal");
        char *shm = ld_path_append_suffix(plan, "-shm");
        unlink_if_exists(wal);
        unlink_if_exists(shm);
        free(wal);
        free(shm);
    }
}

static int validate_restored_ext(const char *device, char **error) {
    ext2_filsys fs = NULL;
    if (ext_open_fs(device, false, &fs, error) != 0) return -1;
    int result = ext_validate_metadata(fs, true, error);
    (void)ext2fs_close(fs);
    return result;
}

/* Return 0/130 for a completed or stopped direct operation, 2 when the complete
   original allocation set cannot fit in a safe high-address workspace and the
   verified working-image fallback should be used, or -1 for a hard failure. */
static int try_workspace_relayout(const char *device, const char *operation,
                                  const char *journal_path, bool live_updates,
                                  uint64_t requested_batch_blocks,
                                  ExtJournal *state,
                                  const ExtGeometry *source_geometry,
                                  char **error) {
    ext2_filsys fs = NULL;
    sqlite3 *db = NULL;
    int fd = -1;
    ExtWorkspace workspace = {0};
    bool growth = strcmp(operation, "growth-defrag") == 0;
    bool source_touched = false;
    int result = -1;

    if (ext_open_plan_db(state->plan, true, &db, error) != 0) goto fail_clean;
    if (ext_open_fs(device, false, &fs, error) != 0) goto fail_clean;
    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ext_set_error(error, "cannot open EXT source for direct relayout: %s", strerror(errno));
        goto fail_clean;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ext_set_error(error, "cannot lock EXT source for direct relayout: %s", strerror(errno));
        goto fail_clean;
    }
    if (ext_catalog_plan(fs, fd, db, source_geometry,
                         &state->movable_blocks, error) != 0 ||
        ext_assign_targets(fs, db, source_geometry, growth, error) != 0 ||
        ext_plan_move_count(db, &state->move_blocks, error) != 0)
        goto fail_clean;

    if (state->move_blocks == 0) {
        ExtCatalogue current = {0};
        ExtGeometry current_geometry;
        if (ext_scan_catalogue(device, &current_geometry, &current, error) != 0)
            goto fail_clean;
        bool okay = !growth || current.growth_10_satisfied;
        ext_catalogue_free(&current);
        if (okay) {
            (void)ext2fs_close(fs); fs = NULL;
            (void)flock(fd, LOCK_UN); close(fd); fd = -1;
            sqlite3_close(db); db = NULL;
            transaction_cleanup(journal_path, state);
            puts("Not needed; canonical EXT layout already verified.");
            emit_result(operation, "not-needed", "");
            return 0;
        }
        goto fallback;
    }

    int prepared = ext_workspace_prepare(fs, db, source_geometry,
                                         requested_batch_blocks,
                                         &workspace, error);
    if (prepared == EXT_WORKSPACE_UNAVAILABLE) goto fallback;
    if (prepared != 0) goto fail_clean;
    (void)ext2fs_close(fs); fs = NULL;

    if (journal_phase(journal_path, state, "direct-staging", error) != 0)
        goto fail_clean;
    printf("EXT relayout preflight: %" PRIu64 " movable allocation blocks; %" PRIu64
           " require relocation; canonical reserve policy %s.\n",
           state->movable_blocks, state->move_blocks, growth ? "10%" : "0%");
    printf("EXT phase 1: staging the complete original allocated set into a %" PRIu64
           "-block high-address safety workspace beginning at block %" PRIu64 ".\n",
           workspace.blocks, workspace.start);
    printf("EXT adaptive transaction budget: up to %" PRIu64
           " blocks (%.1f MiB) per durable copy boundary.\n",
           workspace.batch_blocks,
           (double)(workspace.batch_blocks * (uint64_t)workspace.block_size) /
               (1024.0 * 1024.0));
    fflush(stdout);

    int staged = ext_workspace_stage(fd, db, &workspace, error);
    if (staged == -2 || ld_stop_requested()) {
        (void)flock(fd, LOCK_UN); close(fd); fd = -1;
        sqlite3_close(db); db = NULL;
        transaction_cleanup(journal_path, state);
        puts("Stop requested during EXT workspace staging; original filesystem metadata and payload remain unchanged.");
        emit_result(operation, "stopped", "");
        return 130;
    }
    if (staged != 0) goto fail_clean;
    if (journal_phase(journal_path, state, "direct-staged", error) != 0)
        goto fail_clean;
    if (ld_stop_requested()) {
        (void)flock(fd, LOCK_UN); close(fd); fd = -1;
        sqlite3_close(db); db = NULL;
        transaction_cleanup(journal_path, state);
        puts("Stop requested after EXT workspace staging; original filesystem remains unchanged.");
        emit_result(operation, "stopped", "");
        return 130;
    }
    if (check_unchanged_target(device, state, source_geometry, error) != 0)
        goto fail_clean;

    if (journal_phase(journal_path, state, "direct-placing", error) != 0)
        goto fail_clean;
    source_touched = true;
    puts("EXT phase 2: placing the canonical layout directly from the durable safety workspace; no filesystem-sized working image is used.");
    fflush(stdout);
    int placed = ext_workspace_place(fd, db, &workspace, error);
    if (placed == -2 || ld_stop_requested()) goto stop_restore;
    if (placed != 0) goto fail_restore;

    if (journal_phase(journal_path, state, "direct-metadata", error) != 0)
        goto fail_restore;
    puts("EXT phase 3: committing canonical inode mappings, allocation bitmaps and filesystem metadata.");
    fflush(stdout);
    /* Once direct metadata mutation begins it is intentionally uninterruptible;
       Stop is honoured at the next complete metadata boundary. */
    if (ext_apply_mappings(device, db, false, error) != 0) goto fail_restore;

    if (journal_phase(journal_path, state, "direct-verifying", error) != 0)
        goto fail_restore;
    ExtCatalogue committed = {0};
    if (ext_verify_stage(device, db, source_geometry, growth,
                         &committed, error) != 0) {
        ext_catalogue_free(&committed);
        goto fail_restore;
    }
    printf("Layout verification:      %" PRIu64 " files and %" PRIu64
           " directories contiguous; canonical %s policy verified.\n",
           committed.regular_files, committed.directories, growth ? "10%" : "0%");
    if (live_updates) emit_live_reset(source_geometry, &committed);
    ext_catalogue_free(&committed);

    printf("EXT unified workspace layout: %" PRIu64
           " original allocated blocks protected, %" PRIu64
           " relocated directly, without a filesystem-sized working image.\n",
           workspace.blocks, state->move_blocks);
    printf("%s %s completed with UUID and active filesystem capacity preserved.\n",
           source_geometry->filesystem,
           growth ? "Growth Defrag" : "Defragment");
    fflush(stdout);
    (void)flock(fd, LOCK_UN); close(fd); fd = -1;
    sqlite3_close(db); db = NULL;
    transaction_cleanup(journal_path, state);
    if (ld_stop_requested()) {
        emit_result(operation, "stopped", "");
        return 130;
    }
    emit_result(operation, "completed", "");
    return 0;

stop_restore:
    {
        char *restore_error = NULL;
        if (ext_workspace_restore(fd, db, &workspace, &restore_error) != 0 ||
            validate_restored_ext(device, &restore_error) != 0) {
            free(*error); *error = restore_error;
            goto keep_recovery;
        }
        free(restore_error);
    }
    source_touched = false;
    (void)flock(fd, LOCK_UN); close(fd); fd = -1;
    sqlite3_close(db); db = NULL;
    transaction_cleanup(journal_path, state);
    puts("Growth/Defrag Stop restored the original EXT allocation from the durable workspace at a complete transaction boundary.");
    emit_result(operation, "stopped", "");
    return 130;

fail_restore:
    if (source_touched) {
        char *restore_error = NULL;
        if (ext_workspace_restore(fd, db, &workspace, &restore_error) != 0 ||
            validate_restored_ext(device, &restore_error) != 0) {
            free(*error); *error = restore_error;
            goto keep_recovery;
        }
        free(restore_error);
        source_touched = false;
    }
    goto fail_clean;

fallback:
    if (fs != NULL) { (void)ext2fs_close(fs); fs = NULL; }
    if (fd >= 0) { (void)flock(fd, LOCK_UN); close(fd); fd = -1; }
    if (db != NULL) { sqlite3_close(db); db = NULL; }
    discard_plan_files(state->plan);
    puts("EXT complete live allocation set does not fit a safe high-address workspace; using the verified working-image fallback.");
    fflush(stdout);
    return 2;

fail_clean:
    if (fs != NULL) (void)ext2fs_close(fs);
    if (fd >= 0) { (void)flock(fd, LOCK_UN); close(fd); }
    if (db != NULL) sqlite3_close(db);
    transaction_cleanup(journal_path, state);
    return -1;

keep_recovery:
    if (fs != NULL) (void)ext2fs_close(fs);
    if (fd >= 0) { (void)flock(fd, LOCK_UN); close(fd); }
    if (db != NULL) sqlite3_close(db);
    puts("EXT direct relayout needs Recover; the durable workspace and plan have been retained.");
    fflush(stdout);
    return -1;
}

'''
if text.count(anchor) != 1:
    raise SystemExit(f"worker build_and_commit anchor count {text.count(anchor)}")
text = text.replace(anchor, helper + anchor, 1)

text = text.replace(
    '''static int build_and_commit(const char *device, const char *operation,\n                            const char *journal_path, bool live_updates,\n                            char **error) {''',
    '''static int build_and_commit(const char *device, const char *operation,\n                            const char *journal_path, bool live_updates,\n                            uint64_t batch_blocks, char **error) {''',
    1,
)
old = '''    state.physical_bytes = physical_bytes; state.filesystem_bytes = source_geometry.total_blocks * source_geometry.block_size;\n    if (capacity_preflight(journal_path, &source_geometry, error) != 0 || journal_save(journal_path, &state, error) != 0) goto done;\n    printf("Raw userspace native-C %s engine %s\\n", source_geometry.filesystem, LD_VERSION); fflush(stdout);\n    if (ld_stop_requested()) goto stopped;\n    if (journal_phase(journal_path, &state, "cloning", error) != 0) goto precommit_fail;'''
new = '''    state.physical_bytes = physical_bytes; state.filesystem_bytes = source_geometry.total_blocks * source_geometry.block_size;\n    if (journal_save(journal_path, &state, error) != 0) goto done;\n    printf("Raw userspace native-C %s relayout engine %s\\n", source_geometry.filesystem, LD_VERSION); fflush(stdout);\n    if (ld_stop_requested()) goto stopped;\n    int direct = try_workspace_relayout(device, operation, journal_path, live_updates,\n                                        batch_blocks, &state, &source_geometry, error);\n    if (direct != 2) { result = direct < 0 ? 1 : direct; goto done; }\n    if (capacity_preflight(journal_path, &source_geometry, error) != 0) goto precommit_fail;\n    if (journal_phase(journal_path, &state, "cloning", error) != 0) goto precommit_fail;'''
if text.count(old) != 1:
    raise SystemExit("worker direct insertion anchor not unique")
text = text.replace(old, new, 1)
text = text.replace(
    '''        ext_apply_mappings(state.stage, db, error) != 0) goto precommit_fail;''',
    '''        ext_apply_mappings(state.stage, db, true, error) != 0) goto precommit_fail;''',
    1,
)

recover_anchor = '''    free(real); free(identity);\n    if (strcmp(state.phase, "commit") != 0 && strcmp(state.phase, "verifying-source") != 0) {'''
recover_insert = r'''    free(real); free(identity);
    if (strncmp(state.phase, "direct-", 7) == 0) {
        sqlite3 *direct_db = NULL;
        if (ext_open_plan_db(state.plan, false, &direct_db, error) != 0) {
            journal_free(&state);
            return 1;
        }
        if (strcmp(state.phase, "direct-staging") == 0 ||
            strcmp(state.phase, "direct-staged") == 0) {
            sqlite3_close(direct_db);
            transaction_cleanup(journal_path, &state);
            puts("Discarded an incomplete EXT workspace stage; source filesystem was unchanged.");
            journal_free(&state);
            emit_result("recover", "completed", "");
            return 0;
        }
        ExtWorkspace workspace;
        if (ext_workspace_load(direct_db, &workspace, error) != 0) {
            sqlite3_close(direct_db);
            journal_free(&state);
            return 1;
        }
        if (strcmp(state.phase, "direct-verifying") == 0) {
            ExtGeometry current_geometry;
            ExtCatalogue verified = {0};
            if (ext_read_geometry(device, &current_geometry, error) == 0 &&
                ext_verify_stage(device, direct_db, &current_geometry,
                                 strcmp(state.operation, "growth-defrag") == 0,
                                 &verified, error) == 0) {
                ext_catalogue_free(&verified);
                sqlite3_close(direct_db);
                transaction_cleanup(journal_path, &state);
                puts("EXT direct workspace recovery verified the completed canonical layout.");
                journal_free(&state);
                emit_result("recover", "completed", "");
                return 0;
            }
            ext_catalogue_free(&verified);
            free(*error); *error = NULL;
        }
        int direct_fd = open(device, O_RDWR | O_CLOEXEC);
        if (direct_fd < 0 || flock(direct_fd, LOCK_EX | LOCK_NB) != 0) {
            if (direct_fd >= 0) close(direct_fd);
            sqlite3_close(direct_db);
            ext_set_error(error, "cannot lock EXT source for direct workspace recovery: %s", strerror(errno));
            journal_free(&state);
            return 1;
        }
        if (ext_workspace_restore(direct_fd, direct_db, &workspace, error) != 0 ||
            validate_restored_ext(device, error) != 0) {
            (void)flock(direct_fd, LOCK_UN); close(direct_fd);
            sqlite3_close(direct_db);
            journal_free(&state);
            return 1;
        }
        (void)flock(direct_fd, LOCK_UN); close(direct_fd);
        sqlite3_close(direct_db);
        transaction_cleanup(journal_path, &state);
        puts("EXT direct workspace recovery restored the exact original allocated filesystem state.");
        journal_free(&state);
        emit_result("recover", "completed", "");
        return 0;
    }
    if (strcmp(state.phase, "commit") != 0 && strcmp(state.phase, "verifying-source") != 0) {'''
if text.count(recover_anchor) != 1:
    raise SystemExit("worker recovery anchor not unique")
text = text.replace(recover_anchor, recover_insert, 1)

old_parser = '''    bool write = false, live_updates = false; int growth_percent = 10;'''
new_parser = '''    bool write = false, live_updates = false; int growth_percent = 10;\n    uint64_t batch_blocks = 0;'''
if text.count(old_parser) != 1:
    raise SystemExit("worker parser declaration anchor not unique")
text = text.replace(old_parser, new_parser, 1)
old_opts = '''        else if (strcmp(argv[index], "--live-map-cells") == 0 && index + 1 < argc) { live_updates = atoi(argv[++index]) > 0; }\n        else if ((strcmp(argv[index], "--workers") == 0 || strcmp(argv[index], "--ram-buffer") == 0 || strcmp(argv[index], "--batch-clusters") == 0) && index + 1 < argc) { index++; }'''
new_opts = '''        else if (strcmp(argv[index], "--live-map-cells") == 0 && index + 1 < argc) { live_updates = atoi(argv[++index]) > 0; }\n        else if (strcmp(argv[index], "--batch-clusters") == 0 && index + 1 < argc) {\n            if (parse_u64(argv[++index], &batch_blocks) != 0 || batch_blocks == 0) {\n                fprintf(stderr, "%s: --batch-clusters requires a positive integer\\n", PROGRAM_NAME); return 2;\n            }\n        }\n        else if ((strcmp(argv[index], "--workers") == 0 || strcmp(argv[index], "--ram-buffer") == 0) && index + 1 < argc) { index++; }'''
if text.count(old_opts) != 1:
    raise SystemExit("worker option parser anchor not unique")
text = text.replace(old_opts, new_opts, 1)
text = text.replace(
    '''        result = build_and_commit(device, operation, journal, live_updates, &error);''',
    '''        result = build_and_commit(device, operation, journal, live_updates, batch_blocks, &error);''',
    1,
)
worker.write_text(text, encoding="utf-8")

# Make the end-to-end EXT fixture require the new direct workspace path.
test_path = Path("tests/test_native_top3.py")
tests = test_path.read_text(encoding="utf-8")
old_ext = r'''    output = mutate(worker, image, "defrag", work / "ext-defrag.journal")
    match = re.search(
        r"EXT source commit: writing ([0-9.]+) MB of verified allocated blocks "
        r"instead of rewriting the full ([0-9.]+) MB filesystem\.",
        output,
    )
    assert match is not None, output
    committed_mb, full_mb = map(float, match.groups())
    assert committed_mb < full_mb * 0.50, (committed_mb, full_mb, output)
    packed = run_json(worker, image)
    assert packed["uuid"] == identity
    assert_clean(packed, growth=False)
    output = mutate(worker, image, "growth-defrag", work / "ext-growth.journal")
    match = re.search(
        r"EXT source commit: writing ([0-9.]+) MB of verified allocated blocks "
        r"instead of rewriting the full ([0-9.]+) MB filesystem\.",
        output,
    )
    assert match is not None, output
    committed_mb, full_mb = map(float, match.groups())
    assert committed_mb < full_mb * 0.50, (committed_mb, full_mb, output)
    grown = run_json(worker, image)
'''
new_ext = r'''    output = mutate(worker, image, "defrag", work / "ext-defrag.journal")
    assert "EXT unified workspace layout:" in output, output
    assert "EXT workspace staging complete:" in output, output
    assert "internally verified EXT working image" not in output, output
    packed = run_json(worker, image)
    assert packed["uuid"] == identity
    assert_clean(packed, growth=False)
    output = mutate(worker, image, "growth-defrag", work / "ext-growth.journal")
    assert "EXT unified workspace layout:" in output, output
    assert "EXT workspace staging complete:" in output, output
    assert "internally verified EXT working image" not in output, output
    grown = run_json(worker, image)
'''
if tests.count(old_ext) != 1:
    raise SystemExit("test_native_top3 EXT anchor not unique")
tests = tests.replace(old_ext, new_ext, 1)
test_path.write_text(tests, encoding="utf-8")

print("EXT direct durable-workspace patch applied")
