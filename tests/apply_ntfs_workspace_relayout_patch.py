#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the native-C NTFS durable terminal-workspace relayout candidate."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, found {count}: {old[:80]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


header = ROOT / "gui/filesystems/ntfs/native/ntfs_native.h"
replace_once(
    header,
    "int ntfs_apply_stage_metadata(const char *stage, sqlite3 *db, char **error);\n"
    "int ntfs_verify_stage(const char *stage, sqlite3 *db, bool growth,\n"
    "                      bool allow_dirty, char **error);\n",
    "int ntfs_apply_stage_metadata(const char *stage, sqlite3 *db, bool allow_dirty, char **error);\n"
    "int ntfs_prepare_workspace_map(sqlite3 *db, uint64_t workspace_start,\n"
    "                               uint64_t workspace_clusters, char **error);\n"
    "int ntfs_stage_workspace(const char *device, sqlite3 *db, uint32_t cluster_size, char **error);\n"
    "int ntfs_verify_workspace(const char *device, sqlite3 *db, uint32_t cluster_size, char **error);\n"
    "int ntfs_place_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,\n"
    "                         bool stop_aware, char **error);\n"
    "int ntfs_restore_workspace(const char *device, sqlite3 *db, uint32_t cluster_size, char **error);\n"
    "int ntfs_verify_stage(const char *stage, sqlite3 *db, bool growth,\n"
    "                      bool allow_dirty, char **error);\n",
)

plan = ROOT / "gui/filesystems/ntfs/native/ntfs_plan.c"
replace_once(
    plan,
    "int ntfs_apply_stage_metadata(const char *stage,sqlite3 *db,char **error){NtfsVolume volume;NtfsLayout layout;NtfsCatalogue catalogue;if(ntfs_open_volume(stage,true,&volume,error)<0)return -1;if(ntfs_read_layout(&volume,false,&layout,error)<0){ntfs_close_volume(&volume);return -1;}",
    "int ntfs_apply_stage_metadata(const char *stage,sqlite3 *db,bool allow_dirty,char **error){NtfsVolume volume;NtfsLayout layout;NtfsCatalogue catalogue;if(ntfs_open_volume(stage,true,&volume,error)<0)return -1;if(ntfs_read_layout(&volume,allow_dirty,&layout,error)<0){ntfs_close_volume(&volume);return -1;}",
)

workspace_code = r'''

static int workspace_digest(const uint8_t *data, size_t length,
                            uint8_t digest[SHA256_DIGEST_LENGTH], char **error) {
    unsigned int digest_length = 0;
    if (EVP_Digest(data, length, digest, &digest_length, EVP_sha256(), NULL) != 1 ||
        digest_length != SHA256_DIGEST_LENGTH) {
        ntfs_set_error(error, "computing NTFS workspace cluster digest failed");
        return -1;
    }
    return 0;
}

int ntfs_prepare_workspace_map(sqlite3 *db, uint64_t workspace_start,
                               uint64_t workspace_clusters, char **error) {
    if (workspace_clusters == 0 ||
        workspace_start > UINT64_MAX - workspace_clusters) {
        ntfs_set_error(error, "NTFS terminal workspace geometry is invalid");
        return -1;
    }
    if (sql_exec(db,
                 "CREATE TABLE IF NOT EXISTS workspace("
                 "old INTEGER PRIMARY KEY,target INTEGER NOT NULL UNIQUE,"
                 "slot INTEGER NOT NULL UNIQUE,sha BLOB);"
                 "BEGIN IMMEDIATE;DELETE FROM workspace",
                 error) != 0) return -1;
    sqlite3_stmt *select = NULL, *insert = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT old,target FROM blocks WHERE old<>target ORDER BY old", -1,
            &select, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO workspace(old,target,slot,sha) VALUES (?,?,?,NULL)", -1,
            &insert, NULL) != SQLITE_OK) {
        sql_error(db, error, "preparing NTFS terminal workspace map");
        goto fail;
    }
    uint64_t index = 0;
    while (sqlite3_step(select) == SQLITE_ROW) {
        if (index >= workspace_clusters) {
            ntfs_set_error(error, "NTFS terminal workspace is smaller than the relocation set");
            goto fail;
        }
        uint64_t old_cluster = (uint64_t)sqlite3_column_int64(select, 0);
        uint64_t target_cluster = (uint64_t)sqlite3_column_int64(select, 1);
        uint64_t slot = workspace_start + index;
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        sqlite3_bind_int64(insert, 1, (sqlite3_int64)old_cluster);
        sqlite3_bind_int64(insert, 2, (sqlite3_int64)target_cluster);
        sqlite3_bind_int64(insert, 3, (sqlite3_int64)slot);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            sql_error(db, error, "recording NTFS terminal workspace map");
            goto fail;
        }
        index++;
    }
    if (index != workspace_clusters) {
        ntfs_set_error(error,
                       "NTFS terminal workspace map contains %llu clusters; expected %llu",
                       (unsigned long long)index,
                       (unsigned long long)workspace_clusters);
        goto fail;
    }
    sqlite3_finalize(select);
    sqlite3_finalize(insert);
    if (sql_exec(db, "COMMIT;PRAGMA wal_checkpoint(FULL)", error) != 0) return -1;
    return 0;
fail:
    sqlite3_finalize(select);
    sqlite3_finalize(insert);
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

static int workspace_copy_rows(const char *device, sqlite3 *db, uint32_t cluster_size,
                               const char *query, bool stop_aware,
                               bool record_digest, char **error) {
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ntfs_set_error(error, "cannot open NTFS source for terminal-workspace I/O: %s",
                       strerror(errno));
        return -1;
    }
    sqlite3_stmt *rows = NULL, *update = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &rows, NULL) != SQLITE_OK) {
        close(fd);
        return sql_error(db, error, "reading NTFS terminal workspace map");
    }
    if (record_digest) {
        if (sql_exec(db, "BEGIN IMMEDIATE", error) != 0) {
            sqlite3_finalize(rows); close(fd); return -1;
        }
        if (sqlite3_prepare_v2(db, "UPDATE workspace SET sha=? WHERE old=?", -1,
                              &update, NULL) != SQLITE_OK) {
            sql_error(db, error, "preparing NTFS workspace digest update");
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            sqlite3_finalize(rows); close(fd); return -1;
        }
    }
    uint8_t *buffer = ld_xmalloc(cluster_size);
    uint64_t copied = 0;
    int result = 0;
    while (sqlite3_step(rows) == SQLITE_ROW) {
        if (stop_aware && ld_stop_requested()) { result = -2; break; }
        uint64_t source_cluster = (uint64_t)sqlite3_column_int64(rows, 0);
        uint64_t target_cluster = (uint64_t)sqlite3_column_int64(rows, 1);
        uint64_t source_offset = source_cluster * (uint64_t)cluster_size;
        uint64_t target_offset = target_cluster * (uint64_t)cluster_size;
        ssize_t got = ld_pread_full(fd, buffer, cluster_size, source_offset);
        if (got < 0 || (size_t)got != cluster_size) {
            ntfs_set_error(error, "short read during NTFS terminal-workspace relocation");
            result = -1; break;
        }
        if (record_digest) {
            uint8_t digest[SHA256_DIGEST_LENGTH];
            if (workspace_digest(buffer, cluster_size, digest, error) != 0) {
                result = -1; break;
            }
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
            sqlite3_bind_blob(update, 1, digest, SHA256_DIGEST_LENGTH, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update, 2, (sqlite3_int64)source_cluster);
            if (sqlite3_step(update) != SQLITE_DONE) {
                sql_error(db, error, "recording NTFS workspace cluster digest");
                result = -1; break;
            }
        }
        ssize_t wrote = ld_pwrite_full(fd, buffer, cluster_size, target_offset);
        if (wrote < 0 || (size_t)wrote != cluster_size) {
            ntfs_set_error(error, "short write during NTFS terminal-workspace relocation");
            result = -1; break;
        }
        copied++;
    }
    if (result == 0 && fsync(fd) != 0) {
        ntfs_set_error(error, "syncing NTFS terminal-workspace relocation failed: %s",
                       strerror(errno));
        result = -1;
    }
    if (record_digest) {
        if (result == 0) {
            if (sql_exec(db, "COMMIT;PRAGMA wal_checkpoint(FULL)", error) != 0) result = -1;
        } else {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        }
    }
    sqlite3_finalize(update);
    sqlite3_finalize(rows);
    free(buffer);
    close(fd);
    (void)copied;
    return result;
}

int ntfs_stage_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                         char **error) {
    return workspace_copy_rows(device, db, cluster_size,
                               "SELECT old,slot FROM workspace ORDER BY old",
                               true, true, error);
}

int ntfs_place_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                         bool stop_aware, char **error) {
    return workspace_copy_rows(device, db, cluster_size,
                               "SELECT slot,target FROM workspace ORDER BY old",
                               stop_aware, false, error);
}

int ntfs_restore_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                           char **error) {
    return workspace_copy_rows(device, db, cluster_size,
                               "SELECT slot,old FROM workspace ORDER BY old",
                               false, false, error);
}

int ntfs_verify_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                          char **error) {
    int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ntfs_set_error(error, "cannot open NTFS source to verify terminal workspace: %s",
                       strerror(errno));
        return -1;
    }
    sqlite3_stmt *rows = NULL;
    if (sqlite3_prepare_v2(db, "SELECT slot,sha FROM workspace ORDER BY old", -1,
                          &rows, NULL) != SQLITE_OK) {
        close(fd);
        return sql_error(db, error, "reading NTFS workspace verification map");
    }
    uint8_t *buffer = ld_xmalloc(cluster_size);
    uint64_t verified = 0;
    int result = 0;
    while (sqlite3_step(rows) == SQLITE_ROW) {
        uint64_t slot = (uint64_t)sqlite3_column_int64(rows, 0);
        const void *expected = sqlite3_column_blob(rows, 1);
        int expected_bytes = sqlite3_column_bytes(rows, 1);
        if (expected == NULL || expected_bytes != SHA256_DIGEST_LENGTH) {
            ntfs_set_error(error, "NTFS terminal workspace is not fully checksummed");
            result = -1; break;
        }
        ssize_t got = ld_pread_full(fd, buffer, cluster_size,
                                    slot * (uint64_t)cluster_size);
        uint8_t actual[SHA256_DIGEST_LENGTH];
        if (got < 0 || (size_t)got != cluster_size ||
            workspace_digest(buffer, cluster_size, actual, error) != 0) {
            if (error != NULL && *error == NULL)
                ntfs_set_error(error, "short read while verifying NTFS terminal workspace");
            result = -1; break;
        }
        if (memcmp(actual, expected, SHA256_DIGEST_LENGTH) != 0) {
            ntfs_set_error(error, "NTFS terminal workspace checksum mismatch at cluster %llu",
                           (unsigned long long)slot);
            result = -1; break;
        }
        verified++;
    }
    sqlite3_finalize(rows);
    free(buffer);
    close(fd);
    if (result == 0 && verified == 0) {
        ntfs_set_error(error, "NTFS terminal workspace verification found no staged clusters");
        result = -1;
    }
    return result;
}
'''
plan_text = plan.read_text(encoding="utf-8")
marker = "static NtfsStream *find_stream(NtfsCatalogue *catalogue,uint64_t record,uint32_t offset)"
if plan_text.count(marker) != 1:
    raise SystemExit("ntfs_plan.c: workspace insertion marker not unique")
plan.write_text(plan_text.replace(marker, workspace_code + "\n" + marker, 1), encoding="utf-8")

worker = ROOT / "gui/filesystems/ntfs/native/ntfs_worker.c"
replace_once(
    worker,
    "    uint64_t commit_cluster;\n    uint64_t move_clusters;\n} NtfsJournal;",
    "    uint64_t commit_cluster;\n    uint64_t move_clusters;\n"
    "    uint64_t workspace_start;\n    uint64_t workspace_clusters;\n} NtfsJournal;",
)
replace_once(
    worker,
    "    fprintf(file, \"move_clusters=%\" PRIu64 \"\\n\", state->move_clusters);\n",
    "    fprintf(file, \"move_clusters=%\" PRIu64 \"\\n\", state->move_clusters);\n"
    "    fprintf(file, \"workspace_start=%\" PRIu64 \"\\n\", state->workspace_start);\n"
    "    fprintf(file, \"workspace_clusters=%\" PRIu64 \"\\n\", state->workspace_clusters);\n",
)
replace_once(
    worker,
    "        else if (strcmp(line, \"move_clusters\") == 0 && parse_u64(equals, &state->move_clusters) != 0) goto invalid;\n",
    "        else if (strcmp(line, \"move_clusters\") == 0 && parse_u64(equals, &state->move_clusters) != 0) goto invalid;\n"
    "        else if (strcmp(line, \"workspace_start\") == 0 && parse_u64(equals, &state->workspace_start) != 0) goto invalid;\n"
    "        else if (strcmp(line, \"workspace_clusters\") == 0 && parse_u64(equals, &state->workspace_clusters) != 0) goto invalid;\n",
)
replace_once(
    worker,
    "    if (ntfs_apply_stage_metadata(state.stage, db, error) != 0) goto precommit_fail;",
    "    if (ntfs_apply_stage_metadata(state.stage, db, false, error) != 0) goto precommit_fail;",
)

worker_helpers = r'''

static bool snapshot_bitmap_bit(const uint8_t *bitmap, size_t bitmap_bytes, uint64_t cluster) {
    if (cluster >= bitmap_bytes * 8U) return true;
    return (bitmap[cluster >> 3] & (uint8_t)(1U << (cluster & 7U))) != 0;
}

static bool find_terminal_workspace(const uint8_t *original_bitmap, size_t original_bytes,
                                    const NtfsLayout *planned, uint64_t total_clusters,
                                    uint64_t needed, uint64_t *workspace_start) {
    if (needed == 0 || total_clusters < 4U) return false;
    uint64_t end = total_clusters - 1U; /* keep the final NTFS backup-boot cluster untouched */
    uint64_t cursor = end;
    while (cursor > 1U) {
        uint64_t cluster = cursor - 1U;
        if (snapshot_bitmap_bit(original_bitmap, original_bytes, cluster) ||
            ntfs_bitmap_bit(planned, cluster) != 0) break;
        cursor--;
    }
    uint64_t available = end - cursor;
    if (available < needed) return false;
    *workspace_start = end - needed;
    return true;
}

static void emit_committed_live_reset(const char *device, char **error) {
    NtfsVolume committed;
    NtfsLayout committed_layout;
    memset(&committed, 0, sizeof(committed));
    committed.fd = -1;
    memset(&committed_layout, 0, sizeof(committed_layout));
    if (ntfs_open_volume(device, false, &committed, error) == 0 &&
        ntfs_read_layout(&committed, false, &committed_layout, error) == 0) {
        emit_live_reset(&committed, &committed_layout);
        ntfs_layout_free(&committed_layout);
        ntfs_close_volume(&committed);
    }
}

/* Return 0 when completed/not-needed, 130 when stopped, 2 when the safe
   terminal-workspace fast path cannot be used and the verified image fallback
   should run, or -1 for a hard failure. */
static int try_terminal_workspace_relayout(const char *device, const char *operation,
                                           const char *journal_path, bool live_updates,
                                           NtfsJournal *state, NtfsVolume *source,
                                           NtfsLayout *source_layout,
                                           NtfsCatalogue *source_catalogue,
                                           char **error) {
    bool growth = strcmp(operation, "growth-defrag") == 0;
    uint8_t *original_bitmap = ld_xmalloc(source_layout->bitmap_bytes);
    memcpy(original_bitmap, source_layout->bitmap, source_layout->bitmap_bytes);
    NtfsPlacementVec placements = {0};
    sqlite3 *db = NULL;
    int lock_fd = -1;
    int result = -1;

    if (ntfs_plan_layout(source_layout, source_catalogue, source->total_clusters,
                         growth, &placements, error) != 0) {
        if (error != NULL && *error != NULL && strstr(*error, "no supported movable") != NULL &&
            source_catalogue->fragmented_files == 0 &&
            source_catalogue->fragmented_directories == 0 &&
            (!growth || source_catalogue->growth_10_satisfied)) {
            free(*error); *error = NULL;
            transaction_cleanup(journal_path, state);
            puts("Not needed; canonical NTFS layout already verified.");
            emit_result(operation, "not-needed", "");
            result = 0;
        }
        goto done;
    }
    if (ntfs_create_plan_db(state->plan, source, source_layout, source_catalogue,
                            &placements, growth, &db, error) != 0) goto done;
    state->move_clusters = ntfs_plan_move_count(db, error);
    if (error != NULL && *error != NULL) goto done;
    if (state->move_clusters == 0) {
        /* No payload movement is necessary. Let the mature fallback preserve
           legacy behaviour for unusual metadata-only plans. */
        result = 2;
        goto fallback;
    }
    uint64_t workspace_start = 0;
    if (!find_terminal_workspace(original_bitmap, source_layout->bitmap_bytes,
                                 source_layout, source->total_clusters,
                                 state->move_clusters, &workspace_start)) {
        result = 2;
        goto fallback;
    }
    state->workspace_start = workspace_start;
    state->workspace_clusters = state->move_clusters;
    if (ntfs_prepare_workspace_map(db, workspace_start, state->workspace_clusters,
                                   error) != 0) goto done;

    lock_fd = open(device, O_RDWR | O_CLOEXEC);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        ntfs_set_error(error, "cannot lock NTFS source for direct relayout: %s", strerror(errno));
        goto done;
    }

    printf("Raw userspace native-C NTFS relayout engine %s\n", LD_VERSION);
    printf("NTFS direct relayout: %zu supported streams; %llu clusters require relocation.\n",
           placements.count, (unsigned long long)state->move_clusters);
    if (placements.fixed_streams != 0)
        printf("Preserving %llu unsupported-but-safe NTFS user stream%s in place as fixed allocation obstacle%s.\n",
               (unsigned long long)placements.fixed_streams,
               placements.fixed_streams == 1U ? "" : "s",
               placements.fixed_streams == 1U ? "" : "s");
    fflush(stdout);

    if (ld_stop_requested()) { result = 130; goto stopped_unchanged; }
    if (journal_phase(journal_path, state, "workspace-staging", error) != 0) goto done;
    printf("NTFS phase 1: staging %llu moved clusters into the durable terminal safety workspace at cluster %llu.\n",
           (unsigned long long)state->workspace_clusters,
           (unsigned long long)state->workspace_start);
    fflush(stdout);
    int stage_result = ntfs_stage_workspace(device, db, source->cluster_size, error);
    if (stage_result == -2 || ld_stop_requested()) { result = 130; goto stopped_unchanged; }
    if (stage_result != 0) goto done;
    if (ntfs_verify_workspace(device, db, source->cluster_size, error) != 0) goto done;
    if (journal_phase(journal_path, state, "workspace-staged", error) != 0) goto done;
    printf("NTFS workspace staging complete: %llu clusters durably copied and checksummed; source metadata is still unchanged.\n",
           (unsigned long long)state->workspace_clusters);
    fflush(stdout);
    if (ld_stop_requested()) { result = 130; goto stopped_unchanged; }
    if (check_unchanged_target(device, state, error) != 0) goto done;
    if (mark_source_dirty(device, error) != 0) goto done;
    if (journal_phase(journal_path, state, "workspace-placing", error) != 0) goto recover_required;

    puts("NTFS phase 2: placing the canonical layout directly from the durable terminal workspace.");
    fflush(stdout);
    int place_result = ntfs_place_workspace(device, db, source->cluster_size, true, error);
    if (place_result == -2 || ld_stop_requested()) {
        char *restore_error = NULL;
        if (ntfs_restore_workspace(device, db, source->cluster_size, &restore_error) != 0 ||
            clear_source_dirty(device, &restore_error) != 0) {
            if (error != NULL && *error == NULL) *error = restore_error;
            else free(restore_error);
            goto recover_required;
        }
        free(restore_error);
        result = 130;
        goto stopped_unchanged;
    }
    if (place_result != 0) {
        char *restore_error = NULL;
        if (ntfs_restore_workspace(device, db, source->cluster_size, &restore_error) == 0 &&
            clear_source_dirty(device, &restore_error) == 0) {
            free(restore_error);
            transaction_cleanup(journal_path, state);
            goto done;
        }
        free(restore_error);
        goto recover_required;
    }
    if (journal_phase(journal_path, state, "workspace-metadata", error) != 0) goto recover_required;
    puts("NTFS phase 3: committing canonical MFT mapping pairs and $Bitmap metadata.");
    fflush(stdout);
    if (ntfs_apply_stage_metadata(device, db, true, error) != 0) goto recover_required;
    if (journal_phase(journal_path, state, "workspace-verifying-source", error) != 0)
        goto recover_required;
    if (ntfs_verify_stage(device, db, growth, true, error) != 0) goto recover_required;
    if (clear_source_dirty(device, error) != 0) goto recover_required;
    if (live_updates) emit_committed_live_reset(device, error);
    transaction_cleanup(journal_path, state);
    printf("NTFS unified workspace layout: %llu clusters staged and placed without a filesystem-sized working image.\n",
           (unsigned long long)state->workspace_clusters);
    printf("NTFS %s completed with serial and full device capacity preserved.\n",
           growth ? "Growth Defrag" : "Defragment");
    emit_result(operation, "completed", "");
    result = 0;
    goto done;

fallback:
    if (db != NULL) { sqlite3_close(db); db = NULL; }
    transaction_cleanup(journal_path, state);
    memcpy(source_layout->bitmap, original_bitmap, source_layout->bitmap_bytes);
    ntfs_placements_free(&placements);
    state->workspace_start = 0;
    state->workspace_clusters = 0;
    state->move_clusters = 0;
    puts("NTFS direct relayout: no safe terminal workspace can hold the complete dependency set; using the verified working-image fallback.");
    fflush(stdout);
    free(original_bitmap);
    return 2;

stopped_unchanged:
    transaction_cleanup(journal_path, state);
    puts("Stop requested at a safe NTFS workspace boundary; the original filesystem layout is preserved.");
    emit_result(operation, "stopped", "");
    goto done;

recover_required:
    /* Durable workspace and plan are deliberately retained. Recovery can
       replay placement and metadata from their checksummed copies. */
    result = -1;

done:
    if (lock_fd >= 0) { (void)flock(lock_fd, LOCK_UN); close(lock_fd); }
    if (db != NULL) sqlite3_close(db);
    ntfs_placements_free(&placements);
    free(original_bitmap);
    return result;
}
'''
worker_text = worker.read_text(encoding="utf-8")
marker = "static int build_and_commit(const char *device, const char *operation, const char *journal_path,\n"
if worker_text.count(marker) != 1:
    raise SystemExit("ntfs_worker.c: build_and_commit insertion marker not unique")
worker.write_text(worker_text.replace(marker, worker_helpers + "\n" + marker, 1), encoding="utf-8")

replace_once(
    worker,
    "    state.physical_bytes = physical_bytes; state.filesystem_bytes = source.volume_bytes;\n"
    "    if (capacity_preflight(journal_path, &source, &source_layout, error) != 0 || journal_save(journal_path, &state, error) != 0) goto precommit_fail;\n",
    "    state.physical_bytes = physical_bytes; state.filesystem_bytes = source.volume_bytes;\n"
    "    int workspace_result = try_terminal_workspace_relayout(device, operation, journal_path,\n"
    "                                                            live_updates, &state, &source,\n"
    "                                                            &source_layout, &source_catalogue, error);\n"
    "    if (workspace_result == 0 || workspace_result == 130) { result = workspace_result; goto done; }\n"
    "    if (workspace_result < 0) goto done;\n"
    "    if (capacity_preflight(journal_path, &source, &source_layout, error) != 0 || journal_save(journal_path, &state, error) != 0) goto precommit_fail;\n",
)

recovery_block = r'''
    if (state.workspace_clusters != 0 && strncmp(state.phase, "workspace-", 10) == 0) {
        if (state.workspace_start == 0 || state.workspace_clusters != state.move_clusters) {
            ntfs_set_error(error, "NTFS recovery journal has invalid terminal-workspace geometry");
            goto done;
        }
        if (strcmp(state.phase, "workspace-staging") == 0) {
            transaction_cleanup(journal_path, &state);
            puts("Discarded an incomplete NTFS terminal workspace; source metadata was unchanged.");
            result = 0;
            goto done;
        }
        sqlite3 *workspace_db = NULL;
        if (ntfs_open_plan_db(state.plan, &workspace_db, error) != 0) goto done;
        NtfsVolume workspace_volume;
        memset(&workspace_volume, 0, sizeof(workspace_volume));
        workspace_volume.fd = -1;
        if (ntfs_open_volume(device, false, &workspace_volume, error) != 0) {
            sqlite3_close(workspace_db); goto done;
        }
        uint32_t cluster_size = workspace_volume.cluster_size;
        ntfs_close_volume(&workspace_volume);
        if (ntfs_verify_workspace(device, workspace_db, cluster_size, error) != 0) {
            sqlite3_close(workspace_db); goto done;
        }
        bool growth = strcmp(state.operation, "growth-defrag") == 0;
        puts("Recovering NTFS directly from the checksummed terminal safety workspace.");
        fflush(stdout);
        if (mark_source_dirty(device, error) != 0 ||
            journal_phase(journal_path, &state, "workspace-placing", error) != 0 ||
            ntfs_place_workspace(device, workspace_db, cluster_size, false, error) != 0 ||
            journal_phase(journal_path, &state, "workspace-metadata", error) != 0 ||
            ntfs_apply_stage_metadata(device, workspace_db, true, error) != 0 ||
            journal_phase(journal_path, &state, "workspace-verifying-source", error) != 0 ||
            ntfs_verify_stage(device, workspace_db, growth, true, error) != 0 ||
            clear_source_dirty(device, error) != 0) {
            sqlite3_close(workspace_db); goto done;
        }
        sqlite3_close(workspace_db);
        transaction_cleanup(journal_path, &state);
        puts("NTFS terminal-workspace recovery completed successfully.");
        emit_result("recover", "completed", "");
        result = 0;
        goto done;
    }
'''
worker_text = worker.read_text(encoding="utf-8")
recovery_marker = "    if (strcmp(state.phase, \"commit\") != 0 && strcmp(state.phase, \"verifying-source\") != 0) {\n"
if worker_text.count(recovery_marker) != 1:
    raise SystemExit("ntfs_worker.c: recovery insertion marker not unique")
worker.write_text(worker_text.replace(recovery_marker, recovery_block + recovery_marker, 1), encoding="utf-8")

# Current working-image call already patched above. The direct path and recovery
# are now the only calls that deliberately permit a dirty source layout.

native_test = ROOT / "tests/test_native_top3.py"
replace_once(
    native_test,
    "    mutate(worker, image, \"defrag\", work / \"ntfs-defrag.journal\")\n"
    "    packed = run_json(worker, image)\n",
    "    output = mutate(worker, image, \"defrag\", work / \"ntfs-defrag.journal\")\n"
    "    assert \"NTFS unified workspace layout:\" in output, output\n"
    "    assert \"internally verified raw NTFS working image\" not in output, output\n"
    "    packed = run_json(worker, image)\n",
)
replace_once(
    native_test,
    "    mutate(worker, image, \"growth-defrag\", work / \"ntfs-growth.journal\")\n"
    "    grown = run_json(worker, image)\n",
    "    output = mutate(worker, image, \"growth-defrag\", work / \"ntfs-growth.journal\")\n"
    "    assert \"NTFS unified workspace layout:\" in output, output\n"
    "    assert \"internally verified raw NTFS working image\" not in output, output\n"
    "    grown = run_json(worker, image)\n",
)
replace_once(
    native_test,
    "    assert \"Preserving 1 unsupported-but-safe NTFS user stream\" in completed.stdout, completed.stdout\n",
    "    assert \"Preserving 1 unsupported-but-safe NTFS user stream\" in completed.stdout, completed.stdout\n"
    "    assert \"NTFS unified workspace layout:\" in completed.stdout, completed.stdout\n",
)

print("Applied NTFS terminal-workspace relayout fast path and recovery")
