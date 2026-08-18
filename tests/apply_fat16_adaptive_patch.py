#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Temporary branch-only helper that materialises the validated FAT16 patch."""

from pathlib import Path


# Reuse the already-reviewed base adaptive scheduler embedded in the temporary
# push workflow, then upgrade its single-blocker escape to a blocker batch.
workflow = Path('.github/workflows/fat16-adaptive-patch.yml').read_text(encoding='utf-8')
marker = "          python3 - <<'PY'\n"
start = workflow.index(marker) + len(marker)
end = workflow.index("\n          PY\n", start)
lines = workflow[start:end].splitlines()
if not lines:
    raise SystemExit('embedded patch script is empty')
indent = len(lines[0]) - len(lines[0].lstrip(' '))
prefix = ' ' * indent
code = '\n'.join(
    line[indent:] if prefix and line.startswith(prefix) else line
    for line in lines
)
exec(compile(code, 'fat16-adaptive-base-patch', 'exec'))

path = Path('gui/filesystems/fat/native/writer.c')
text = path.read_text(encoding='utf-8')
begin_marker = '        size_t max_workspace_run = largest_free_workspace_run(\n'
end_marker = '    }\n}\n\nstatic GrowthStats fat_relayout_volume'
begin = text.index(begin_marker, text.index('static bool execute_adaptive_dependency_layout'))
end = text.index(end_marker, begin)
replacement = r'''        size_t max_workspace_run = largest_free_workspace_run(
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
        uint8_t *selected = ld_xcalloc(objects->len, 1);
        RelocationMove *stage_moves = NULL;
        size_t stage_move_count = 0;
        size_t stage_move_cap = 0;
        size_t stage_objects = 0;
        size_t stage_files = 0;
        size_t stage_directories = 0;
        size_t released_targets = 0;

        for (;;) {
            if (stage_move_count >= cluster_batch_limit) break;

            size_t best_index = SIZE_MAX;
            size_t best_score = 0;
            size_t best_clusters = 0;
            uint32_t best_staged_at = 0;

            for (size_t index = 0; index < objects->len; index++) {
                if (!nonexact[index] || staged[index] || selected[index]) continue;
                GrowthObject *object = &objects->v[index];
                if (object->clusters > max_workspace_run ||
                    object->clusters > cluster_batch_limit - stage_move_count) {
                    continue;
                }

                const FileRecord *current_file = NULL;
                U32Vec local_root = {0};
                const U32Vec *chain = find_growth_object_chain(
                    fs, object, &current_files, &local_root, &current_file);
                (void)current_file;

                size_t score = 0;
                for (size_t i = 0; i < chain->len; i++) {
                    if (target_needed[chain->v[i]]) score++;
                }
                if (score == 0) {
                    u32vec_free(&local_root);
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

                if (candidate_at != 0 &&
                    (score > best_score ||
                     (score == best_score &&
                      (best_index == SIZE_MAX || chain->len < best_clusters)))) {
                    best_index = index;
                    best_score = score;
                    best_clusters = chain->len;
                    best_staged_at = candidate_at;
                }
                u32vec_free(&local_root);
            }

            if (best_index == SIZE_MAX) break;

            GrowthObject *blocker = &objects->v[best_index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_growth_object_chain(
                fs, blocker, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != blocker->clusters) {
                u32vec_free(&local_root);
                free(stage_moves);
                free(selected);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during adaptive blocker batch");
            }

            if (stage_move_count + chain->len > stage_move_cap) {
                size_t new_cap = stage_move_cap == 0 ? chain->len : stage_move_cap;
                while (new_cap < stage_move_count + chain->len) {
                    if (new_cap > SIZE_MAX / 2) {
                        u32vec_free(&local_root);
                        free(stage_moves);
                        free(selected);
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
                stage_moves = ld_xrealloc(stage_moves, new_cap * sizeof(*stage_moves));
                stage_move_cap = new_cap;
            }
            for (size_t i = 0; i < chain->len; i++) {
                stage_moves[stage_move_count++] = (RelocationMove){
                    .source = chain->v[i],
                    .destination = best_staged_at + (uint32_t)i,
                };
            }
            size_t first_offset = (size_t)(best_staged_at - workspace_start);
            for (size_t i = 0; i < chain->len; i++) {
                workspace_available[first_offset + i] = 0;
            }
            selected[best_index] = 1;
            stage_objects++;
            if (blocker->is_dir) stage_directories++;
            else stage_files++;
            if (best_score > SIZE_MAX - released_targets) {
                u32vec_free(&local_root);
                free(stage_moves);
                free(selected);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("adaptive blocker score overflow");
            }
            released_targets += best_score;
            u32vec_free(&local_root);
        }

        free(selected);
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

        detail_log(
            "layout-adaptive-stage-batch: %zu blockers / %zu clusters -> reusable "
            "terminal workspace; releases %zu outstanding target-cluster references\n",
            stage_objects, stage_move_count, released_targets);
        fat_relocation_execute(
            fs, &current_refs, journal_path, stage_moves, stage_move_count,
            &g_io, detail_log);
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
'''
text = text[:begin] + replacement + text[end:]
path.write_text(text, encoding='utf-8')
