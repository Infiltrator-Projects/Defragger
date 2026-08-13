// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux Defragger engine
 * Author: Shannon Smith
 *
 * Implements FAT12, FAT16 and FAT32 analysis and canonical packed defragmentation,
 * growth-space defragmentation, transaction journalling and recovery. This native
 * worker is owned by the authoritative GUI FAT plugin and remains independently
 * testable without creating a second filesystem registry.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"
#include "infiltratr/core.h"
#include "version.h"
#include "fat_analysis.h"
#include "fat_directory.h"
#include "fat_growth.h"
#include "fat_io.h"
#include "fat_journal.h"
#include "fat_relocation.h"
#include "fat_volume.h"

#define PROGRAM_NAME "linux-defragger-fat-worker"
#define PROGRAM_VERSION LD_VERSION

/* Runtime I/O policy and counters used by the buffered relocation pipeline. */
static FatIoConfig g_io;
static bool g_verbose = false;
static FILE *g_diagnostic_log = NULL;

static void detail_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (g_verbose) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(stderr, format, copy);
        va_end(copy);
    }
    if (g_diagnostic_log != NULL) {
        vfprintf(g_diagnostic_log, format, args);
        fflush(g_diagnostic_log);
    }
    va_end(args);
}
static char *default_journal_path(const char *device_path) {
    const char *base = strrchr(device_path, '/');
    base = base == NULL ? device_path : base + 1;
    size_t n = strlen(base) + 40;
    char *path = ld_xmalloc(n);
    snprintf(path, n, ".linux-defragger-fat-worker-%s.journal", base);
    return path;
}

static bool cluster_is_movable_allocation(const Fat32 *fs, uint32_t cluster) {
    uint32_t value = fat_value(fs, cluster);
    return value != 0 && value != fat_bad_value(fs);
}

static uint64_t count_free_clusters(const Fat32 *fs) {
    uint64_t count = 0;
    for (uint32_t cluster = 2; cluster <= fs->max_cluster; cluster++) {
        if (fat_is_free(fs, cluster)) count++;
    }
    return count;
}

static uint64_t terminal_free_clusters(const Fat32 *fs) {
    uint64_t free_count = 0;
    for (uint32_t c = fs->max_cluster; c >= 2; c--) {
        if (!fat_is_free(fs, c)) break;
        free_count++;
        if (c == 2) break;
    }
    return free_count;
}

typedef struct {
    size_t clusters_moved;
    size_t transactions;
    size_t whole_objects;
    size_t whole_clusters;
    size_t staged_objects;
    size_t staged_clusters;
    size_t extent_transactions;
    size_t extent_clusters;
    size_t singleton_transactions;
} PackingStats;

typedef struct {
    bool is_dir;
    const U32Vec *chain;
    const char *path;
    uint32_t destination;
} PlannedWholeObject;

static uint32_t chain_min_cluster(const U32Vec *chain) {
    uint32_t minimum = UINT32_MAX;
    for (size_t i = 0; i < chain->len; i++) {
        if (chain->v[i] < minimum) minimum = chain->v[i];
    }
    return minimum;
}

static bool chain_contains_cluster(const U32Vec *chain, uint32_t cluster) {
    for (size_t i = 0; i < chain->len; i++) {
        if (chain->v[i] == cluster) return true;
    }
    return false;
}

static bool chain_lies_above_destination(const U32Vec *chain, uint32_t destination) {
    if (chain->len == 0 || chain->len > UINT32_MAX) return false;
    uint64_t end64 = (uint64_t)destination + chain->len - 1;
    if (end64 > UINT32_MAX) return false;
    uint32_t end = (uint32_t)end64;
    for (size_t i = 0; i < chain->len; i++) {
        if (chain->v[i] <= end) return false;
    }
    return true;
}

static bool first_free_run_below_high_water(const Fat32 *fs, uint32_t *start_out,
                                             size_t *length_out, uint32_t *highest_out) {
    uint32_t highest = 1;
    for (uint32_t c = fs->max_cluster; c >= 2; c--) {
        if (cluster_is_movable_allocation(fs, c)) {
            highest = c;
            break;
        }
        if (c == 2) break;
    }
    *highest_out = highest;
    if (highest < 2) return false;

    for (uint32_t c = 2; c <= highest; c++) {
        if (!fat_is_free(fs, c)) continue;
        uint32_t start = c;
        size_t length = 0;
        while (c <= highest && fat_is_free(fs, c)) {
            length++;
            if (c == highest) break;
            c++;
        }
        *start_out = start;
        *length_out = length;
        return true;
    }
    return false;
}

static bool better_whole_object(uint32_t minimum, size_t clusters, const char *path,
                                bool have_best, uint32_t best_minimum,
                                size_t best_clusters, const char *best_path) {
    if (!have_best) return true;
    if (minimum != best_minimum) return minimum < best_minimum;
    if (clusters != best_clusters) return clusters > best_clusters;
    return strcmp(path, best_path) < 0;
}

static void relocation_execute_moves(Fat32 *fs, const DirRefList *dir_refs,
                                  const char *journal_path,
                                  const RelocationMove *moves, size_t move_count) {
    fat_relocation_execute(
        fs, dir_refs, journal_path, moves, move_count, &g_io, detail_log
    );
    fat_analysis_emit_live_map_update(fs);
}

static PackingStats growth_prepare_volume(Fat32 *fs, const char *journal_path,
                                   size_t max_clusters, size_t batch_clusters,
                                   size_t max_transactions,
                                   bool allow_extent_fallback) {
    PackingStats stats = {0};
    if (batch_clusters == 0) batch_clusters = 4096;

    for (;;) {
        if (ld_stop_requested()) {
            fprintf(stderr, "interrupt requested; stopping packing between transactions\n");
            break;
        }
        if (max_transactions != 0 && stats.transactions >= max_transactions) break;
        size_t remaining = max_clusters == 0 ? SIZE_MAX : max_clusters - stats.clusters_moved;
        if (remaining == 0) break;

        DirRefList dir_refs = {0};
        FileList files = scan_files(fs, &dir_refs);
        U32Vec root_chain = filesystem_root_chain(fs);

        uint32_t hole_start = 0;
        size_t hole_length = 0;
        uint32_t highest = 1;
        if (!first_free_run_below_high_water(fs, &hole_start, &hole_length, &highest)) {
            u32vec_free(&root_chain);
            filelist_free(&files);
            dirreflist_free(&dir_refs);
            break;
        }

        bool *selected_files = ld_xcalloc(files.len, 1);
        bool selected_root = false;
        RelocationMove *moves = NULL;
        size_t move_count = 0;
        size_t move_cap = 0;
        PlannedWholeObject *planned = NULL;
        size_t planned_count = 0;
        size_t planned_cap = 0;
        uint32_t destination = hole_start;
        size_t hole_remaining = hole_length;

        for (;;) {
            bool have_best = false;
            bool best_is_root = false;
            size_t best_index = 0;
            const U32Vec *best_chain = NULL;
            const char *best_path = NULL;
            bool best_is_dir = false;
            uint32_t best_minimum = 0;
            size_t available = remaining - move_count;
            if (available == 0 || hole_remaining == 0) break;

            size_t transaction_available = move_count == 0 && batch_clusters < available
                                           ? available : batch_clusters > move_count
                                           ? batch_clusters - move_count : 0;
            if (move_count != 0 && transaction_available == 0) break;

            if (!selected_root && root_chain.len != 0 && root_chain.len <= hole_remaining &&
                root_chain.len <= available &&
                (move_count == 0 || root_chain.len <= transaction_available) &&
                chain_lies_above_destination(&root_chain, destination)) {
                uint32_t minimum = chain_min_cluster(&root_chain);
                if (better_whole_object(minimum, root_chain.len, "<root directory>",
                                        have_best, best_minimum,
                                        best_chain == NULL ? 0 : best_chain->len, best_path)) {
                    have_best = true;
                    best_is_root = true;
                    best_chain = &root_chain;
                    best_path = "<root directory>";
                    best_is_dir = true;
                    best_minimum = minimum;
                }
            }

            for (size_t i = 0; i < files.len; i++) {
                const FileRecord *candidate = &files.v[i];
                if (selected_files[i] || candidate->chain.len == 0 ||
                    candidate->chain.len > hole_remaining || candidate->chain.len > available ||
                    (move_count != 0 && candidate->chain.len > transaction_available) ||
                    !chain_lies_above_destination(&candidate->chain, destination)) {
                    continue;
                }
                uint32_t minimum = chain_min_cluster(&candidate->chain);
                if (better_whole_object(minimum, candidate->chain.len, candidate->path,
                                        have_best, best_minimum,
                                        best_chain == NULL ? 0 : best_chain->len, best_path)) {
                    have_best = true;
                    best_is_root = false;
                    best_index = i;
                    best_chain = &candidate->chain;
                    best_path = candidate->path;
                    best_is_dir = candidate->is_dir;
                    best_minimum = minimum;
                }
            }

            if (!have_best) break;
            if (move_count + best_chain->len > move_cap) {
                size_t new_cap = move_cap == 0 ? best_chain->len : move_cap;
                while (new_cap < move_count + best_chain->len) new_cap *= 2;
                moves = ld_xrealloc(moves, new_cap * sizeof(*moves));
                move_cap = new_cap;
            }
            for (size_t i = 0; i < best_chain->len; i++) {
                moves[move_count++] = (RelocationMove){
                    .source = best_chain->v[i],
                    .destination = destination + (uint32_t)i,
                };
            }
            if (planned_count == planned_cap) {
                size_t new_cap = planned_cap == 0 ? 16 : planned_cap * 2;
                planned = ld_xrealloc(planned, new_cap * sizeof(*planned));
                planned_cap = new_cap;
            }
            planned[planned_count++] = (PlannedWholeObject){
                .is_dir = best_is_dir,
                .chain = best_chain,
                .path = best_path,
                .destination = destination,
            };
            if (best_is_root) selected_root = true;
            else selected_files[best_index] = true;
            destination += (uint32_t)best_chain->len;
            hole_remaining -= best_chain->len;
        }

        if (move_count != 0) {
            for (size_t i = 0; i < planned_count; i++) {
                detail_log("pack-whole: %s %s (%zu clusters) -> cluster %" PRIu32 "\n",
                           planned[i].is_dir ? "DIR" : "FILE", planned[i].path,
                           planned[i].chain->len, planned[i].destination);
            }
            relocation_execute_moves(fs, &dir_refs, journal_path, moves, move_count);
            stats.transactions++;
            stats.clusters_moved += move_count;
            stats.whole_clusters += move_count;
            stats.whole_objects += planned_count;
            fprintf(stderr,
                    "pack: moved %zu whole object%s / %zu clusters (total %zu); "
                    "terminal free run now %" PRIu64 " clusters\n",
                    planned_count, planned_count == 1 ? "" : "s", move_count,
                    stats.clusters_moved, terminal_free_clusters(fs));
            free(planned);
            free(moves);
            free(selected_files);
            u32vec_free(&root_chain);
            filelist_free(&files);
            dirreflist_free(&dir_refs);
            continue;
        }

        free(planned);
        free(moves);
        free(selected_files);

        /* A small hole immediately before a contiguous object cannot accept that
           whole object directly because the source and destination overlap. Move
           the object temporarily into the terminal free run. On the next pass its
           old allocation has merged with the hole, allowing whole-chain packing. */
        uint32_t source = hole_start + (uint32_t)hole_length;
        uint64_t terminal_count64 = terminal_free_clusters(fs);
        const U32Vec *stage_chain = NULL;
        const char *stage_path = NULL;
        bool stage_is_dir = false;
        if (source <= highest && cluster_is_movable_allocation(fs, source)) {
            if (root_chain.len != 0 && chain_contains_cluster(&root_chain, source)) {
                stage_chain = &root_chain;
                stage_path = "<root directory>";
                stage_is_dir = true;
            } else {
                for (size_t i = 0; i < files.len; i++) {
                    const FileRecord *candidate = &files.v[i];
                    if (candidate->chain.len != 0 &&
                        chain_contains_cluster(&candidate->chain, source)) {
                        stage_chain = &candidate->chain;
                        stage_path = candidate->path;
                        stage_is_dir = candidate->is_dir;
                        break;
                    }
                }
            }
        }
        if (stage_chain != NULL && stage_chain->len <= terminal_count64 &&
            stage_chain->len <= remaining) {
            uint32_t terminal_start = fs->max_cluster - (uint32_t)terminal_count64 + 1;
            moves = ld_xmalloc(stage_chain->len * sizeof(*moves));
            for (size_t i = 0; i < stage_chain->len; i++) {
                moves[i] = (RelocationMove){
                    .source = stage_chain->v[i],
                    .destination = terminal_start + (uint32_t)i,
                };
            }
            detail_log(
                    "pack-stage: %s %s (%zu clusters) -> terminal cluster %" PRIu32
                    " to expand the low free run\n",
                    stage_is_dir ? "DIR" : "FILE", stage_path, stage_chain->len,
                    terminal_start);
            relocation_execute_moves(fs, &dir_refs, journal_path, moves, stage_chain->len);
            stats.transactions++;
            stats.clusters_moved += stage_chain->len;
            stats.staged_objects++;
            stats.staged_clusters += stage_chain->len;
            fprintf(stderr,
                    "pack: staged one whole object / %zu clusters (total %zu); "
                    "terminal free run now %" PRIu64 " clusters\n",
                    stage_chain->len, stats.clusters_moved, terminal_free_clusters(fs));
            free(moves);
            u32vec_free(&root_chain);
            filelist_free(&files);
            dirreflist_free(&dir_refs);
            continue;
        }

        if (!allow_extent_fallback) {
            fprintf(stderr,
                    "pack: stopped at free run cluster %" PRIu32
                    "+%zu because no complete file or directory can be moved or "
                    "staged there without fragmenting it\n",
                    hole_start, hole_length);
            u32vec_free(&root_chain);
            filelist_free(&files);
            dirreflist_free(&dir_refs);
            break;
        }

        /* No complete chain fits and no contiguous object can be staged. Shift
           the next physical allocated extent downward without reversing or
           scattering its cluster order. */
        while (source <= highest && !cluster_is_movable_allocation(fs, source)) source++;
        if (source > highest) {
            fprintf(stderr,
                    "pack: cannot fill the free run at cluster %" PRIu32
                    " because no movable allocation follows it\n", hole_start);
            u32vec_free(&root_chain);
            filelist_free(&files);
            dirreflist_free(&dir_refs);
            break;
        }
        size_t source_run = 0;
        uint32_t c = source;
        while (c <= highest && cluster_is_movable_allocation(fs, c)) {
            source_run++;
            if (c == highest) break;
            c++;
        }
        size_t count = hole_length;
        if (count > source_run) count = source_run;
        if (count > batch_clusters) count = batch_clusters;
        if (count > remaining) count = remaining;
        if (count == 0) {
            u32vec_free(&root_chain);
            filelist_free(&files);
            dirreflist_free(&dir_refs);
            break;
        }

        moves = ld_xmalloc(count * sizeof(*moves));
        for (size_t i = 0; i < count; i++) {
            moves[i] = (RelocationMove){
                .source = source + (uint32_t)i,
                .destination = hole_start + (uint32_t)i,
            };
        }
        detail_log(
                "pack-extent: shifted %zu contiguous cluster%s from %" PRIu32
                " to %" PRIu32 " without reordering\n",
                count, count == 1 ? "" : "s", source, hole_start);
        relocation_execute_moves(fs, &dir_refs, journal_path, moves, count);
        stats.transactions++;
        stats.clusters_moved += count;
        stats.extent_clusters += count;
        stats.extent_transactions++;
        if (count == 1) stats.singleton_transactions++;
        fprintf(stderr,
                "pack: moved ordered extent of %zu cluster%s (total %zu); "
                "terminal free run now %" PRIu64 " clusters\n",
                count, count == 1 ? "" : "s", stats.clusters_moved,
                terminal_free_clusters(fs));
        free(moves);
        u32vec_free(&root_chain);
        filelist_free(&files);
        dirreflist_free(&dir_refs);
    }

    fat_relocation_update_fsinfo(fs, fat_relocation_first_free_hint(fs));
    fat32_sync(fs);
    return stats;
}




static bool cluster_range_is_free(const Fat32 *fs, uint32_t start, size_t length) {
    if (length == 0) return false;
    uint64_t end64 = (uint64_t)start + length - 1;
    if (start < 2 || end64 > fs->max_cluster) return false;
    for (size_t i = 0; i < length; i++) {
        if (!fat_is_free(fs, start + (uint32_t)i)) return false;
    }
    return true;
}

static const U32Vec *find_growth_object_chain(Fat32 *fs, const GrowthObject *object,
                                              FileList *files, U32Vec *root_chain,
                                              const FileRecord **file_out) {
    *file_out = NULL;
    if (object->is_root) {
        *root_chain = filesystem_root_chain(fs);
        return root_chain;
    }
    for (size_t i = 0; i < files->len; i++) {
        if (strcmp(files->v[i].path, object->path) == 0 &&
            files->v[i].is_dir == object->is_dir) {
            *file_out = &files->v[i];
            return &files->v[i].chain;
        }
    }
    ld_die("growth-defrag object disappeared during rescan");
    return NULL;
}

static void growth_move_chain(Fat32 *fs, const DirRefList *dir_refs,
                              const U32Vec *chain, uint32_t destination,
                              const char *journal_path, bool emit_map) {
    RelocationMove *moves = ld_xmalloc(chain->len * sizeof(*moves));
    for (size_t i = 0; i < chain->len; i++) {
        moves[i] = (RelocationMove){
            .source = chain->v[i],
            .destination = destination + (uint32_t)i,
        };
    }
    fat_relocation_execute(
        fs, dir_refs, journal_path, moves, chain->len, &g_io, detail_log
    );
    free(moves);
    if (emit_map) fat_analysis_emit_live_map_update(fs);
}

static size_t automatic_growth_batch_clusters(const Fat32 *fs) {
    size_t by_ram = g_io.ram_limit / (size_t)fs->cluster_size;
    size_t four_gb = (size_t)(UINT64_C(4) * 1024 * 1024 * 1024 / fs->cluster_size);
    if (by_ram > four_gb) by_ram = four_gb;
    if (by_ram < 4096) by_ram = 4096;
    return by_ram;
}

static size_t automatic_growth_batch_objects(void) {
    const size_t gb = (size_t)1024 * 1024 * 1024;
    if (g_io.ram_limit >= 8 * gb) return 128;
    if (g_io.ram_limit >= 2 * gb) return 64;
    return 32;
}

static bool growth_batch_can_add(const Fat32 *fs, const U32Vec *chain,
                                 uint32_t destination,
                                 const uint8_t *source_seen,
                                 const uint8_t *destination_seen) {
    if (!cluster_range_is_free(fs, destination, chain->len)) return false;
    for (size_t i = 0; i < chain->len; i++) {
        uint32_t source = chain->v[i];
        uint32_t target = destination + (uint32_t)i;
        if (source_seen[source] || destination_seen[source] ||
            source_seen[target] || destination_seen[target]) return false;
    }
    return true;
}

static GrowthStats growth_defrag_volume(Fat32 *fs, const char *journal_path,
                                        unsigned requested_percent,
                                        size_t batch_clusters) {
    GrowthStats stats = {0};
    const char *layout_name = requested_percent == 0 ? "Defragment" : "Growth Defrag";
    if (requested_percent > 25) {
        ld_die("growth reserve percentage must be between 0 and 25");
    }

    DirRefList initial_refs = {0};
    FileList initial_files = scan_files(fs, &initial_refs);
    size_t initial_largest = 0, regular_files = 0, directories = 0;
    uint64_t regular_clusters = 0;
    GrowthObjectList initial_objects = build_growth_objects(
        fs, &initial_files, &initial_largest, &regular_clusters,
        &regular_files, &directories);
    uint64_t free_before = count_free_clusters(fs);

    fprintf(stderr,
            "%s preflight (%s): checking %zu regular file%s and %zu director%s "
            "for contiguity%s.\n",
            layout_name, PROGRAM_VERSION,
            regular_files, regular_files == 1 ? "" : "s",
            directories, directories == 1 ? "y" : "ies",
            requested_percent == 0 ? " and canonical zero-gap packing" :
                                     " with a 10% post-file reserve");
    GrowthPreflight preflight = growth_layout_preflight(
        fs, &initial_files, requested_percent);
    size_t existing_reserve = preflight.reserve_clusters;
    bool canonical_verified = false;
    if (regular_files != 0 && preflight.issue == GROWTH_PREFLIGHT_OK) {
        canonical_verified = growth_layout_matches_canonical(
            fs, &initial_objects, requested_percent, &existing_reserve);
    }
    if (canonical_verified) {
        stats.already_satisfied = true;
        stats.canonical_layout_verified = canonical_verified;
        stats.applied_percent = requested_percent;
        stats.reserve_clusters = existing_reserve;
        stats.checked_files = regular_files;
        stats.checked_directories = directories;
        if (requested_percent == 0) {
            fprintf(stderr,
                    "%s preflight result: every file and directory is contiguous and the "
                    "allocation area is already packed with no internal free clusters.\n",
                    layout_name);
        } else {
            fprintf(stderr,
                    "%s preflight result: the existing FAT layout already satisfies a 10%% "
                    "post-file reserve for %zu regular file%s; %zu director%s %s contiguous.\n",
                    layout_name, regular_files, regular_files == 1 ? "" : "s",
                    directories, directories == 1 ? "y" : "ies",
                    directories == 1 ? "is" : "are");
        }
        fprintf(stderr, "No layout rewrite is required.\n");
        growth_preflight_free(&preflight);
        growth_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        return stats;
    }
    if (preflight.issue == GROWTH_PREFLIGHT_OK) {
        fprintf(stderr,
                "%s preflight result: all objects are contiguous, but the physical layout is "
                "not the canonical earliest packed layout.\n",
                requested_percent == 0 ? "Defragment" : "Growth Defrag");
        fprintf(stderr,
                "%s preflight completed read-only; no filesystem writes have occurred yet.\n",
                requested_percent == 0 ? "Defragment" : "Growth Defrag");
    } else {
        print_growth_preflight_failure(&preflight, requested_percent, layout_name);
    }
    growth_preflight_free(&preflight);

    if (initial_objects.len == 0 || regular_files == 0) {
        growth_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        fprintf(stderr, "%s: no allocated regular files require a layout rewrite\n", layout_name);
        return stats;
    }
    if (free_before <= initial_largest || regular_clusters == 0) {
        growth_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        ld_die("layout rewrite needs free space larger than the largest allocated object for safe staging");
    }
    uint64_t requested_reserve = 0;
    for (size_t i = 0; i < initial_objects.len; i++) {
        const GrowthObject *object = &initial_objects.v[i];
        if (object->is_dir) continue;
        uint64_t reserve = ((uint64_t)object->clusters * requested_percent + 99) / 100;
        requested_reserve += reserve;
    }
    if (requested_reserve > free_before - initial_largest) {
        growth_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        ld_die("layout rewrite does not have enough free clusters for the requested reserve plus a safe staging workspace");
    }
    growth_object_list_free(&initial_objects);
    filelist_free(&initial_files);
    dirreflist_free(&initial_refs);

    fprintf(stderr,
            "%s phase 1: consolidating allocation into a safe terminal workspace.\n",
            layout_name);
    size_t preparation_batch_clusters = batch_clusters;
    if (preparation_batch_clusters == 4096) {
        preparation_batch_clusters = automatic_growth_batch_clusters(fs);
    }
    fprintf(stderr,
            "%s preparation batching: up to %zu clusters per journal transaction.\n",
            layout_name, preparation_batch_clusters);
    PackingStats packing_stats = growth_prepare_volume(
        fs, journal_path, 0, preparation_batch_clusters, 0, true);
    stats.packing_clusters = packing_stats.clusters_moved;
    stats.packing_transactions = packing_stats.transactions;
    if (ld_stop_requested()) {
        stats.interrupted = true;
        fprintf(stderr,
                "%s stopped safely during preparation after moving %zu cluster%s "
                "in %zu completed transaction%s.\n",
                layout_name, packing_stats.clusters_moved,
                packing_stats.clusters_moved == 1 ? "" : "s",
                packing_stats.transactions,
                packing_stats.transactions == 1 ? "" : "s");
        fprintf(stderr,
                "The growth-space layout was not started, so no expansion gaps were applied.\n");
        return stats;
    }
    fprintf(stderr,
            "%s preparation complete: %zu cluster%s moved in %zu transaction%s.\n",
            layout_name, packing_stats.clusters_moved, packing_stats.clusters_moved == 1 ? "" : "s",
            packing_stats.transactions, packing_stats.transactions == 1 ? "" : "s");

    DirRefList refs = {0};
    FileList files = scan_files(fs, &refs);
    size_t largest = 0;
    regular_clusters = 0;
    regular_files = 0;
    directories = 0;
    GrowthObjectList objects = build_growth_objects(
        fs, &files, &largest, &regular_clusters, &regular_files, &directories);
    uint64_t terminal = terminal_free_clusters(fs);
    if (terminal < largest) {
        growth_object_list_free(&objects);
        filelist_free(&files);
        dirreflist_free(&refs);
        ld_die("growth-defrag could not create a terminal workspace large enough for the largest object");
    }
    uint32_t workspace_start = fs->max_cluster - (uint32_t)largest + 1;
    unsigned applied_percent = requested_percent;
    size_t reserve_total = 0;
    uint32_t layout_end = 1;
    if (!plan_growth_layout(fs, &objects, applied_percent, workspace_start,
                            &reserve_total, &layout_end)) {
        growth_object_list_free(&objects);
        filelist_free(&files);
        dirreflist_free(&refs);
        ld_die("growth-defrag could not fit the requested growth layout below the staging workspace");
    }
    stats.applied_percent = applied_percent;
    stats.reserve_clusters = reserve_total;
    stats.layout_started = true;
    fprintf(stderr,
            "%s phase 2: rewriting %zu regular file%s and %zu director%s into the canonical layout%s.\n",
            layout_name, regular_files, regular_files == 1 ? "" : "s",
            directories, directories == 1 ? "y" : "ies",
            applied_percent == 0 ? " with no internal free clusters" :
                                   " with a 10% post-file reserve");
    fprintf(stderr,
            "Final planned layout ends at cluster %" PRIu32 "; reusable staging workspace begins at cluster %" PRIu32 ".\n",
            layout_end, workspace_start);
    filelist_free(&files);
    dirreflist_free(&refs);

    size_t reverse = objects.len;
    size_t object_batch_limit = automatic_growth_batch_objects();
    size_t cluster_batch_limit = automatic_growth_batch_clusters(fs);
    fprintf(stderr,
            "%s layout batching: up to %zu objects or %zu clusters per journal transaction.\n",
            layout_name, object_batch_limit, cluster_batch_limit);

    while (reverse != 0) {
        if (ld_stop_requested()) {
            stats.interrupted = true;
            fprintf(stderr,
                    "%s stopped safely during layout between complete batches.\n",
                    layout_name);
            break;
        }

        DirRefList current_refs = {0};
        FileList current_files = scan_files(fs, &current_refs);
        U32Vec root_chain = {0};
        RelocationMove *batch_moves = NULL;
        size_t batch_move_count = 0;
        size_t batch_move_cap = 0;
        size_t batch_objects = 0;
        size_t batch_files = 0;
        size_t batch_directories = 0;
        size_t skipped_exact = 0;
        uint8_t *source_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        uint8_t *destination_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);

        while (reverse != 0 && batch_objects < object_batch_limit) {
            GrowthObject *object = &objects.v[reverse - 1];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_growth_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                free(source_seen);
                free(destination_seen);
                free(batch_moves);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                growth_object_list_free(&objects);
                ld_die("growth-defrag object changed size during offline operation");
            }
            if (chain_is_exact_run(chain, object->target)) {
                reverse--;
                skipped_exact++;
                u32vec_free(&local_root);
                continue;
            }
            if (batch_move_count != 0 &&
                object->clusters > cluster_batch_limit - batch_move_count) {
                u32vec_free(&local_root);
                break;
            }
            if (batch_move_count == 0 && object->clusters > cluster_batch_limit) {
                /* A single large object may exceed the normal batch cap; the copy
                   pipeline will still stream it through the RAM budget safely. */
            } else if (batch_move_count + object->clusters > cluster_batch_limit) {
                u32vec_free(&local_root);
                break;
            }
            if (!growth_batch_can_add(fs, chain, object->target,
                                      source_seen, destination_seen)) {
                u32vec_free(&local_root);
                break;
            }

            if (batch_move_count + chain->len > batch_move_cap) {
                size_t new_cap = batch_move_cap == 0 ? chain->len : batch_move_cap;
                while (new_cap < batch_move_count + chain->len) new_cap *= 2;
                batch_moves = ld_xrealloc(batch_moves, new_cap * sizeof(*batch_moves));
                batch_move_cap = new_cap;
            }
            for (size_t i = 0; i < chain->len; i++) {
                uint32_t source = chain->v[i];
                uint32_t destination = object->target + (uint32_t)i;
                batch_moves[batch_move_count++] = (RelocationMove){
                    .source = source,
                    .destination = destination,
                };
                source_seen[source] = 1;
                destination_seen[destination] = 1;
            }
            detail_log(
                "growth-place: %s %s (%zu clusters) -> cluster %" PRIu32
                " with %zu reserved cluster%s after it\n",
                object->is_dir ? "DIR" : "FILE", object->path,
                object->clusters, object->target, object->reserve_after,
                object->reserve_after == 1 ? "" : "s");
            batch_objects++;
            if (object->is_dir) batch_directories++;
            else batch_files++;
            reverse--;
            u32vec_free(&local_root);
            if (batch_move_count >= cluster_batch_limit) break;
        }

        free(source_seen);
        free(destination_seen);
        if (batch_move_count != 0) {
            relocation_execute_moves(fs, &current_refs, journal_path,
                                  batch_moves, batch_move_count);
            stats.transactions++;
            stats.clusters_copied += batch_move_count;
            stats.objects_moved += batch_objects;
            stats.files_moved += batch_files;
            stats.directories_moved += batch_directories;
            fprintf(stderr,
                    "%s layout batch: %zu object%s (%zu file%s, %zu director%s), "
                    "%zu cluster%s committed in one journal transaction; %zu object%s remain.\n",
                    layout_name, batch_objects, batch_objects == 1 ? "" : "s",
                    batch_files, batch_files == 1 ? "" : "s",
                    batch_directories, batch_directories == 1 ? "y" : "ies",
                    batch_move_count, batch_move_count == 1 ? "" : "s",
                    reverse, reverse == 1 ? "" : "s");
            free(batch_moves);
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            continue;
        }
        free(batch_moves);
        if (skipped_exact != 0) {
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            continue;
        }

        /* The next target is not currently free.  Stage this object in the
           reusable terminal workspace.  A fragmented chain can leave another
           object's clusters inside the target even after this object's own
           source has been freed.  Evacuate those blockers into the staged
           object's now-free source clusters, then place the staged object. */
        GrowthObject *object = &objects.v[reverse - 1];
        const FileRecord *current_file = NULL;
        const U32Vec *chain = find_growth_object_chain(
            fs, object, &current_files, &root_chain, &current_file);
        (void)current_file;
        if (chain->len != object->clusters) {
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            growth_object_list_free(&objects);
            ld_die("growth-defrag object changed size during offline operation");
        }
        U32Vec original_chain = {0};
        for (size_t i = 0; i < chain->len; i++) u32vec_push(&original_chain, chain->v[i]);
        if (!cluster_range_is_free(fs, workspace_start, object->clusters)) {
            u32vec_free(&original_chain);
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            growth_object_list_free(&objects);
            ld_die("growth-defrag staging workspace is unexpectedly occupied");
        }
        detail_log("growth-stage: %s %s (%zu clusters) -> workspace cluster %" PRIu32 "\n",
                   object->is_dir ? "DIR" : "FILE", object->path,
                   object->clusters, workspace_start);
        growth_move_chain(fs, &current_refs, chain, workspace_start,
                          journal_path, false);
        stats.transactions++;
        stats.clusters_copied += object->clusters;
        u32vec_free(&root_chain);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);

        current_refs = (DirRefList){0};
        current_files = scan_files(fs, &current_refs);
        root_chain = (U32Vec){0};
        current_file = NULL;
        chain = find_growth_object_chain(
            fs, object, &current_files, &root_chain, &current_file);
        if (!chain_is_exact_run(chain, workspace_start)) {
            u32vec_free(&original_chain);
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            growth_object_list_free(&objects);
            ld_die("growth-defrag staged object did not reopen at the workspace");
        }

        bool target_blockers_evacuated = false;
        if (!cluster_range_is_free(fs, object->target, object->clusters)) {
            uint64_t target_end64 = (uint64_t)object->target + object->clusters - 1;
            if (target_end64 > fs->max_cluster) {
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                growth_object_list_free(&objects);
                ld_die("growth-defrag target range exceeds the filesystem");
            }
            uint32_t target_end = (uint32_t)target_end64;
            U32Vec blocker_destinations = {0};
            for (size_t i = 0; i < original_chain.len; i++) {
                uint32_t candidate = original_chain.v[i];
                if (candidate >= object->target && candidate <= target_end) continue;
                if (!fat_is_free(fs, candidate)) {
                    u32vec_free(&blocker_destinations);
                    u32vec_free(&original_chain);
                    u32vec_free(&root_chain);
                    filelist_free(&current_files);
                    dirreflist_free(&current_refs);
                    growth_object_list_free(&objects);
                    ld_die("growth-defrag staged source cluster was unexpectedly reused");
                }
                u32vec_push(&blocker_destinations, candidate);
            }

            size_t blocker_count = 0;
            for (uint32_t cluster = object->target; cluster <= target_end; cluster++) {
                if (!fat_is_free(fs, cluster)) blocker_count++;
                if (cluster == UINT32_MAX) break;
            }
            if (blocker_count == 0 || blocker_count > blocker_destinations.len) {
                u32vec_free(&blocker_destinations);
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                growth_object_list_free(&objects);
                ld_die("growth-defrag could not allocate safe source slots for target blockers");
            }

            RelocationMove *blocker_moves = ld_xmalloc(blocker_count * sizeof(*blocker_moves));
            size_t blocker_index = 0;
            for (uint32_t cluster = object->target; cluster <= target_end; cluster++) {
                if (!fat_is_free(fs, cluster)) {
                    blocker_moves[blocker_index] = (RelocationMove){
                        .source = cluster,
                        .destination = blocker_destinations.v[blocker_index],
                    };
                    blocker_index++;
                }
                if (cluster == UINT32_MAX) break;
            }
            detail_log(
                "growth-unblock: moved %zu blocking cluster%s out of target %" PRIu32
                "-%" PRIu32 " using the staged object's released source slots\n",
                blocker_count, blocker_count == 1 ? "" : "s",
                object->target, target_end);
            relocation_execute_moves(fs, &current_refs, journal_path,
                                  blocker_moves, blocker_count);
            target_blockers_evacuated = true;
            stats.transactions++;
            stats.clusters_copied += blocker_count;
            fprintf(stderr,
                    "Growth layout cleared %zu blocking cluster%s from %s's target range.\n",
                    blocker_count, blocker_count == 1 ? "" : "s", object->path);
            free(blocker_moves);
            u32vec_free(&blocker_destinations);

            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            current_refs = (DirRefList){0};
            current_files = scan_files(fs, &current_refs);
            root_chain = (U32Vec){0};
            current_file = NULL;
            chain = find_growth_object_chain(
                fs, object, &current_files, &root_chain, &current_file);
            if (!chain_is_exact_run(chain, workspace_start)) {
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                growth_object_list_free(&objects);
                ld_die("growth-defrag staged object changed while clearing its target");
            }
            if (!cluster_range_is_free(fs, object->target, object->clusters)) {
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                growth_object_list_free(&objects);
                ld_die("growth-defrag target range remained occupied after blocker evacuation");
            }
        }

        detail_log(
            "growth-place: %s %s -> cluster %" PRIu32
            " with %zu reserved cluster%s after it\n",
            object->is_dir ? "DIR" : "FILE", object->path,
            object->target, object->reserve_after,
            object->reserve_after == 1 ? "" : "s");
        growth_move_chain(fs, &current_refs, chain, object->target,
                          journal_path, true);
        stats.transactions++;
        stats.clusters_copied += object->clusters;
        stats.objects_moved++;
        if (object->is_dir) stats.directories_moved++;
        else stats.files_moved++;
        reverse--;
        fprintf(stderr,
                "Growth layout staged one %s in %u journal transactions; %zu object%s remain.\n",
                object->is_dir ? "directory" : "file",
                target_blockers_evacuated ? 3U : 2U,
                reverse, reverse == 1 ? "" : "s");
        u32vec_free(&original_chain);
        u32vec_free(&root_chain);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
    }

    fat_relocation_update_fsinfo(fs, fat_relocation_first_free_hint(fs));
    fat32_sync(fs);
    growth_object_list_free(&objects);
    return stats;
}

static void usage(FILE *out) {
    fprintf(out,
        "Usage:\n"
        "  %s identify DEVICE\n"
        "  %s analyze DEVICE [--list]\n"
        "  %s map DEVICE [--cells N]\n"
        "  %s defrag DEVICE --write --confirm DEVICE [--journal PATH]\n"
        "       [--batch-clusters N] [--ram-buffer auto|SIZE] [--workers auto|N]\n"
        "       [--live-map-cells N] [--diagnostic-log PATH] [--verbose]\n"
        "  %s growth-defrag DEVICE --write --confirm DEVICE [--journal PATH]\n"
        "       [--growth-percent 10] [--batch-clusters N]\n"
        "       [--ram-buffer auto|SIZE] [--workers auto|N] [--live-map-cells N]\n"
        "       [--diagnostic-log PATH] [--verbose]\n"
        "  %s recover DEVICE --write --confirm DEVICE [--journal PATH]\n"
        "       [--ram-buffer auto|SIZE] [--workers auto|N]\n\n"
        "DEVICE may be an unmounted block-device partition or a regular FAT12/FAT16/FAT32 image.\n"
        "Defragment rewrites every allocated file and directory into the canonical packed\n"
        "layout: every chain is contiguous and every usable data cluster before the final\n"
        "object is allocated. Growth Defrag applies the same layout while leaving the\n"
        "requested proportional free run immediately after each regular file. Both operations\n"
        "write the unmounted FAT volume directly, journal every completed transaction and\n"
        "publish live allocation-map updates. SIZE accepts suffixes such as 512M, 2G, or 8GB.\n"
        "Ctrl-C requests a clean stop after the current journalled transaction.\n",
        PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME,
        PROGRAM_NAME);
}

static size_t parse_size(const char *s) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(s, 10U, 0U, (uint64_t)SIZE_MAX, &value))
        ld_die("invalid numeric argument");
    return (size_t)value;
}

static size_t parse_byte_size(const char *s) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno != 0 || end == s) ld_die("invalid RAM buffer size");
    char suffix[8] = {0};
    size_t suffix_len = strlen(end);
    if (suffix_len >= sizeof(suffix)) ld_die("invalid RAM buffer suffix");
    for (size_t i = 0; i < suffix_len; i++) suffix[i] = (char)toupper((unsigned char)end[i]);

    uint64_t multiplier = 1;
    if (suffix[0] == '\0' || strcmp(suffix, "B") == 0) multiplier = 1;
    else if (strcmp(suffix, "K") == 0 || strcmp(suffix, "KB") == 0) {
        multiplier = UINT64_C(1024);
    } else if (strcmp(suffix, "M") == 0 || strcmp(suffix, "MB") == 0) {
        multiplier = UINT64_C(1024) * 1024;
    } else if (strcmp(suffix, "G") == 0 || strcmp(suffix, "GB") == 0) {
        multiplier = UINT64_C(1024) * 1024 * 1024;
    } else if (strcmp(suffix, "T") == 0 || strcmp(suffix, "TB") == 0) {
        multiplier = UINT64_C(1024) * 1024 * 1024 * 1024;
    } else {
        ld_die("invalid RAM buffer suffix; use K, M, G, or T");
    }
    if (value > UINT64_MAX / multiplier) ld_die("RAM buffer size is too large");
    uint64_t bytes = (uint64_t)value * multiplier;
    if (bytes == 0 || bytes > SIZE_MAX) ld_die("RAM buffer size is outside this build's range");
    return (size_t)bytes;
}

static void close_diagnostic_log(void) {
    if (g_diagnostic_log != NULL) {
        fclose(g_diagnostic_log);
        g_diagnostic_log = NULL;
    }
}

static void print_io_configuration(void) {
    double mb = (double)g_io.ram_limit / (1024.0 * 1024.0);
    printf("RAM I/O buffer:          %.1f MB\n", mb);
    printf("Source read workers:     %zu\n", g_io.workers);
    printf("Rotational target:       %s\n", g_io.rotational ? "yes" : "no");
    printf("Serial flash target:     %s\n", g_io.serial_flash ? "yes" : "no");
}

static void print_io_statistics(void) {
    double read_mb = (double)g_io.bytes_read / (1024.0 * 1024.0);
    double write_mb = (double)g_io.bytes_written / (1024.0 * 1024.0);
    printf("Buffered data read:      %.1f MB in %" PRIu64 " extent%s\n",
           read_mb, g_io.read_extents, g_io.read_extents == 1 ? "" : "s");
    printf("Buffered data written:   %.1f MB in %" PRIu64 " extent%s\n",
           write_mb, g_io.write_extents, g_io.write_extents == 1 ? "" : "s");
}

static void emit_result_event(const char *operation, const char *status) {
    printf("@@RESULT {\"operation\":\"%s\",\"status\":\"%s\",\"message\":\"\"}\n",
           operation, status);
}

int main(int argc, char **argv) {
    ld_runtime_set_program_name(PROGRAM_NAME);
    ld_stop_clear();
    if (atexit(close_diagnostic_log) != 0) ld_die("cannot register diagnostic-log cleanup");
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
        return EXIT_SUCCESS;
    }
    if (argc < 3) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    const char *command = argv[1];
    const char *device_path = argv[2];
    bool write_flag = false;
    const char *confirm = NULL;
    const char *journal_arg = NULL;
    bool list = false;
    size_t map_cells = 4096;
    size_t live_map_cells = 0;
    size_t batch_clusters = 4096;
    unsigned growth_percent = 10;
    bool growth_percent_set = false;
    const char *ram_buffer_arg = "auto";
    const char *workers_arg = "auto";
    const char *diagnostic_log_path = NULL;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--write") == 0) write_flag = true;
        else if (strcmp(argv[i], "--list") == 0) list = true;
        else if (strcmp(argv[i], "--cells") == 0 && i + 1 < argc) {
            map_cells = parse_size(argv[++i]);
            if (map_cells == 0 || map_cells > 1048576) {
                ld_die("--cells must be between 1 and 1048576");
            }
        }
        else if (strcmp(argv[i], "--live-map-cells") == 0 && i + 1 < argc) {
            live_map_cells = parse_size(argv[++i]);
            if (live_map_cells == 0 || live_map_cells > 1048576)
                ld_die("--live-map-cells must be between 1 and 1048576");
        }
        else if (strcmp(argv[i], "--confirm") == 0 && i + 1 < argc) confirm = argv[++i];
        else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) journal_arg = argv[++i];
        else if (strcmp(argv[i], "--batch-clusters") == 0 && i + 1 < argc) {
            batch_clusters = parse_size(argv[++i]);
            if (batch_clusters == 0) ld_die("--batch-clusters must be at least 1");
        }
        else if (strcmp(argv[i], "--growth-percent") == 0 && i + 1 < argc) {
            size_t parsed = parse_size(argv[++i]);
            if (parsed != 10) ld_die("--growth-percent is fixed at 10");
            growth_percent = (unsigned)parsed;
            growth_percent_set = true;
        }
        else if (strcmp(argv[i], "--ram-buffer") == 0 && i + 1 < argc) {
            ram_buffer_arg = argv[++i];
        }
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers_arg = argv[++i];
        }
        else if (strcmp(argv[i], "--diagnostic-log") == 0 && i + 1 < argc) {
            diagnostic_log_path = argv[++i];
        }
        else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
            return EXIT_SUCCESS;
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (growth_percent_set && strcmp(command, "growth-defrag") != 0) {
        ld_die("--growth-percent is valid only with growth-defrag");
    }
    if (map_cells != 4096 && strcmp(command, "map") != 0) {
        ld_die("--cells is valid only with map");
    }
    if (live_map_cells != 0 && strcmp(command, "defrag") != 0 &&
        strcmp(command, "growth-defrag") != 0) {
        ld_die("--live-map-cells is valid only with defrag or growth-defrag");
    }
    fat_analysis_set_live_map_cells(live_map_cells);

    bool mutating = strcmp(command, "defrag") == 0 ||
                    strcmp(command, "growth-defrag") == 0 || strcmp(command, "recover") == 0;
    if (mutating && (!write_flag || confirm == NULL || strcmp(confirm, device_path) != 0)) {
        ld_die("writes require both --write and --confirm with the exact DEVICE path");
    }
    if (!mutating && strcmp(command, "identify") != 0
            && strcmp(command, "analyze") != 0 && strcmp(command, "map") != 0) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (diagnostic_log_path != NULL) {
        g_diagnostic_log = fopen(diagnostic_log_path, "a");
        if (g_diagnostic_log == NULL) ld_die_errno("open diagnostic log");
        fprintf(g_diagnostic_log, "\n=== %s %s ===\n", PROGRAM_NAME, PROGRAM_VERSION);
        fflush(g_diagnostic_log);
    }
    if (strcmp(command, "identify") == 0) {
        FatType type;
        if (!fat_geometry_probe_path(device_path, &type)) {
            ld_die("target does not contain a supported FAT12/16/32 geometry");
        }
        printf("{\"filesystem\":\"FAT%u\",\"bits\":%u}\n",
               (unsigned)type, (unsigned)type);
        return EXIT_SUCCESS;
    }
    char *journal_path = journal_arg == NULL ? default_journal_path(device_path) : ld_xstrdup(journal_arg);
    Device dev = ld_device_open(device_path, mutating);
    g_io.rotational = ld_device_is_rotational(&dev);
    g_io.serial_flash = ld_device_is_serial_flash(&dev);
    g_io.ram_limit = strcmp(ram_buffer_arg, "auto") == 0
                         ? ld_default_ram_limit()
                         : parse_byte_size(ram_buffer_arg);
    g_io.workers = strcmp(workers_arg, "auto") == 0
                       ? ld_default_worker_count(g_io.rotational, g_io.serial_flash)
                       : parse_size(workers_arg);
    if (g_io.workers == 0) ld_die("--workers must be at least 1");
    size_t cpus = ld_online_cpu_count();
    if (g_io.workers > cpus * 4) ld_die("--workers is unreasonably larger than the available CPU count");
    if (mutating) ld_stop_install_handlers();
    Fat32 fs;
    fat32_load(&fs, dev, strcmp(command, "recover") == 0);
    if (g_io.ram_limit < fs.cluster_size) g_io.ram_limit = (size_t)fs.cluster_size;

    if (mutating) print_io_configuration();

    if (strcmp(command, "recover") == 0) {
        if (!path_exists(journal_path)) ld_die("journal file does not exist");
        if (journal_has_magic(journal_path, RELOCATION_JOURNAL_MAGIC)) {
            fat_relocation_recover_mapped(
                &fs, journal_path, &g_io, detail_log
            );
        } else if (journal_has_magic(journal_path, JOURNAL_MAGIC)) {
            fat_relocation_recover_legacy(&fs, journal_path);
        } else {
            ld_die("journal has an unrecognised format");
        }
        print_io_statistics();
        emit_result_event("recover", "completed");
        fat32_unload(&fs);
        free(journal_path);
        return EXIT_SUCCESS;
    }

    if (path_exists(journal_path)) {
        ld_die("an unfinished journal exists; run recover before analysis or filesystem relocation");
    }

    DirRefList dir_refs = {0};
    bool need_dir_refs = strcmp(command, "defrag") == 0 ||
                         strcmp(command, "growth-defrag") == 0;
    FileList files = scan_files(&fs, need_dir_refs ? &dir_refs : NULL);
    fat_analysis_initialise_live_map(&fs, &files);
    if (strcmp(command, "map") == 0) {
        fat_analysis_print_map_json(&fs, &files, map_cells);
        filelist_free(&files);
        dirreflist_free(&dir_refs);
        fat32_unload(&fs);
        free(journal_path);
        return EXIT_SUCCESS;
    }
    fat_analysis_print(&fs, &files);
    if (list) fat_analysis_list_fragmented(&fs, &files);

    const char *result_operation = NULL;
    const char *result_status = NULL;
    if (strcmp(command, "defrag") == 0) {
        result_operation = "defrag";
        filelist_free(&files);
        dirreflist_free(&dir_refs);
        GrowthStats packed = growth_defrag_volume(&fs, journal_path, 0, batch_clusters);
        if (packed.already_satisfied) {
            result_status = "not-needed";
            printf("Defragment status:        Not needed; canonical packed layout verified\n");
            printf("Defragment layout check:  %zu regular file%s and %zu director%s verified\n",
                   packed.checked_files, packed.checked_files == 1 ? "" : "s",
                   packed.checked_directories,
                   packed.checked_directories == 1 ? "y" : "ies");
        } else if (packed.interrupted) {
            result_status = "stopped";
            size_t transactions = packed.transactions + packed.packing_transactions;
            printf("Defragment status:        Stopped safely after completed transactions\n");
            printf("Defragment layout I/O:    %zu clusters in %zu completed transaction%s\n",
                   packed.clusters_copied + packed.packing_clusters,
                   transactions, transactions == 1 ? "" : "s");
        } else {
            result_status = "completed";
            size_t transactions = packed.transactions + packed.packing_transactions;
            printf("Defragment status:        Completed\n");
            printf("Defragment layout phase:  %zu file%s and %zu director%s repositioned\n",
                   packed.files_moved, packed.files_moved == 1 ? "" : "s",
                   packed.directories_moved, packed.directories_moved == 1 ? "y" : "ies");
            printf("Defragment layout I/O:    %zu clusters in %zu transaction%s\n",
                   packed.clusters_copied + packed.packing_clusters,
                   transactions, transactions == 1 ? "" : "s");
        }
        files = scan_files(&fs, NULL);
        fat_analysis_print(&fs, &files);
        if (!packed.interrupted) fat_analysis_verify_layout_policy(&fs, 0);
    } else if (strcmp(command, "growth-defrag") == 0) {
        result_operation = "growth-defrag";
        filelist_free(&files);
        dirreflist_free(&dir_refs);
        GrowthStats growth = growth_defrag_volume(&fs, journal_path, growth_percent, batch_clusters);
        if (growth.already_satisfied) {
            result_status = "not-needed";
            printf("Growth Defrag status:          Not needed; layout already satisfies %u%% reserve\n",
                   growth.applied_percent);
            printf("Growth Defrag layout check:    %zu regular file%s and %zu director%s verified\n",
                   growth.checked_files, growth.checked_files == 1 ? "" : "s",
                   growth.checked_directories,
                   growth.checked_directories == 1 ? "y" : "ies");
            printf("Growth Defrag reserve verified: %zu requested clusters are free after files\n",
                   growth.reserve_clusters);
            printf("Growth Defrag changes:         None\n");
        } else if (growth.interrupted && !growth.layout_started) {
            result_status = "stopped";
            printf("Growth Defrag status:          Stopped safely during preparation\n");
            printf("Growth Defrag preparation:     %zu clusters in %zu completed transaction%s\n",
                   growth.packing_clusters, growth.packing_transactions,
                   growth.packing_transactions == 1 ? "" : "s");
            printf("Growth Defrag reserve applied: No\n");
            printf("Growth Defrag layout phase:    Not started\n");
        } else if (growth.interrupted) {
            result_status = "stopped";
            printf("Growth Defrag status:          Stopped safely during layout\n");
            printf("Growth Defrag preparation:     %zu clusters in %zu transaction%s\n",
                   growth.packing_clusters, growth.packing_transactions,
                   growth.packing_transactions == 1 ? "" : "s");
            printf("Growth Defrag planned reserve: %u%% (%zu clusters)\n",
                   growth.applied_percent, growth.reserve_clusters);
            printf("Growth Defrag layout progress: %zu file%s and %zu director%s repositioned\n",
                   growth.files_moved, growth.files_moved == 1 ? "" : "s",
                   growth.directories_moved, growth.directories_moved == 1 ? "y" : "ies");
            printf("Growth Defrag layout I/O:      %zu clusters in %zu completed transaction%s\n",
                   growth.clusters_copied, growth.transactions,
                   growth.transactions == 1 ? "" : "s");
            printf("Growth Defrag reserve applied: Partial; the requested layout is incomplete\n");
        } else {
            result_status = "completed";
            printf("Growth Defrag status:          Completed\n");
            printf("Growth Defrag applied reserve: %u%% (%zu clusters)\n",
                   growth.applied_percent, growth.reserve_clusters);
            printf("Growth Defrag packing phase:   %zu clusters in %zu transaction%s\n",
                   growth.packing_clusters, growth.packing_transactions,
                   growth.packing_transactions == 1 ? "" : "s");
            printf("Growth Defrag layout phase:    %zu file%s and %zu director%s repositioned\n",
                   growth.files_moved, growth.files_moved == 1 ? "" : "s",
                   growth.directories_moved, growth.directories_moved == 1 ? "y" : "ies");
            printf("Growth Defrag layout I/O:      %zu clusters in %zu transaction%s\n",
                   growth.clusters_copied, growth.transactions,
                   growth.transactions == 1 ? "" : "s");
        }
        files = scan_files(&fs, NULL);
        fat_analysis_print(&fs, &files);

    }
    if (mutating) print_io_statistics();
    if (result_operation != NULL && result_status != NULL) {
        emit_result_event(result_operation, result_status);
    }

    filelist_free(&files);
    dirreflist_free(&dir_refs);
    fat32_unload(&fs);
    fat_analysis_reset_live_map();
    free(journal_path);
    return ld_stop_requested() ? 130 : EXIT_SUCCESS;
}
