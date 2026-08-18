// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared FAT12/FAT16/FAT32 canonical relayout model and pure planner. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld_runtime.h"
#include "fat_relayout.h"

static uint32_t chain_min_cluster(const U32Vec *chain) {
    uint32_t minimum = UINT32_MAX;
    for (size_t i = 0; i < chain->len; i++) {
        if (chain->v[i] < minimum) minimum = chain->v[i];
    }
    return minimum;
}

static void relayout_object_list_push(FatRelayoutObjectList *list, FatRelayoutObject object) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 128 : list->cap * 2;
        list->v = ld_xrealloc(list->v, new_cap * sizeof(*list->v));
        list->cap = new_cap;
    }
    list->v[list->len++] = object;
}

void fat_relayout_object_list_free(FatRelayoutObjectList *list) {
    for (size_t i = 0; i < list->len; i++) free(list->v[i].path);
    free(list->v);
    memset(list, 0, sizeof(*list));
}

static int compare_relayout_objects_asc(const void *a, const void *b) {
    const FatRelayoutObject *oa = a;
    const FatRelayoutObject *ob = b;
    if (oa->physical_first != ob->physical_first) {
        return oa->physical_first < ob->physical_first ? -1 : 1;
    }
    if (oa->is_root != ob->is_root) return oa->is_root ? -1 : 1;
    if (oa->is_dir != ob->is_dir) return oa->is_dir ? -1 : 1;
    return strcmp(oa->path, ob->path);
}

FatRelayoutObjectList fat_relayout_build_objects(Fat32 *fs, const FileList *files,
                                               size_t *largest_out,
                                               uint64_t *regular_clusters_out,
                                               size_t *regular_files_out,
                                               size_t *directories_out) {
    FatRelayoutObjectList objects = {0};
    size_t largest = 0;
    uint64_t regular_clusters = 0;
    size_t regular_files = 0;
    size_t directories = 0;

    U32Vec root = filesystem_root_chain(fs);
    if (root.len != 0) {
        relayout_object_list_push(&objects, (FatRelayoutObject){
            .is_root = true,
            .is_dir = true,
            .path = ld_xstrdup("<root directory>"),
            .clusters = root.len,
            .physical_first = chain_min_cluster(&root),
        });
        if (root.len > largest) largest = root.len;
        directories++;
    }
    u32vec_free(&root);

    for (size_t i = 0; i < files->len; i++) {
        const FileRecord *file = &files->v[i];
        if (file->chain.len == 0) continue;
        relayout_object_list_push(&objects, (FatRelayoutObject){
            .is_root = false,
            .is_dir = file->is_dir,
            .path = ld_xstrdup(file->path),
            .clusters = file->chain.len,
            .physical_first = chain_min_cluster(&file->chain),
        });
        if (file->chain.len > largest) largest = file->chain.len;
        if (file->is_dir) directories++;
        else {
            regular_files++;
            regular_clusters += file->chain.len;
        }
    }
    qsort(objects.v, objects.len, sizeof(objects.v[0]), compare_relayout_objects_asc);
    *largest_out = largest;
    *regular_clusters_out = regular_clusters;
    *regular_files_out = regular_files;
    *directories_out = directories;
    return objects;
}

static bool relayout_cluster_is_barrier(const Fat32 *fs, uint32_t cluster) {
    return fat_value(fs, cluster) == fat_bad_value(fs);
}

static bool relayout_find_usable_run(const Fat32 *fs, uint32_t cursor, size_t length,
                                   uint32_t limit_exclusive, uint32_t *start_out) {
    if (length == 0 || cursor < 2) return false;
    uint32_t start = cursor;
    while (start < limit_exclusive) {
        uint64_t end64 = (uint64_t)start + length;
        if (end64 > limit_exclusive) return false;
        bool clear = true;
        for (size_t i = 0; i < length; i++) {
            uint32_t cluster = start + (uint32_t)i;
            if (relayout_cluster_is_barrier(fs, cluster)) {
                if (cluster == UINT32_MAX) return false;
                start = cluster + 1;
                clear = false;
                break;
            }
        }
        if (clear) {
            *start_out = start;
            return true;
        }
    }
    return false;
}

static bool relayout_advance_reserve(const Fat32 *fs, uint32_t cursor, size_t reserve,
                                   uint32_t limit_exclusive, uint32_t *cursor_out) {
    size_t remaining = reserve;
    uint32_t c = cursor;
    while (remaining != 0) {
        if (c >= limit_exclusive) return false;
        if (!relayout_cluster_is_barrier(fs, c)) remaining--;
        if (c == UINT32_MAX) return false;
        c++;
    }
    *cursor_out = c;
    return c <= limit_exclusive;
}

bool fat_relayout_plan_layout(Fat32 *fs, FatRelayoutObjectList *objects,
                               unsigned percent, uint32_t workspace_start,
                               size_t *reserve_total_out, uint32_t *layout_end_out) {
    uint32_t cursor = 2;
    size_t reserve_total = 0;
    for (size_t i = 0; i < objects->len; i++) {
        FatRelayoutObject *object = &objects->v[i];
        uint32_t target = 0;
        if (!relayout_find_usable_run(fs, cursor, object->clusters,
                                    workspace_start, &target)) {
            return false;
        }
        object->target = target;
        uint64_t after64 = (uint64_t)target + object->clusters;
        if (after64 > workspace_start) return false;
        cursor = (uint32_t)after64;
        object->reserve_after = 0;
        if (!object->is_dir) {
            uint64_t scaled = (uint64_t)object->clusters * percent;
            size_t reserve = (size_t)((scaled + 99) / 100);
            object->reserve_after = reserve;
            if (reserve > SIZE_MAX - reserve_total) return false;
            reserve_total += reserve;
            if (!relayout_advance_reserve(fs, cursor, reserve,
                                        workspace_start, &cursor)) {
                return false;
            }
        }
    }
    *reserve_total_out = reserve_total;
    *layout_end_out = cursor == 0 ? 0 : cursor - 1;
    return true;
}

bool chain_is_exact_run(const U32Vec *chain, uint32_t start) {
    if (chain->len == 0) return false;
    for (size_t i = 0; i < chain->len; i++) {
        if (chain->v[i] != start + (uint32_t)i) return false;
    }
    return true;
}

/* Canonical FAT relayout is idempotent for both zero-gap Defragment and
   reserve-bearing Growth Defrag.  The preflight is deliberately
   read-only and records the first reason an existing layout cannot be accepted.
   It is run before any journal is created or any FAT/data write is attempted. */
FatRelayoutPreflight fat_relayout_preflight(Fat32 *fs, const FileList *files,
                                                unsigned percent) {
    FatRelayoutPreflight result = {0};

    U32Vec root = filesystem_root_chain(fs);
    if (root.len != 0) {
        result.directories++;
        if (!chain_is_exact_run(&root, root.v[0])) {
            result.issue = FAT_RELAYOUT_PREFLIGHT_ROOT_FRAGMENTED;
            result.problem_path = ld_xstrdup("<root directory>");
            u32vec_free(&root);
            return result;
        }
    }
    u32vec_free(&root);

    for (size_t i = 0; i < files->len; i++) {
        const FileRecord *file = &files->v[i];
        if (file->chain.len == 0) continue;
        if (!chain_is_exact_run(&file->chain, file->chain.v[0])) {
            result.issue = FAT_RELAYOUT_PREFLIGHT_OBJECT_FRAGMENTED;
            result.problem_path = ld_xstrdup(file->path);
            return result;
        }
        if (file->is_dir) {
            result.directories++;
            continue;
        }

        result.regular_files++;
        uint64_t scaled = (uint64_t)file->chain.len * percent;
        size_t reserve = (size_t)((scaled + 99) / 100);
        if (reserve > SIZE_MAX - result.reserve_clusters) {
            result.issue = FAT_RELAYOUT_PREFLIGHT_RESERVE_SHORT;
            result.problem_path = ld_xstrdup(file->path);
            result.required_reserve = reserve;
            return result;
        }
        result.reserve_clusters += reserve;

        uint64_t cursor64 = (uint64_t)file->chain.v[0] + file->chain.len;
        size_t available = 0;
        while (available < reserve) {
            if (cursor64 > fs->max_cluster) break;
            uint32_t cluster = (uint32_t)cursor64;
            uint32_t value = fat_value(fs, cluster);
            if (value == fat_bad_value(fs)) {
                cursor64++;
                continue;
            }
            if (!fat_is_free(fs, cluster)) break;
            available++;
            cursor64++;
        }
        if (available < reserve) {
            result.issue = FAT_RELAYOUT_PREFLIGHT_RESERVE_SHORT;
            result.problem_path = ld_xstrdup(file->path);
            result.required_reserve = reserve;
            result.available_reserve = available;
            return result;
        }
    }

    result.issue = result.regular_files == 0 ? FAT_RELAYOUT_PREFLIGHT_NO_FILES : FAT_RELAYOUT_PREFLIGHT_OK;
    return result;
}

void fat_relayout_preflight_free(FatRelayoutPreflight *preflight) {
    free(preflight->problem_path);
    preflight->problem_path = NULL;
}

/* A second, independent idempotence check recognises the exact canonical
   physical layout produced by the shared FAT relayout planner.  It compares every object's live
   first cluster with a fresh read-only plan generated from the current physical
   order.  scan_files() has already rejected lost or unreferenced FAT chains, so
   matching canonical targets also proves that the planned inter-file gaps are
   genuinely unallocated. */
bool fat_relayout_matches_canonical(Fat32 *fs, FatRelayoutObjectList *objects,
                                            unsigned percent,
                                            size_t *reserve_clusters_out) {
    if (fs->max_cluster == UINT32_MAX) return false;
    uint32_t limit_exclusive = fs->max_cluster + 1;
    size_t reserve_total = 0;
    uint32_t layout_end = 0;
    if (!fat_relayout_plan_layout(fs, objects, percent, limit_exclusive,
                            &reserve_total, &layout_end)) {
        return false;
    }
    (void)layout_end;
    for (size_t i = 0; i < objects->len; i++) {
        if (objects->v[i].target != objects->v[i].physical_first) return false;
    }
    *reserve_clusters_out = reserve_total;
    return objects->len != 0;
}

void fat_relayout_print_preflight_failure(const FatRelayoutPreflight *preflight,
                                           unsigned percent,
                                           const char *layout_name) {
    switch (preflight->issue) {
        case FAT_RELAYOUT_PREFLIGHT_NO_FILES:
            fprintf(stderr,
                    "%s preflight result: no allocated regular files were found.\n", layout_name);
            break;
        case FAT_RELAYOUT_PREFLIGHT_ROOT_FRAGMENTED:
            fprintf(stderr,
                    "%s preflight result: the FAT root directory is fragmented.\n", layout_name);
            break;
        case FAT_RELAYOUT_PREFLIGHT_OBJECT_FRAGMENTED:
            fprintf(stderr,
                    "%s preflight result: %s is fragmented.\n",
                    layout_name, preflight->problem_path == NULL ? "an allocated object" : preflight->problem_path);
            break;
        case FAT_RELAYOUT_PREFLIGHT_RESERVE_SHORT:
            fprintf(stderr,
                    "%s preflight result: %s has %zu usable free cluster%s immediately "
                    "after it; %zu %s required for a %u%% reserve.\n",
                    layout_name,
                    preflight->problem_path == NULL ? "a regular file" : preflight->problem_path,
                    preflight->available_reserve,
                    preflight->available_reserve == 1 ? "" : "s",
                    preflight->required_reserve,
                    preflight->required_reserve == 1 ? "is" : "are",
                    percent);
            break;
        case FAT_RELAYOUT_PREFLIGHT_OK:
            break;
    }
    fprintf(stderr,
            "%s preflight completed read-only; no filesystem writes have occurred yet.\n", layout_name);
}

