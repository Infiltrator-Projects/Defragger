// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_FAT_GROWTH_H
#define LINUX_DEFRAGGER_FAT_GROWTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat_directory.h"
#include "fat_volume.h"

typedef struct {
    bool is_root;
    bool is_dir;
    char *path;
    size_t clusters;
    uint32_t physical_first;
    uint32_t target;
    size_t reserve_after;
} GrowthObject;

typedef struct {
    GrowthObject *v;
    size_t len;
    size_t cap;
} GrowthObjectList;

typedef struct {
    size_t objects_moved;
    size_t files_moved;
    size_t directories_moved;
    size_t clusters_copied;
    size_t transactions;
    size_t packing_clusters;
    size_t packing_transactions;
    size_t reserve_clusters;
    unsigned applied_percent;
    bool interrupted;
    bool layout_started;
    bool already_satisfied;
    bool canonical_layout_verified;
    size_t checked_files;
    size_t checked_directories;
} GrowthStats;

typedef enum {
    GROWTH_PREFLIGHT_OK = 0,
    GROWTH_PREFLIGHT_NO_FILES,
    GROWTH_PREFLIGHT_ROOT_FRAGMENTED,
    GROWTH_PREFLIGHT_OBJECT_FRAGMENTED,
    GROWTH_PREFLIGHT_RESERVE_SHORT,
} GrowthPreflightIssue;

typedef struct {
    GrowthPreflightIssue issue;
    size_t regular_files;
    size_t directories;
    size_t reserve_clusters;
    size_t required_reserve;
    size_t available_reserve;
    char *problem_path;
} GrowthPreflight;

void growth_object_list_free(GrowthObjectList *list);
GrowthObjectList build_growth_objects(
    Fat32 *filesystem,
    const FileList *files,
    size_t *largest_out,
    uint64_t *regular_clusters_out,
    size_t *regular_files_out,
    size_t *directories_out
);
bool plan_growth_layout(
    Fat32 *filesystem,
    GrowthObjectList *objects,
    unsigned percent,
    uint32_t workspace_start,
    size_t *reserve_total_out,
    uint32_t *layout_end_out
);
bool chain_is_exact_run(const U32Vec *chain, uint32_t start);
GrowthPreflight growth_layout_preflight(
    Fat32 *filesystem,
    const FileList *files,
    unsigned percent
);
void growth_preflight_free(GrowthPreflight *preflight);
bool growth_layout_matches_canonical(
    Fat32 *filesystem,
    GrowthObjectList *objects,
    unsigned percent,
    size_t *reserve_clusters_out
);
void print_growth_preflight_failure(
    const GrowthPreflight *preflight,
    unsigned percent,
    const char *layout_name
);

#endif
