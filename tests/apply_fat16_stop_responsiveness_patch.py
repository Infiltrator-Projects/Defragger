#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Branch-only helper for the FAT16 adaptive scheduler stop-latency fix."""

from pathlib import Path

writer_path = Path("gui/filesystems/fat/native/writer.c")
text = writer_path.read_text(encoding="utf-8")
start = text.index("/* Mixed FAT layouts can contain a small number of fragmented objects whose\n")
end = text.index("\nstatic GrowthStats fat_relayout_volume", start)
replacement = r'''/* Mixed FAT layouts can contain fragmented objects whose scattered source
   clusters block thousands of otherwise independent final targets.  The
   adaptive scheduler finishes every currently-free target first, then stages a
   RAM-sized set of blockers into the durable terminal workspace.  Candidate
   scores are calculated once per scheduler pass and sorted once; repeatedly
   rescanning every object for every selected blocker turns a few thousand FAT16
   files into an accidental cubic-time planner. */
typedef struct {
    size_t object_index;
    size_t score;
    U32Vec chain;
} AdaptiveBlockerCandidate;

static int compare_adaptive_blocker_candidate(const void *left_ptr,
                                               const void *right_ptr) {
    const AdaptiveBlockerCandidate *left = left_ptr;
    const AdaptiveBlockerCandidate *right = right_ptr;
    if (left->score > right->score) return -1;
    if (left->score < right->score) return 1;
    if (left->chain.len < right->chain.len) return -1;
    if (left->chain.len > right->chain.len) return 1;
    if (left->object_index < right->object_index) return -1;
    if (left->object_index > right->object_index) return 1;
    return 0;
}

static void adaptive_blocker_candidates_free(AdaptiveBlockerCandidate *candidates,
                                             size_t count) {
    if (candidates == NULL) return;
    for (size_t index = 0; index < count; index++) {
        u32vec_free(&candidates[index].chain);
    }
    free(candidates);
}

static bool adaptive_stop_between_transactions(const char *layout_name,
                                                GrowthStats *stats) {
    stats->interrupted = true;
    fprintf(stderr,
            "%s stopped safely during adaptive dependency layout at a complete "
            "transaction boundary.\n",
            layout_name);
    return true;
}

/* Large RAM buffers should increase throughput, but they must not make Stop wait
   behind an arbitrarily large journal transaction.  Sixty-four MiB is a byte
   budget, not an object-count cap: tiny FAT16 files still batch by the thousand,
   while a single larger object remains allowed when it is the dependency that
   must be moved. */
static size_t adaptive_transaction_cluster_limit(const Fat32 *fs,
                                                 size_t ram_cluster_limit) {
    const uint64_t stop_bytes = UINT64_C(64) * 1024 * 1024;
    uint64_t by_stop = stop_bytes / fs->cluster_size;
    if (by_stop == 0) by_stop = 1;
    size_t limit = by_stop > SIZE_MAX ? SIZE_MAX : (size_t)by_stop;
    if (limit > ram_cluster_limit) limit = ram_cluster_limit;
    if (limit == 0) limit = 1;
    return limit;
}

static bool execute_adaptive_dependency_layout(
    Fat32 *fs,
    const char *journal_path,
    GrowthObjectList *objects,
    uint32_t workspace_start,
    size_t workspace_clusters,
    size_t cluster_batch_limit,
    const char *layout_name,
    GrowthStats *stats
) {
    size_t transaction_cluster_limit = adaptive_transaction_cluster_limit(
        fs, cluster_batch_limit);
    double transaction_mib =
        ((double)transaction_cluster_limit * (double)fs->cluster_size) /
        (1024.0 * 1024.0);
    fprintf(stderr,
            "%s adaptive dependency transaction budget: up to %zu clusters "
            "(%.1f MiB) per journal transaction for bounded Stop latency.\n",
            layout_name, transaction_cluster_limit, transaction_mib);

    for (;;) {
        if (ld_stop_requested()) {
            return adaptive_stop_between_transactions(layout_name, stats);
        }

        DirRefList current_refs = {0};
        FileList current_files = scan_files(fs, &current_refs);
        uint8_t *nonexact = ld_xcalloc(objects->len, 1);
        uint8_t *staged = ld_xcalloc(objects->len, 1);
        uint8_t *target_needed = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        size_t remaining = 0;

        for (size_t index = 0; index < objects->len; index++) {
            if (ld_stop_requested()) {
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return adaptive_stop_between_transactions(layout_name, stats);
            }
            GrowthObject *object = &objects->v[index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_growth_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during adaptive dependency scan");
            }
            if (!chain_is_exact_run(chain, object->target)) {
                nonexact[index] = 1;
                staged[index] = chain_is_inside_workspace(
                    chain, workspace_start, workspace_clusters) ? 1 : 0;
                remaining++;
                uint64_t target_end =
                    (uint64_t)object->target + object->clusters - 1;
                if (target_end > fs->max_cluster) {
                    u32vec_free(&local_root);
                    free(target_needed);
                    free(staged);
                    free(nonexact);
                    filelist_free(&current_files);
                    dirreflist_free(&current_refs);
                    ld_die("adaptive dependency target exceeds the filesystem");
                }
                for (size_t i = 0; i < object->clusters; i++) {
                    target_needed[object->target + (uint32_t)i] = 1;
                }
            }
            u32vec_free(&local_root);
        }

        if (remaining == 0) {
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return true;
        }

        uint8_t *source_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        uint8_t *destination_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        RelocationMove *moves = NULL;
        size_t move_count = 0;
        size_t move_cap = 0;
        size_t batch_objects = 0;
        size_t batch_files = 0;
        size_t batch_directories = 0;
        bool stop_during_direct_scan = false;

        for (size_t cursor = objects->len; cursor != 0; cursor--) {
            if (ld_stop_requested()) {
                stop_during_direct_scan = true;
                break;
            }
            size_t index = cursor - 1;
            if (!nonexact[index]) continue;
            GrowthObject *object = &objects->v[index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_growth_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                free(moves);
                free(destination_seen);
                free(source_seen);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during adaptive direct batch");
            }

            if (move_count != 0 &&
                object->clusters > transaction_cluster_limit - move_count) {
                u32vec_free(&local_root);
                continue;
            }
            if (move_count != 0 &&
                move_count + object->clusters > transaction_cluster_limit) {
                u32vec_free(&local_root);
                continue;
            }
            if (!growth_batch_can_add(fs, chain, object->target,
                                      source_seen, destination_seen)) {
                u32vec_free(&local_root);
                continue;
            }

            if (move_count + chain->len > move_cap) {
                size_t new_cap = move_cap == 0 ? chain->len : move_cap;
                while (new_cap < move_count + chain->len) new_cap *= 2;
                moves = ld_xrealloc(moves, new_cap * sizeof(*moves));
                move_cap = new_cap;
            }
            for (size_t i = 0; i < chain->len; i++) {
                uint32_t source = chain->v[i];
                uint32_t destination = object->target + (uint32_t)i;
                moves[move_count++] = (RelocationMove){
                    .source = source,
                    .destination = destination,
                };
                source_seen[source] = 1;
                destination_seen[destination] = 1;
            }
            batch_objects++;
            if (object->is_dir) batch_directories++;
            else batch_files++;
            u32vec_free(&local_root);
            if (move_count >= transaction_cluster_limit) break;
        }

        free(destination_seen);
        free(source_seen);
        if (stop_during_direct_scan || ld_stop_requested()) {
            free(moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return adaptive_stop_between_transactions(layout_name, stats);
        }
        if (move_count != 0) {
            fprintf(stderr,
                    "%s adaptive direct batch: committing %zu object%s "
                    "(%zu cluster%s) at the next safe journal boundary.\n",
                    layout_name,
                    batch_objects, batch_objects == 1 ? "" : "s",
                    move_count, move_count == 1 ? "" : "s");
            fflush(stderr);
            relocation_execute_moves(
                fs, &current_refs, journal_path, moves, move_count);
            stats->transactions++;
            stats->clusters_copied += move_count;
            stats->objects_moved += batch_objects;
            stats->files_moved += batch_files;
            stats->directories_moved += batch_directories;
            fprintf(stderr,
                    "%s adaptive direct batch: %zu object%s (%zu file%s, "
                    "%zu director%s), %zu cluster%s committed in one journal "
                    "transaction; %zu object%s remain.\n",
                    layout_name,
                    batch_objects, batch_objects == 1 ? "" : "s",
                    batch_files, batch_files == 1 ? "" : "s",
                    batch_directories, batch_directories == 1 ? "y" : "ies",
                    move_count, move_count == 1 ? "" : "s",
                    remaining - batch_objects,
                    remaining - batch_objects == 1 ? "" : "s");
            free(moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            continue;
        }
        free(moves);

        if (ld_stop_requested()) {
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return adaptive_stop_between_transactions(layout_name, stats);
        }

        size_t max_workspace_run = largest_free_workspace_run(
            fs, workspace_start, workspace_clusters);
        uint32_t first_workspace_cluster = find_free_workspace_run(
            fs, workspace_start, workspace_clusters, 1);
        if (max_workspace_run == 0 || first_workspace_cluster == 0) {
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return false;
        }

        uint8_t *workspace_available = ld_xcalloc(workspace_clusters, 1);
        for (size_t offset = 0; offset < workspace_clusters; offset++) {
            workspace_available[offset] = fat_is_free(
                fs, workspace_start + (uint32_t)offset) ? 1 : 0;
        }

        AdaptiveBlockerCandidate *candidates = ld_xcalloc(
            objects->len, sizeof(*candidates));
        size_t candidate_count = 0;
        for (size_t index = 0; index < objects->len; index++) {
            if (ld_stop_requested()) {
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return adaptive_stop_between_transactions(layout_name, stats);
            }
            if (!nonexact[index] || staged[index]) continue;
            GrowthObject *object = &objects->v[index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_growth_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during adaptive blocker planning");
            }

            size_t score = 0;
            for (size_t i = 0; i < chain->len; i++) {
                if (target_needed[chain->v[i]]) score++;
            }
            if (score != 0 && chain->len <= max_workspace_run) {
                AdaptiveBlockerCandidate *candidate =
                    &candidates[candidate_count++];
                candidate->object_index = index;
                candidate->score = score;
                candidate->chain.v = ld_xmalloc(
                    chain->len * sizeof(*candidate->chain.v));
                memcpy(candidate->chain.v, chain->v,
                       chain->len * sizeof(*candidate->chain.v));
                candidate->chain.len = chain->len;
                candidate->chain.cap = chain->len;
            }
            u32vec_free(&local_root);
        }

        qsort(candidates, candidate_count, sizeof(*candidates),
              compare_adaptive_blocker_candidate);
        fprintf(stderr,
                "%s adaptive dependency planning: %zu object%s remain; %zu "
                "blocking candidate%s scored once for a %zu-cluster workspace.\n",
                layout_name,
                remaining, remaining == 1 ? "" : "s",
                candidate_count, candidate_count == 1 ? "" : "s",
                workspace_clusters);
        fflush(stderr);

        RelocationMove *stage_moves = NULL;
        size_t stage_move_count = 0;
        size_t stage_move_cap = 0;
        size_t stage_objects = 0;
        size_t stage_files = 0;
        size_t stage_directories = 0;
        size_t released_targets = 0;

        for (size_t candidate_index = 0;
             candidate_index < candidate_count;
             candidate_index++) {
            if (ld_stop_requested()) {
                free(stage_moves);
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return adaptive_stop_between_transactions(layout_name, stats);
            }
            if (stage_move_count >= transaction_cluster_limit) break;

            AdaptiveBlockerCandidate *candidate = &candidates[candidate_index];
            const U32Vec *chain = &candidate->chain;
            if (stage_move_count != 0 &&
                chain->len > transaction_cluster_limit - stage_move_count) {
                continue;
            }

            size_t run = 0;
            uint32_t candidate_at = 0;
            for (size_t offset = 0; offset < workspace_clusters; offset++) {
                if (workspace_available[offset]) {
                    run++;
                    if (run == chain->len) {
                        size_t first_offset = offset + 1 - chain->len;
                        candidate_at = workspace_start + (uint32_t)first_offset;
                        break;
                    }
                } else {
                    run = 0;
                }
            }
            if (candidate_at == 0) continue;

            if (stage_move_count + chain->len > stage_move_cap) {
                size_t new_cap = stage_move_cap == 0 ? chain->len : stage_move_cap;
                while (new_cap < stage_move_count + chain->len) {
                    if (new_cap > SIZE_MAX / 2) {
                        free(stage_moves);
                        adaptive_blocker_candidates_free(candidates, candidate_count);
                        free(workspace_available);
                        free(target_needed);
                        free(staged);
                        free(nonexact);
                        filelist_free(&current_files);
                        dirreflist_free(&current_refs);
                        ld_die("adaptive blocker move array overflow");
                    }
                    new_cap *= 2;
                }
                stage_moves = ld_xrealloc(
                    stage_moves, new_cap * sizeof(*stage_moves));
                stage_move_cap = new_cap;
            }
            for (size_t i = 0; i < chain->len; i++) {
                stage_moves[stage_move_count++] = (RelocationMove){
                    .source = chain->v[i],
                    .destination = candidate_at + (uint32_t)i,
                };
            }
            size_t first_offset = (size_t)(candidate_at - workspace_start);
            for (size_t i = 0; i < chain->len; i++) {
                workspace_available[first_offset + i] = 0;
            }
            stage_objects++;
            if (objects->v[candidate->object_index].is_dir) stage_directories++;
            else stage_files++;
            if (candidate->score > SIZE_MAX - released_targets) {
                free(stage_moves);
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("adaptive blocker score overflow");
            }
            released_targets += candidate->score;
        }

        adaptive_blocker_candidates_free(candidates, candidate_count);
        free(workspace_available);
        if (stage_move_count == 0) {
            free(stage_moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return false;
        }

        if (ld_stop_requested()) {
            free(stage_moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return adaptive_stop_between_transactions(layout_name, stats);
        }

        double stage_mib =
            ((double)stage_move_count * (double)fs->cluster_size) /
            (1024.0 * 1024.0);
        fprintf(stderr,
                "%s adaptive dependency batch: staging %zu blocker%s "
                "(%zu cluster%s, %.1f MiB) into the safety workspace in one "
                "journal transaction.\n",
                layout_name,
                stage_objects, stage_objects == 1 ? "" : "s",
                stage_move_count, stage_move_count == 1 ? "" : "s",
                stage_mib);
        fflush(stderr);
        detail_log(
            "layout-adaptive-stage-batch: %zu blockers / %zu clusters -> reusable "
            "terminal workspace; releases %zu outstanding target-cluster references\n",
            stage_objects, stage_move_count, released_targets);
        relocation_execute_moves(
            fs, &current_refs, journal_path, stage_moves, stage_move_count);
        stats->transactions++;
        stats->clusters_copied += stage_move_count;
        fprintf(stderr,
                "%s adaptive dependency batch: staged %zu blocker%s "
                "(%zu file%s, %zu director%s), %zu cluster%s in one journal "
                "transaction to release %zu blocked target-cluster references.\n",
                layout_name,
                stage_objects, stage_objects == 1 ? "" : "s",
                stage_files, stage_files == 1 ? "" : "s",
                stage_directories, stage_directories == 1 ? "y" : "ies",
                stage_move_count, stage_move_count == 1 ? "" : "s",
                released_targets);

        free(stage_moves);
        free(target_needed);
        free(staged);
        free(nonexact);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
    }
}
'''
writer_path.write_text(text[:start] + replacement + text[end:], encoding="utf-8")

# The GUI does not know whether the engine is currently writing or merely
# calculating the next dependency batch.  Describe the actual contract: Stop is
# honoured at the next safe transaction boundary.
runner_path = Path("gui/ui/command_runner.py")
runner = runner_path.read_text(encoding="utf-8")
old_runner = (
    '"Stop requested. Waiting for the active journalled transaction "\n'
    '                "to finish…",'
)
new_runner = (
    '"Stop requested. Waiting for the engine to reach the next safe "\n'
    '                "transaction boundary…",'
)
if old_runner not in runner:
    raise SystemExit("command_runner Stop text anchor not found")
runner_path.write_text(runner.replace(old_runner, new_runner, 1), encoding="utf-8")

priv_path = Path("gui/ui/privilege_session.py")
priv = priv_path.read_text(encoding="utf-8")
old_priv = (
    '"Stop signal delivered; the engine will exit after the "\n'
    '                        "active journalled transaction.",'
)
new_priv = (
    '"Stop signal delivered; the engine will exit at the next safe "\n'
    '                        "transaction boundary.",'
)
if old_priv not in priv:
    raise SystemExit("privilege_session Stop text anchor not found")
priv_path.write_text(priv.replace(old_priv, new_priv, 1), encoding="utf-8")

# Keep the physical-FAT16 regression explicit about the new planner path and
# pre-transaction status line.
test_path = Path("tests/test_fat_relayout_engine.sh")
test = test_path.read_text(encoding="utf-8")
anchor = '''grep -q 'adaptive dependency batch' "$WORK/fat16-dependencies.log" || {
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 dependency blocker workload did not exercise adaptive staging"
}
'''
addition = anchor + '''grep -q 'adaptive dependency transaction budget:' "$WORK/fat16-dependencies.log" || {
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 adaptive dependency workload did not report its Stop-safe transaction budget"
}
grep -q 'adaptive dependency batch: staging' "$WORK/fat16-dependencies.log" || {
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 adaptive dependency workload did not report the staging transaction before it began"
}
'''
if anchor not in test:
    raise SystemExit("FAT relayout regression anchor not found")
test_path.write_text(test.replace(anchor, addition, 1), encoding="utf-8")
