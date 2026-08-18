#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the dependency-safe in-place exFAT relayout implementation."""

from pathlib import Path

ROOT = Path('.')
NATIVE = ROOT / 'gui/filesystems/exfat/native'

relayout_c = r'''// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * exFAT canonical in-place relayout engine.
 *
 * The fast path mirrors the FAT12/FAT16/FAT32 design: build one canonical
 * target plan, copy the complete live object set to a durable terminal safety
 * workspace, then place the final layout directly on the source device.  The
 * allocation bitmap and directory entry sets are rebuilt while placing, and
 * the FAT/boot metadata are committed last.  A small manifest is the only
 * external journal; no full-filesystem shadow image is created.
 */

#include "exfat_native.h"

#include "ld_io.h"
#include "ld_path.h"
#include "ld_runtime.h"
#include "ld_stop.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXFAT_RELAYOUT_MAGIC "LINUX-DEFRAGGER-EXFAT-RELAYOUT-1"
#define EXFAT_RELAYOUT_MAX_BUFFER (UINT64_C(64) * 1024 * 1024)

typedef struct {
    ExfatObjectKind kind;
    uint32_t target_start;
    uint32_t staged_start;
    uint32_t clusters;
    uint32_t reserve_clusters;
    uint64_t parent_index;
    uint64_t entry_offset;
    uint32_t entry_count;
    uint64_t system_entry_offset;
    uint64_t data_length;
    uint64_t valid_length;
    bool regular_file;
    bool directory;
} ExfatRelayoutRecord;

typedef struct {
    uint32_t serial;
    uint64_t volume_bytes;
    uint32_t bytes_per_sector;
    uint32_t cluster_size;
    uint32_t cluster_count;
    uint64_t fat_offset;
    uint64_t fat_length;
    uint64_t heap_offset;
    uint32_t workspace_start;
    unsigned reserve_percent;
    ExfatRelayoutRecord *records;
    size_t record_count;
} ExfatRelayoutManifest;

typedef struct {
    size_t index;
    uint32_t target_start;
} PlacementIndex;

static int placement_index_compare(const void *left_ptr, const void *right_ptr) {
    const PlacementIndex *left = left_ptr;
    const PlacementIndex *right = right_ptr;
    if (left->target_start < right->target_start) return -1;
    if (left->target_start > right->target_start) return 1;
    if (left->index < right->index) return -1;
    if (left->index > right->index) return 1;
    return 0;
}

static void manifest_free(ExfatRelayoutManifest *manifest) {
    if (manifest == NULL) return;
    free(manifest->records);
    memset(manifest, 0, sizeof(*manifest));
}

static int ensure_directory_tree(const char *path, char **error) {
    char *copy = ld_xstrdup(path);
    size_t length = strlen(copy);
    for (size_t i = 1; i <= length; ++i) {
        if (copy[i] != '/' && copy[i] != '\0') continue;
        char saved = copy[i];
        copy[i] = '\0';
        if (copy[0] != '\0' && mkdir(copy, 0700) != 0 && errno != EEXIST) {
            exfat_set_error(error, "cannot create exFAT journal directory %s: %s",
                            copy, strerror(errno));
            free(copy);
            return -1;
        }
        copy[i] = saved;
    }
    free(copy);
    return 0;
}

static int save_manifest(const char *journal_path,
                         const ExfatRelayoutManifest *manifest,
                         char **error) {
    char *parent = ld_path_parent_directory(journal_path);
    if (ensure_directory_tree(parent, error) != 0) {
        free(parent);
        return -1;
    }
    free(parent);

    char *temporary = ld_path_append_suffix(journal_path, ".tmp");
    FILE *file = fopen(temporary, "w");
    if (file == NULL) {
        exfat_set_error(error, "cannot create exFAT relayout journal: %s",
                        strerror(errno));
        free(temporary);
        return -1;
    }
    fprintf(file, "%s\n", EXFAT_RELAYOUT_MAGIC);
    fprintf(file, "serial=%u\n", manifest->serial);
    fprintf(file, "volume_bytes=%" PRIu64 "\n", manifest->volume_bytes);
    fprintf(file, "bytes_per_sector=%u\n", manifest->bytes_per_sector);
    fprintf(file, "cluster_size=%u\n", manifest->cluster_size);
    fprintf(file, "cluster_count=%u\n", manifest->cluster_count);
    fprintf(file, "fat_offset=%" PRIu64 "\n", manifest->fat_offset);
    fprintf(file, "fat_length=%" PRIu64 "\n", manifest->fat_length);
    fprintf(file, "heap_offset=%" PRIu64 "\n", manifest->heap_offset);
    fprintf(file, "workspace_start=%u\n", manifest->workspace_start);
    fprintf(file, "reserve_percent=%u\n", manifest->reserve_percent);
    fprintf(file, "object_count=%zu\n", manifest->record_count);
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *record = &manifest->records[i];
        fprintf(file,
                "record=%u,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%u,%u\n",
                (unsigned)record->kind,
                record->target_start,
                record->staged_start,
                record->clusters,
                record->reserve_clusters,
                record->parent_index,
                record->entry_offset,
                record->entry_count,
                record->system_entry_offset,
                record->data_length,
                record->valid_length,
                record->regular_file ? 1U : 0U,
                record->directory ? 1U : 0U);
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        exfat_set_error(error, "cannot sync exFAT relayout journal: %s",
                        strerror(errno));
        unlink(temporary);
        free(temporary);
        return -1;
    }
    if (rename(temporary, journal_path) != 0) {
        exfat_set_error(error, "cannot publish exFAT relayout journal: %s",
                        strerror(errno));
        unlink(temporary);
        free(temporary);
        return -1;
    }
    free(temporary);
    ld_path_fsync_parent(journal_path);
    return 0;
}

static bool parse_u64_text(const char *text, uint64_t *value) {
    if (text == NULL || *text == '\0') return false;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_u32_text(const char *text, uint32_t *value) {
    uint64_t parsed = 0;
    if (!parse_u64_text(text, &parsed) || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static int load_manifest(const char *journal_path,
                         ExfatRelayoutManifest *manifest,
                         bool *handled,
                         char **error) {
    memset(manifest, 0, sizeof(*manifest));
    *handled = false;
    FILE *file = fopen(journal_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) return 0;
        exfat_set_error(error, "cannot open exFAT recovery journal: %s",
                        strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) {
        free(line);
        fclose(file);
        return 0;
    }
    while (*line != '\0' && (line[strlen(line) - 1U] == '\n' ||
                             line[strlen(line) - 1U] == '\r')) {
        line[strlen(line) - 1U] = '\0';
    }
    if (strcmp(line, EXFAT_RELAYOUT_MAGIC) != 0) {
        free(line);
        fclose(file);
        return 0;
    }
    *handled = true;

    size_t records_seen = 0;
    while (getline(&line, &capacity, file) >= 0) {
        while (*line != '\0' && (line[strlen(line) - 1U] == '\n' ||
                                 line[strlen(line) - 1U] == '\r')) {
            line[strlen(line) - 1U] = '\0';
        }
        char *equals = strchr(line, '=');
        if (equals == NULL) goto malformed;
        *equals++ = '\0';
        if (strcmp(line, "serial") == 0) {
            if (!parse_u32_text(equals, &manifest->serial)) goto malformed;
        } else if (strcmp(line, "volume_bytes") == 0) {
            if (!parse_u64_text(equals, &manifest->volume_bytes)) goto malformed;
        } else if (strcmp(line, "bytes_per_sector") == 0) {
            if (!parse_u32_text(equals, &manifest->bytes_per_sector)) goto malformed;
        } else if (strcmp(line, "cluster_size") == 0) {
            if (!parse_u32_text(equals, &manifest->cluster_size)) goto malformed;
        } else if (strcmp(line, "cluster_count") == 0) {
            if (!parse_u32_text(equals, &manifest->cluster_count)) goto malformed;
        } else if (strcmp(line, "fat_offset") == 0) {
            if (!parse_u64_text(equals, &manifest->fat_offset)) goto malformed;
        } else if (strcmp(line, "fat_length") == 0) {
            if (!parse_u64_text(equals, &manifest->fat_length)) goto malformed;
        } else if (strcmp(line, "heap_offset") == 0) {
            if (!parse_u64_text(equals, &manifest->heap_offset)) goto malformed;
        } else if (strcmp(line, "workspace_start") == 0) {
            if (!parse_u32_text(equals, &manifest->workspace_start)) goto malformed;
        } else if (strcmp(line, "reserve_percent") == 0) {
            uint32_t percent = 0;
            if (!parse_u32_text(equals, &percent) || percent > 25U) goto malformed;
            manifest->reserve_percent = percent;
        } else if (strcmp(line, "object_count") == 0) {
            uint64_t count = 0;
            if (!parse_u64_text(equals, &count) || count > SIZE_MAX) goto malformed;
            manifest->record_count = (size_t)count;
            manifest->records = ld_xcalloc(manifest->record_count == 0 ? 1U : manifest->record_count,
                                           sizeof(*manifest->records));
        } else if (strcmp(line, "record") == 0) {
            if (manifest->records == NULL || records_seen >= manifest->record_count) goto malformed;
            unsigned kind = 0, target = 0, staged = 0, clusters = 0, reserve = 0;
            unsigned entry_count = 0, regular = 0, directory = 0;
            unsigned long long parent = 0, entry_offset = 0, system_offset = 0;
            unsigned long long data_length = 0, valid_length = 0;
            int fields = sscanf(equals,
                                "%u,%u,%u,%u,%u,%llu,%llu,%u,%llu,%llu,%llu,%u,%u",
                                &kind, &target, &staged, &clusters, &reserve,
                                &parent, &entry_offset, &entry_count, &system_offset,
                                &data_length, &valid_length, &regular, &directory);
            if (fields != 13 || kind > (unsigned)EXFAT_OBJ_FILE || clusters == 0U) goto malformed;
            ExfatRelayoutRecord *record = &manifest->records[records_seen++];
            record->kind = (ExfatObjectKind)kind;
            record->target_start = target;
            record->staged_start = staged;
            record->clusters = clusters;
            record->reserve_clusters = reserve;
            record->parent_index = (uint64_t)parent;
            record->entry_offset = (uint64_t)entry_offset;
            record->entry_count = entry_count;
            record->system_entry_offset = (uint64_t)system_offset;
            record->data_length = (uint64_t)data_length;
            record->valid_length = (uint64_t)valid_length;
            record->regular_file = regular != 0U;
            record->directory = directory != 0U;
        } else {
            goto malformed;
        }
    }
    free(line);
    fclose(file);
    if (manifest->serial == 0U || manifest->volume_bytes == 0U ||
        manifest->bytes_per_sector == 0U || manifest->cluster_size == 0U ||
        manifest->cluster_count == 0U || manifest->fat_length == 0U ||
        manifest->workspace_start < 2U || manifest->records == NULL ||
        records_seen != manifest->record_count) {
        goto malformed_closed;
    }
    return 0;

malformed:
    free(line);
    fclose(file);
malformed_closed:
    manifest_free(manifest);
    exfat_set_error(error, "exFAT relayout recovery journal is malformed or incomplete");
    return -1;
}

static uint32_t fat_value(const ExfatVolume *volume, uint32_t cluster) {
    uint64_t offset = (uint64_t)cluster * 4U;
    if (offset + 4U > volume->fat_length) return EXFAT_BAD_CLUSTER;
    return exfat_u32(volume->fat, (size_t)offset);
}

static bool volume_has_bad_clusters(const ExfatVolume *volume) {
    for (uint32_t cluster = 2U; cluster < volume->cluster_count + 2U; ++cluster) {
        if (fat_value(volume, cluster) == EXFAT_BAD_CLUSTER) return true;
    }
    return false;
}

static bool canonical_layout(const ExfatCatalogue *catalogue,
                             const ExfatPlan *plan) {
    if (catalogue->bitmap_length != plan->bitmap_length ||
        memcmp(catalogue->bitmap, plan->expected_bitmap, plan->bitmap_length) != 0) {
        return false;
    }
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        const ExfatObject *object = &catalogue->objects.items[i];
        if (object->clusters.count == 0U) continue;
        for (size_t j = 0; j < object->clusters.count; ++j) {
            if (object->clusters.items[j] != object->target_start + (uint32_t)j) return false;
        }
        if (plan->reserve_percent != 0U && object->regular_file) {
            uint32_t start = object->target_start + (uint32_t)object->clusters.count;
            for (uint32_t r = 0; r < object->reserve_clusters; ++r) {
                if (exfat_allocated(catalogue, start + r)) return false;
            }
        }
    }
    return true;
}

static size_t object_cluster_total(const ExfatCatalogue *catalogue) {
    size_t total = 0;
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        size_t count = catalogue->objects.items[i].clusters.count;
        if (count > SIZE_MAX - total) return SIZE_MAX;
        total += count;
    }
    return total;
}

static bool terminal_workspace(const ExfatVolume *volume,
                               const ExfatCatalogue *catalogue,
                               uint32_t final_cluster,
                               size_t needed,
                               uint32_t *start_out) {
    if (needed == 0U || needed > volume->cluster_count) return false;
    uint32_t max_cluster = volume->cluster_count + 1U;
    size_t run = 0;
    for (uint32_t cluster = max_cluster; cluster >= 2U; --cluster) {
        if (exfat_allocated(catalogue, cluster) ||
            fat_value(volume, cluster) == EXFAT_BAD_CLUSTER) {
            break;
        }
        run++;
        if (run >= needed) break;
        if (cluster == 2U) break;
    }
    if (run < needed) return false;
    uint32_t start = max_cluster - (uint32_t)needed + 1U;
    if (start <= final_cluster) return false;
    *start_out = start;
    return true;
}

static size_t choose_buffer_bytes(uint32_t cluster_size, size_t ram_bytes,
                                  size_t batch_clusters) {
    if (ram_bytes == 0U) ram_bytes = ld_default_ram_limit();
    uint64_t limit = ram_bytes;
    if (limit > EXFAT_RELAYOUT_MAX_BUFFER) limit = EXFAT_RELAYOUT_MAX_BUFFER;
    if (batch_clusters != 0U) {
        uint64_t batch = (uint64_t)batch_clusters * cluster_size;
        if (batch != 0U && batch < limit) limit = batch;
    }
    if (limit < cluster_size) limit = cluster_size;
    limit -= limit % cluster_size;
    if (limit == 0U) limit = cluster_size;
    if (limit > SIZE_MAX) limit = SIZE_MAX - (SIZE_MAX % cluster_size);
    return (size_t)limit;
}

static int copy_range(int fd, uint64_t source, uint64_t destination,
                      uint64_t length, uint8_t *buffer, size_t buffer_bytes,
                      bool honour_stop, bool *stopped, char **error) {
    uint64_t done = 0;
    while (done < length) {
        size_t chunk = (size_t)((length - done) < buffer_bytes
                                    ? (length - done) : buffer_bytes);
        if (ld_pread_full(fd, buffer, chunk, source + done) != (ssize_t)chunk ||
            ld_pwrite_full(fd, buffer, chunk, destination + done) != (ssize_t)chunk) {
            exfat_set_error(error, "short exFAT relayout copy at byte %" PRIu64,
                            source + done);
            return -1;
        }
        done += chunk;
        if (honour_stop && ld_stop_requested()) {
            *stopped = true;
            return 0;
        }
    }
    return 0;
}

static int copy_object_to_run(ExfatVolume *volume, const ExfatObject *object,
                              uint32_t target_start, uint8_t *buffer,
                              size_t buffer_bytes, bool honour_stop,
                              bool *stopped, char **error) {
    size_t position = 0;
    while (position < object->clusters.count) {
        size_t run = 1;
        while (position + run < object->clusters.count &&
               object->clusters.items[position + run] ==
                   object->clusters.items[position] + (uint32_t)run) {
            run++;
        }
        uint64_t source = exfat_cluster_offset(volume, object->clusters.items[position]);
        uint64_t destination = exfat_cluster_offset(volume,
                                                    target_start + (uint32_t)position);
        uint64_t bytes = (uint64_t)run * volume->cluster_size;
        if (copy_range(volume->fd, source, destination, bytes, buffer, buffer_bytes,
                       honour_stop, stopped, error) != 0 || *stopped) {
            return *stopped ? 0 : -1;
        }
        position += run;
    }
    return 0;
}

static int copy_run_to_object(ExfatVolume *volume, uint32_t source_start,
                              const ExfatObject *object, uint8_t *buffer,
                              size_t buffer_bytes, char **error) {
    for (size_t position = 0; position < object->clusters.count;) {
        size_t run = 1;
        while (position + run < object->clusters.count &&
               object->clusters.items[position + run] ==
                   object->clusters.items[position] + (uint32_t)run) {
            run++;
        }
        uint64_t source = exfat_cluster_offset(volume,
                                               source_start + (uint32_t)position);
        uint64_t destination = exfat_cluster_offset(volume,
                                                    object->clusters.items[position]);
        uint64_t bytes = (uint64_t)run * volume->cluster_size;
        bool stopped = false;
        if (copy_range(volume->fd, source, destination, bytes, buffer, buffer_bytes,
                       false, &stopped, error) != 0) {
            return -1;
        }
        position += run;
    }
    return 0;
}

static void bitmap_set(uint8_t *bitmap, uint32_t cluster, bool allocated) {
    uint32_t bit = cluster - 2U;
    uint8_t mask = (uint8_t)(1U << (bit & 7U));
    if (allocated) bitmap[bit >> 3U] |= mask;
    else bitmap[bit >> 3U] &= (uint8_t)~mask;
}

static int build_expected_bitmap(const ExfatRelayoutManifest *manifest,
                                 size_t bitmap_length,
                                 uint8_t **bitmap_out,
                                 char **error) {
    uint8_t *bitmap = calloc(bitmap_length == 0U ? 1U : bitmap_length, 1U);
    if (bitmap == NULL) {
        exfat_set_error(error, "allocating exFAT canonical bitmap failed");
        return -1;
    }
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *record = &manifest->records[i];
        uint64_t end = (uint64_t)record->target_start + record->clusters;
        if (record->target_start < 2U ||
            end > (uint64_t)manifest->cluster_count + 2U) {
            free(bitmap);
            exfat_set_error(error, "exFAT canonical object target exceeds the heap");
            return -1;
        }
        for (uint32_t j = 0; j < record->clusters; ++j) {
            uint32_t cluster = record->target_start + j;
            uint32_t bit = cluster - 2U;
            if ((uint64_t)bit >= bitmap_length * 8ULL) {
                free(bitmap);
                exfat_set_error(error, "exFAT canonical bitmap target is out of bounds");
                return -1;
            }
            bitmap_set(bitmap, cluster, true);
        }
    }
    *bitmap_out = bitmap;
    return 0;
}

static const ExfatRelayoutRecord *find_kind(const ExfatRelayoutManifest *manifest,
                                             ExfatObjectKind kind) {
    for (size_t i = 0; i < manifest->record_count; ++i) {
        if (manifest->records[i].kind == kind) return &manifest->records[i];
    }
    return NULL;
}

static void patch_entry_set_checksum(uint8_t *payload, size_t offset,
                                     uint32_t count) {
    exfat_put_u16(payload, offset + 2U,
                  exfat_entry_checksum(payload + offset, (size_t)count * 32U));
}

static int patch_directory_payload(const ExfatRelayoutManifest *manifest,
                                   size_t directory_index,
                                   uint8_t *payload, size_t length,
                                   char **error) {
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *child = &manifest->records[i];
        if (child->parent_index != directory_index ||
            child->kind == EXFAT_OBJ_BITMAP || child->kind == EXFAT_OBJ_UPCASE ||
            child->kind == EXFAT_OBJ_ROOT) {
            continue;
        }
        if (child->entry_count < 2U || child->entry_offset == UINT64_MAX ||
            child->entry_offset + (uint64_t)child->entry_count * 32U > length) {
            exfat_set_error(error, "missing exFAT directory reference during relayout");
            return -1;
        }
        size_t stream = SIZE_MAX;
        for (uint32_t entry = 1U; entry < child->entry_count; ++entry) {
            size_t candidate = (size_t)child->entry_offset + (size_t)entry * 32U;
            if (payload[candidate] == 0xc0U) {
                stream = candidate;
                break;
            }
        }
        if (stream == SIZE_MAX) {
            exfat_set_error(error, "exFAT stream extension disappeared during relayout");
            return -1;
        }
        exfat_put_u32(payload, stream + 20U, child->target_start);
        payload[stream + 1U] |= 0x02U;
        patch_entry_set_checksum(payload, (size_t)child->entry_offset,
                                 child->entry_count);
    }

    if (manifest->records[directory_index].kind == EXFAT_OBJ_ROOT) {
        const ExfatRelayoutRecord *bitmap = find_kind(manifest, EXFAT_OBJ_BITMAP);
        const ExfatRelayoutRecord *upcase = find_kind(manifest, EXFAT_OBJ_UPCASE);
        if (bitmap == NULL || upcase == NULL ||
            bitmap->system_entry_offset == UINT64_MAX ||
            upcase->system_entry_offset == UINT64_MAX ||
            bitmap->system_entry_offset + 32U > length ||
            upcase->system_entry_offset + 32U > length) {
            exfat_set_error(error, "exFAT root system entries are unavailable during relayout");
            return -1;
        }
        exfat_put_u32(payload, (size_t)bitmap->system_entry_offset + 20U,
                      bitmap->target_start);
        exfat_put_u32(payload, (size_t)upcase->system_entry_offset + 20U,
                      upcase->target_start);
    }
    return 0;
}

static int write_buffer_to_run(ExfatVolume *volume, uint32_t target_start,
                               const uint8_t *payload, size_t length,
                               char **error) {
    uint64_t offset = exfat_cluster_offset(volume, target_start);
    if (ld_pwrite_full(volume->fd, payload, length, offset) != (ssize_t)length) {
        exfat_set_error(error, "short exFAT canonical payload write");
        return -1;
    }
    return 0;
}

static int place_record(ExfatVolume *volume,
                        const ExfatRelayoutManifest *manifest,
                        size_t index, uint8_t *copy_buffer,
                        size_t copy_buffer_bytes, bool honour_stop,
                        bool *stopped, char **error) {
    const ExfatRelayoutRecord *record = &manifest->records[index];
    uint64_t allocation64 = (uint64_t)record->clusters * volume->cluster_size;
    if (allocation64 > SIZE_MAX) {
        exfat_set_error(error, "exFAT relayout object exceeds addressable memory");
        return -1;
    }

    if (record->kind != EXFAT_OBJ_BITMAP && !record->directory) {
        return copy_range(volume->fd,
                          exfat_cluster_offset(volume, record->staged_start),
                          exfat_cluster_offset(volume, record->target_start),
                          allocation64, copy_buffer, copy_buffer_bytes,
                          honour_stop, stopped, error);
    }

    size_t allocation = (size_t)allocation64;
    uint8_t *payload = calloc(allocation == 0U ? 1U : allocation, 1U);
    if (payload == NULL) {
        exfat_set_error(error, "allocating exFAT relayout metadata payload failed");
        return -1;
    }
    if (record->kind == EXFAT_OBJ_BITMAP) {
        uint8_t *bitmap = NULL;
        if (build_expected_bitmap(manifest, (size_t)record->data_length,
                                  &bitmap, error) != 0) {
            free(payload);
            return -1;
        }
        if (record->data_length > allocation64) {
            free(bitmap);
            free(payload);
            exfat_set_error(error, "exFAT allocation bitmap exceeds its allocation");
            return -1;
        }
        memcpy(payload, bitmap, (size_t)record->data_length);
        free(bitmap);
    } else {
        if (ld_pread_full(volume->fd, payload, allocation,
                          exfat_cluster_offset(volume, record->staged_start)) !=
            (ssize_t)allocation) {
            free(payload);
            exfat_set_error(error, "short exFAT staged directory read");
            return -1;
        }
        if (patch_directory_payload(manifest, index, payload, allocation, error) != 0) {
            free(payload);
            return -1;
        }
    }
    int result = write_buffer_to_run(volume, record->target_start,
                                     payload, allocation, error);
    free(payload);
    if (result == 0 && honour_stop && ld_stop_requested()) *stopped = true;
    return result;
}

static int build_final_fat(const ExfatVolume *volume,
                           const ExfatRelayoutManifest *manifest,
                           uint8_t **fat_out, char **error) {
    if (volume->fat_length > SIZE_MAX) {
        exfat_set_error(error, "exFAT FAT is too large");
        return -1;
    }
    uint8_t *fat = calloc((size_t)volume->fat_length, 1U);
    if (fat == NULL) {
        exfat_set_error(error, "allocating exFAT canonical FAT failed");
        return -1;
    }
    if (volume->fat_length >= 8U) memcpy(fat, volume->fat, 8U);
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *record = &manifest->records[i];
        if (record->kind != EXFAT_OBJ_BITMAP && record->kind != EXFAT_OBJ_UPCASE &&
            record->kind != EXFAT_OBJ_ROOT) {
            continue;
        }
        for (uint32_t j = 0; j < record->clusters; ++j) {
            uint32_t cluster = record->target_start + j;
            uint64_t offset = (uint64_t)cluster * 4U;
            if (offset + 4U > volume->fat_length) {
                free(fat);
                exfat_set_error(error, "exFAT FAT target exceeds the FAT region");
                return -1;
            }
            uint32_t value = j + 1U < record->clusters ? cluster + 1U : EXFAT_EOC;
            exfat_put_u32(fat, (size_t)offset, value);
        }
    }
    *fat_out = fat;
    return 0;
}

static void fill_boot_checksum_sector(uint8_t *region, uint32_t bytes_per_sector) {
    uint32_t checksum = exfat_boot_checksum(region, bytes_per_sector);
    uint8_t *sector = region + (size_t)11U * bytes_per_sector;
    for (uint32_t offset = 0; offset + 4U <= bytes_per_sector; offset += 4U) {
        exfat_put_u32(sector, offset, checksum);
    }
}

static int write_boot_state(ExfatVolume *volume, uint32_t root_cluster,
                            bool dirty, uint8_t percent_in_use,
                            char **error) {
    size_t region_bytes = (size_t)12U * volume->bytes_per_sector;
    size_t total_bytes = region_bytes * 2U;
    uint8_t *boot = ld_xmalloc(total_bytes);
    memcpy(boot, volume->boot_regions, total_bytes);
    for (size_t copy = 0; copy < 2U; ++copy) {
        uint8_t *base = boot + copy * region_bytes;
        exfat_put_u32(base, 96U, root_cluster);
        uint16_t flags = exfat_u16(base, 106U);
        if (dirty) flags |= EXFAT_VOLUME_DIRTY;
        else flags &= (uint16_t)~EXFAT_VOLUME_DIRTY;
        exfat_put_u16(base, 106U, flags);
        base[112U] = percent_in_use;
        fill_boot_checksum_sector(base, volume->bytes_per_sector);
    }
    if (ld_pwrite_full(volume->fd, boot + region_bytes, region_bytes,
                       region_bytes) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0 ||
        ld_pwrite_full(volume->fd, boot, region_bytes, 0) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0) {
        free(boot);
        exfat_set_error(error, "cannot publish exFAT boot-region transaction state");
        return -1;
    }
    free(boot);
    return 0;
}

static int restore_original_boot(ExfatVolume *volume, char **error) {
    size_t region_bytes = (size_t)12U * volume->bytes_per_sector;
    if (ld_pwrite_full(volume->fd, volume->boot_regions + region_bytes,
                       region_bytes, region_bytes) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0 ||
        ld_pwrite_full(volume->fd, volume->boot_regions,
                       region_bytes, 0) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0) {
        exfat_set_error(error, "cannot restore the original exFAT boot region");
        return -1;
    }
    return 0;
}

static uint8_t percent_in_use(const ExfatRelayoutManifest *manifest) {
    uint64_t allocated = 0;
    for (size_t i = 0; i < manifest->record_count; ++i) {
        allocated += manifest->records[i].clusters;
    }
    uint64_t percent = manifest->cluster_count == 0U ? 0U
        : allocated * 100U / manifest->cluster_count;
    if (percent > 100U) percent = 100U;
    return (uint8_t)percent;
}

static int commit_final_metadata(ExfatVolume *volume,
                                 const ExfatRelayoutManifest *manifest,
                                 char **error) {
    uint8_t *fat = NULL;
    if (build_final_fat(volume, manifest, &fat, error) != 0) return -1;
    if (ld_pwrite_full(volume->fd, fat, (size_t)volume->fat_length,
                       volume->fat_offset) != (ssize_t)volume->fat_length ||
        fsync(volume->fd) != 0) {
        free(fat);
        exfat_set_error(error, "cannot commit the canonical exFAT FAT");
        return -1;
    }
    free(fat);
    const ExfatRelayoutRecord *root = find_kind(manifest, EXFAT_OBJ_ROOT);
    if (root == NULL) {
        exfat_set_error(error, "canonical exFAT root record is missing");
        return -1;
    }
    return write_boot_state(volume, root->target_start, false,
                            percent_in_use(manifest), error);
}

static void emit_live_reset(const ExfatVolume *volume,
                            const ExfatCatalogue *catalogue) {
    printf("@@LIVE_RESET {\"unit_size\":%u,\"filesystem_units\":%u,\"used_ranges\":[",
           volume->cluster_size, volume->cluster_count);
    bool first = true;
    uint32_t cluster = 2U;
    while (cluster < volume->cluster_count + 2U) {
        if (!exfat_allocated(catalogue, cluster)) {
            cluster++;
            continue;
        }
        uint32_t start = cluster;
        while (cluster < volume->cluster_count + 2U &&
               exfat_allocated(catalogue, cluster)) {
            cluster++;
        }
        if (!first) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 "]",
               (uint64_t)(start - 2U) * volume->cluster_size,
               (uint64_t)(cluster - start) * volume->cluster_size);
        first = false;
    }
    fputs("]}\n", stdout);
    fflush(stdout);
}

static int verify_final_layout(const char *device, unsigned reserve_percent,
                               bool live_updates, char **error) {
    ExfatVolume volume;
    ExfatCatalogue catalogue;
    ExfatPlan plan;
    if (exfat_scan(device, false, &volume, &catalogue, error) != 0) return -1;
    if (exfat_build_plan(&volume, &catalogue, reserve_percent, &plan, error) != 0) {
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&volume);
        return -1;
    }
    bool verified = canonical_layout(&catalogue, &plan);
    if (verified && live_updates) emit_live_reset(&volume, &catalogue);
    size_t objects = catalogue.objects.count;
    uint64_t files = catalogue.regular_files;
    uint64_t directories = catalogue.directories;
    exfat_plan_free(&plan);
    exfat_catalogue_free(&catalogue);
    exfat_close_volume(&volume);
    if (!verified) {
        exfat_set_error(error, "final exFAT layout differs from the canonical relayout plan");
        return -1;
    }
    printf("Layout verification:      %zu exFAT objects (%" PRIu64
           " files, %" PRIu64 " directories) contiguous; canonical %u%% policy verified.\n",
           objects, files, directories, reserve_percent);
    return 0;
}

static void populate_manifest(ExfatRelayoutManifest *manifest,
                              const ExfatVolume *volume,
                              const ExfatCatalogue *catalogue,
                              unsigned reserve_percent,
                              uint32_t workspace_start) {
    memset(manifest, 0, sizeof(*manifest));
    manifest->serial = volume->serial;
    manifest->volume_bytes = volume->volume_bytes;
    manifest->bytes_per_sector = volume->bytes_per_sector;
    manifest->cluster_size = volume->cluster_size;
    manifest->cluster_count = volume->cluster_count;
    manifest->fat_offset = volume->fat_offset;
    manifest->fat_length = volume->fat_length;
    manifest->heap_offset = volume->heap_offset;
    manifest->workspace_start = workspace_start;
    manifest->reserve_percent = reserve_percent;
    manifest->record_count = catalogue->objects.count;
    manifest->records = ld_xcalloc(manifest->record_count == 0U ? 1U : manifest->record_count,
                                   sizeof(*manifest->records));
    uint32_t staged = workspace_start;
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatObject *object = &catalogue->objects.items[i];
        ExfatRelayoutRecord *record = &manifest->records[i];
        record->kind = object->kind;
        record->target_start = object->target_start;
        record->staged_start = staged;
        record->clusters = (uint32_t)object->clusters.count;
        record->reserve_clusters = object->reserve_clusters;
        record->parent_index = object->parent_index == SIZE_MAX
                                   ? UINT64_MAX : (uint64_t)object->parent_index;
        record->entry_offset = object->entry_offset;
        record->entry_count = object->entry_count;
        record->system_entry_offset = object->system_entry_offset;
        record->data_length = object->data_length;
        record->valid_length = object->valid_length;
        record->regular_file = object->regular_file;
        record->directory = object->directory;
        staged += record->clusters;
    }
}

static int rollback_original_layout(ExfatVolume *volume,
                                    const ExfatCatalogue *catalogue,
                                    const ExfatRelayoutManifest *manifest,
                                    uint8_t *buffer, size_t buffer_bytes,
                                    char **error) {
    fprintf(stderr,
            "Stop requested during final placement; restoring the original exFAT layout from the durable terminal workspace.\n");
    fflush(stderr);
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        if (copy_run_to_object(volume, manifest->records[i].staged_start,
                               &catalogue->objects.items[i], buffer,
                               buffer_bytes, error) != 0) {
            return -1;
        }
    }
    if (fsync(volume->fd) != 0 || restore_original_boot(volume, error) != 0) {
        if (error != NULL && *error == NULL) {
            exfat_set_error(error, "cannot sync restored exFAT source layout");
        }
        return -1;
    }
    return 0;
}

static int execute_manifest_layout(ExfatVolume *volume,
                                   const ExfatRelayoutManifest *manifest,
                                   size_t ram_bytes, size_t batch_clusters,
                                   bool honour_stop, bool *stopped,
                                   char **error) {
    size_t buffer_bytes = choose_buffer_bytes(volume->cluster_size,
                                              ram_bytes, batch_clusters);
    uint8_t *buffer = ld_xmalloc(buffer_bytes);
    PlacementIndex *order = ld_xmalloc(manifest->record_count * sizeof(*order));
    for (size_t i = 0; i < manifest->record_count; ++i) {
        order[i] = (PlacementIndex){.index = i,
                                  .target_start = manifest->records[i].target_start};
    }
    qsort(order, manifest->record_count, sizeof(*order), placement_index_compare);
    for (size_t position = 0; position < manifest->record_count; ++position) {
        if (place_record(volume, manifest, order[position].index,
                         buffer, buffer_bytes, honour_stop,
                         stopped, error) != 0 || *stopped) {
            free(order);
            free(buffer);
            return *stopped ? 0 : -1;
        }
    }
    free(order);
    free(buffer);
    if (fsync(volume->fd) != 0) {
        exfat_set_error(error, "cannot sync canonical exFAT data placement");
        return -1;
    }
    return 0;
}

int exfat_relayout_in_place(const char *device, const char *journal_path,
                            unsigned reserve_percent, size_t ram_bytes,
                            size_t batch_clusters, bool live_updates,
                            ExfatRelayoutStats *stats, char **error) {
    memset(stats, 0, sizeof(*stats));
    if (reserve_percent > 25U) {
        exfat_set_error(error, "exFAT reserve percentage must be between 0 and 25");
        return EXFAT_RELAYOUT_FAILED;
    }

    ExfatVolume source;
    ExfatCatalogue catalogue;
    ExfatPlan plan;
    if (exfat_scan(device, false, &source, &catalogue, error) != 0) {
        return EXFAT_RELAYOUT_FAILED;
    }
    if (exfat_build_plan(&source, &catalogue, reserve_percent, &plan, error) != 0) {
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_FAILED;
    }
    if (canonical_layout(&catalogue, &plan)) {
        printf("Not needed; canonical exFAT layout with %u%% post-file reserve policy verified.\n",
               reserve_percent);
        exfat_plan_free(&plan);
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_NOT_NEEDED;
    }

    size_t total_clusters = object_cluster_total(&catalogue);
    if (total_clusters == 0U || total_clusters == SIZE_MAX ||
        volume_has_bad_clusters(&source)) {
        exfat_plan_free(&plan);
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_FALLBACK;
    }
    uint32_t workspace_start = 0;
    if (!terminal_workspace(&source, &catalogue, plan.final_cluster,
                            total_clusters, &workspace_start)) {
        exfat_plan_free(&plan);
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_FALLBACK;
    }
    if (ram_bytes == 0U) ram_bytes = ld_default_ram_limit();
    for (size_t i = 0; i < catalogue.objects.count; ++i) {
        const ExfatObject *object = &catalogue.objects.items[i];
        if ((object->directory || object->kind == EXFAT_OBJ_BITMAP) &&
            (uint64_t)object->clusters.count * source.cluster_size > ram_bytes) {
            exfat_plan_free(&plan);
            exfat_catalogue_free(&catalogue);
            exfat_close_volume(&source);
            return EXFAT_RELAYOUT_FALLBACK;
        }
    }

    printf("Raw userspace native-C exFAT relayout engine %s\n", LD_VERSION);
    printf("exFAT relayout preflight: %zu objects, %zu allocated clusters; canonical reserve policy %u%%.\n",
           catalogue.objects.count, total_clusters, reserve_percent);
    printf("exFAT phase 1: staging the complete live object set into a %zu-cluster terminal safety workspace beginning at cluster %u.\n",
           total_clusters, workspace_start);
    printf("exFAT RAM copy buffer:    %.1f MiB\n",
           (double)choose_buffer_bytes(source.cluster_size, ram_bytes,
                                       batch_clusters) / (1024.0 * 1024.0));
    fflush(stdout);

    exfat_plan_free(&plan);
    exfat_close_volume(&source);

    ExfatVolume volume;
    if (exfat_open_volume(device, true, false, &volume, error) != 0) {
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    size_t buffer_bytes = choose_buffer_bytes(volume.cluster_size,
                                              ram_bytes, batch_clusters);
    uint8_t *buffer = ld_xmalloc(buffer_bytes);
    ExfatRelayoutManifest manifest;
    populate_manifest(&manifest, &volume, &catalogue, reserve_percent,
                      workspace_start);

    bool stopped = false;
    for (size_t i = 0; i < catalogue.objects.count; ++i) {
        if (copy_object_to_run(&volume, &catalogue.objects.items[i],
                               manifest.records[i].staged_start,
                               buffer, buffer_bytes, true,
                               &stopped, error) != 0) {
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_FAILED;
        }
        if (stopped) {
            fprintf(stderr,
                    "exFAT relayout stopped safely during workspace staging; the source filesystem was never modified.\n");
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_STOPPED;
        }
    }
    if (fsync(volume.fd) != 0) {
        exfat_set_error(error, "cannot sync the durable exFAT terminal workspace");
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    stats->workspace_clusters = total_clusters;
    stats->staged_clusters = total_clusters;
    printf("exFAT workspace staging complete: %zu clusters copied in one durable pass; original metadata is still unchanged.\n",
           total_clusters);
    fflush(stdout);

    if (save_manifest(journal_path, &manifest, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    if (ld_stop_requested()) {
        unlink(journal_path);
        ld_path_fsync_parent(journal_path);
        fprintf(stderr,
                "exFAT relayout stopped safely before source placement; the original filesystem is unchanged.\n");
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_STOPPED;
    }

    if (write_boot_state(&volume, volume.root_cluster, true,
                         volume.percent_in_use, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    printf("exFAT phase 2: placing the canonical layout directly from the durable terminal workspace; no full-filesystem working image is used.\n");
    fflush(stdout);

    if (execute_manifest_layout(&volume, &manifest, ram_bytes,
                                batch_clusters, true, &stopped, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    if (stopped) {
        if (rollback_original_layout(&volume, &catalogue, &manifest,
                                     buffer, buffer_bytes, error) != 0) {
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_FAILED;
        }
        unlink(journal_path);
        ld_path_fsync_parent(journal_path);
        fprintf(stderr,
                "exFAT relayout stopped safely; the original allocation layout was restored.\n");
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_STOPPED;
    }
    stats->placed_clusters = total_clusters;

    printf("exFAT phase 3: committing the canonical FAT and boot metadata.\n");
    fflush(stdout);
    if (commit_final_metadata(&volume, &manifest, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    free(buffer);
    exfat_close_volume(&volume);
    exfat_catalogue_free(&catalogue);

    if (verify_final_layout(device, reserve_percent, live_updates, error) != 0) {
        manifest_free(&manifest);
        return EXFAT_RELAYOUT_FAILED;
    }
    unlink(journal_path);
    ld_path_fsync_parent(journal_path);
    stats->objects_repositioned = manifest.record_count;
    stats->completed = true;
    printf("exFAT unified workspace layout: %zu objects, %zu clusters, two direct data passes plus one metadata commit.\n",
           manifest.record_count, total_clusters);
    printf("exFAT relayout completed with serial and full volume capacity preserved.\n");
    manifest_free(&manifest);
    return EXFAT_RELAYOUT_COMPLETED;
}

int exfat_relayout_recover(const char *device, const char *journal_path,
                           size_t ram_bytes, size_t batch_clusters,
                           bool live_updates, bool *handled, char **error) {
    ExfatRelayoutManifest manifest;
    int loaded = load_manifest(journal_path, &manifest, handled, error);
    if (loaded != 0 || !*handled) return loaded != 0 ? 1 : 0;

    ExfatVolume volume;
    if (exfat_open_volume(device, true, true, &volume, error) != 0) {
        manifest_free(&manifest);
        return 1;
    }
    if (volume.serial != manifest.serial ||
        volume.volume_bytes != manifest.volume_bytes ||
        volume.bytes_per_sector != manifest.bytes_per_sector ||
        volume.cluster_size != manifest.cluster_size ||
        volume.cluster_count != manifest.cluster_count ||
        volume.fat_offset != manifest.fat_offset ||
        volume.fat_length != manifest.fat_length ||
        volume.heap_offset != manifest.heap_offset) {
        exfat_close_volume(&volume);
        manifest_free(&manifest);
        exfat_set_error(error, "exFAT relayout journal belongs to a different target geometry");
        return 1;
    }

    fprintf(stderr,
            "Recovering exFAT canonical relayout from the durable terminal workspace; placement is idempotently replayed.\n");
    fflush(stderr);
    bool stopped = false;
    if (execute_manifest_layout(&volume, &manifest, ram_bytes,
                                batch_clusters, false, &stopped, error) != 0 ||
        commit_final_metadata(&volume, &manifest, error) != 0) {
        exfat_close_volume(&volume);
        manifest_free(&manifest);
        return 1;
    }
    exfat_close_volume(&volume);
    if (verify_final_layout(device, manifest.reserve_percent,
                            live_updates, error) != 0) {
        manifest_free(&manifest);
        return 1;
    }
    unlink(journal_path);
    ld_path_fsync_parent(journal_path);
    puts("exFAT relayout recovery completed successfully.");
    manifest_free(&manifest);
    return 0;
}
'''

(NATIVE / 'exfat_relayout.c').write_text(relayout_c, encoding='utf-8')

header = (NATIVE / 'exfat_native.h').read_text(encoding='utf-8')
header = header.replace(
    'typedef struct {\n    uint8_t *expected_bitmap;\n    size_t bitmap_length;\n    uint32_t final_cluster;\n    bool growth;\n} ExfatPlan;',
    'typedef struct {\n    uint8_t *expected_bitmap;\n    size_t bitmap_length;\n    uint32_t final_cluster;\n    unsigned reserve_percent;\n} ExfatPlan;'
)
header = header.replace(
    'int exfat_build_plan(ExfatVolume *volume, ExfatCatalogue *catalogue, bool growth, ExfatPlan *plan, char **error);',
    'int exfat_build_plan(ExfatVolume *volume, ExfatCatalogue *catalogue, unsigned reserve_percent, ExfatPlan *plan, char **error);'
)
insert = r'''

typedef enum {
    EXFAT_RELAYOUT_FAILED = -1,
    EXFAT_RELAYOUT_FALLBACK = 0,
    EXFAT_RELAYOUT_COMPLETED = 1,
    EXFAT_RELAYOUT_NOT_NEEDED = 2,
    EXFAT_RELAYOUT_STOPPED = 3,
} ExfatRelayoutResult;

typedef struct {
    size_t workspace_clusters;
    size_t staged_clusters;
    size_t placed_clusters;
    size_t objects_repositioned;
    bool completed;
} ExfatRelayoutStats;

int exfat_relayout_in_place(const char *device, const char *journal_path,
                            unsigned reserve_percent, size_t ram_bytes,
                            size_t batch_clusters, bool live_updates,
                            ExfatRelayoutStats *stats, char **error);
int exfat_relayout_recover(const char *device, const char *journal_path,
                           size_t ram_bytes, size_t batch_clusters,
                           bool live_updates, bool *handled, char **error);
'''
header = header.replace('\n#endif\n', insert + '\n#endif\n')
(NATIVE / 'exfat_native.h').write_text(header, encoding='utf-8')

plan_path = NATIVE / 'exfat_plan.c'
plan = plan_path.read_text(encoding='utf-8')
plan = plan.replace(
    'int exfat_build_plan(ExfatVolume *volume, ExfatCatalogue *catalogue,\n                     bool growth, ExfatPlan *plan, char **error) {',
    'int exfat_build_plan(ExfatVolume *volume, ExfatCatalogue *catalogue,\n                     unsigned reserve_percent, ExfatPlan *plan, char **error) {'
)
plan = plan.replace('    plan->growth = growth;\n', '    plan->reserve_percent = reserve_percent;\n')
plan = plan.replace(
    '        object->reserve_clusters = growth && object->regular_file\n            ? (uint32_t)(((uint64_t)object->clusters.count * 10U + 99U) / 100U)\n            : 0U;',
    '        object->reserve_clusters = reserve_percent != 0U && object->regular_file\n            ? (uint32_t)(((uint64_t)object->clusters.count * reserve_percent + 99U) / 100U)\n            : 0U;'
)
plan = plan.replace('if (plan->growth && planned->regular_file)',
                    'if (plan->reserve_percent != 0U && planned->regular_file)')
plan_path.write_text(plan, encoding='utf-8')

worker_path = NATIVE / 'exfat_worker.c'
worker = worker_path.read_text(encoding='utf-8')
worker = worker.replace('if (plan->growth && object->regular_file)',
                        'if (plan->reserve_percent != 0U && object->regular_file)')
worker = worker.replace('exfat_build_plan(&source, &catalogue, growth, &plan, error)',
                        'exfat_build_plan(&source, &catalogue, growth ? 10U : 0U, &plan, error)')
worker = worker.replace(
    'static int build_and_commit(const char *device, const char *operation, const char *journal_path,\n                            bool live_updates, char **error) {',
    'static int build_and_commit(const char *device, const char *operation, const char *journal_path,\n                            size_t ram_bytes, size_t batch_clusters,\n                            bool live_updates, char **error) {'
)
anchor = '    if (access(journal_path, F_OK) == 0) { exfat_set_error(error, "an unfinished exFAT journal exists; run Recover first"); return 1; }\n'
fast = anchor + r'''    bool growth = strcmp(operation, "growth-defrag") == 0;
    ExfatRelayoutStats relayout_stats;
    int relayout = exfat_relayout_in_place(device, journal_path,
                                           growth ? 10U : 0U,
                                           ram_bytes, batch_clusters,
                                           live_updates, &relayout_stats, error);
    if (relayout == EXFAT_RELAYOUT_COMPLETED) {
        emit_result(operation, "completed", "");
        return 0;
    }
    if (relayout == EXFAT_RELAYOUT_NOT_NEEDED) {
        emit_result(operation, "not-needed", "");
        return 0;
    }
    if (relayout == EXFAT_RELAYOUT_STOPPED) {
        emit_result(operation, "stopped", "");
        return 130;
    }
    if (relayout == EXFAT_RELAYOUT_FAILED) return 1;
    fprintf(stderr,
            "exFAT terminal-workspace fast path is unavailable for this layout; using the verified shadow-image compatibility path.\n");
'''
worker = worker.replace(anchor, fast, 1)
worker = worker.replace('    bool growth = strcmp(operation, "growth-defrag") == 0;\n    if (exfat_build_plan',
                        '    if (exfat_build_plan', 1)

# Add RAM/batch parsing helpers before the legacy recovery entry point.
parse_helpers = r'''
static size_t parse_ram_bytes(const char *text) {
    if (strcmp(text, "auto") == 0) return ld_default_ram_limit();
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text) return 0U;
    uint64_t multiplier = 1U;
    if (*end == '\0' || strcmp(end, "B") == 0 || strcmp(end, "b") == 0) multiplier = 1U;
    else if (strcasecmp(end, "K") == 0 || strcasecmp(end, "KB") == 0) multiplier = UINT64_C(1024);
    else if (strcasecmp(end, "M") == 0 || strcasecmp(end, "MB") == 0) multiplier = UINT64_C(1024) * 1024U;
    else if (strcasecmp(end, "G") == 0 || strcasecmp(end, "GB") == 0) multiplier = UINT64_C(1024) * 1024U * 1024U;
    else return 0U;
    if ((uint64_t)value > UINT64_MAX / multiplier) return 0U;
    uint64_t bytes = (uint64_t)value * multiplier;
    if (bytes == 0U || bytes > SIZE_MAX) return 0U;
    return (size_t)bytes;
}

static size_t parse_batch_clusters(const char *text) {
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > SIZE_MAX) return 0U;
    return (size_t)value;
}

'''
worker = worker.replace('static int recover_transaction(const char *device, const char *journal_path, char **error) {',
                        parse_helpers + 'static int recover_transaction(const char *device, const char *journal_path,\n                               size_t ram_bytes, size_t batch_clusters,\n                               bool live_updates, char **error) {\n    bool handled = false;\n    int modern = exfat_relayout_recover(device, journal_path, ram_bytes,\n                                        batch_clusters, live_updates,\n                                        &handled, error);\n    if (handled) return modern;')

worker = worker.replace(
    '    const char *confirm = NULL, *journal = NULL; bool write = false, live_updates = false; int growth_percent = 10;\n',
    '    const char *confirm = NULL, *journal = NULL; bool write = false, live_updates = false; int growth_percent = 10;\n    size_t ram_bytes = ld_default_ram_limit();\n    size_t batch_clusters = 0U;\n'
)
worker = worker.replace(
    '        else if ((strcmp(argv[i], "--workers") == 0 || strcmp(argv[i], "--ram-buffer") == 0 || strcmp(argv[i], "--batch-clusters") == 0) && i + 1 < argc) { i++; }',
    '        else if (strcmp(argv[i], "--ram-buffer") == 0 && i + 1 < argc) { ram_bytes = parse_ram_bytes(argv[++i]); if (ram_bytes == 0U) { fprintf(stderr, "%s: invalid RAM buffer size\\n", PROGRAM_NAME); return 2; } }\n        else if (strcmp(argv[i], "--batch-clusters") == 0 && i + 1 < argc) { batch_clusters = parse_batch_clusters(argv[++i]); if (batch_clusters == 0U) { fprintf(stderr, "%s: invalid batch cluster count\\n", PROGRAM_NAME); return 2; } }\n        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) { i++; }'
)
worker = worker.replace('fprintf(stderr, "%s: Growth Defrag requires exactly 10%%%%\\n", PROGRAM_NAME);',
                        'fprintf(stderr, "%s: Growth Defrag requires exactly 10%%\\n", PROGRAM_NAME);')
worker = worker.replace(
    '? recover_transaction(device, journal, &error)\n        : build_and_commit(device, operation, journal, live_updates, &error);',
    '? recover_transaction(device, journal, ram_bytes, batch_clusters, live_updates, &error)\n        : build_and_commit(device, operation, journal, ram_bytes, batch_clusters, live_updates, &error);'
)
worker_path.write_text(worker, encoding='utf-8')

project_path = ROOT / 'cmake/project.cmake'
project = project_path.read_text(encoding='utf-8')
project = project.replace(
    'add_library(linux-defragger-exfat-native STATIC\n    gui/filesystems/exfat/native/exfat_common.c\n    gui/filesystems/exfat/native/exfat_plan.c)',
    'add_library(linux-defragger-exfat-native STATIC\n    gui/filesystems/exfat/native/exfat_common.c\n    gui/filesystems/exfat/native/exfat_plan.c\n    gui/filesystems/exfat/native/exfat_relayout.c)'
)
project_path.write_text(project, encoding='utf-8')

root_cmake = ROOT / 'CMakeLists.txt'
cmake_text = root_cmake.read_text(encoding='utf-8')
if 'cmake/exfat_relayout.cmake' not in cmake_text:
    cmake_text = cmake_text.replace('include("${CMAKE_CURRENT_LIST_DIR}/cmake/fat_relayout.cmake")\n',
                                    'include("${CMAKE_CURRENT_LIST_DIR}/cmake/fat_relayout.cmake")\ninclude("${CMAKE_CURRENT_LIST_DIR}/cmake/exfat_relayout.cmake")\n')
root_cmake.write_text(cmake_text, encoding='utf-8')

(ROOT / 'cmake/exfat_relayout.cmake').write_text(r'''# SPDX-License-Identifier: GPL-3.0-or-later
if(BUILD_TESTING)
    add_test(
        NAME linux-defragger-exfat-relayout
        COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/../tests/test_exfat_relayout_engine.sh"
                "$<TARGET_FILE:linux-defragger-exfat-worker>")
endif()
''', encoding='utf-8')

(ROOT / 'tests/verify_exfat_relayout.py').write_text(r'''#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import annotations
import argparse
import json
import struct
import subprocess
from pathlib import Path

BPS=512
HEAP_OFF=32
CS=512
EOC=0xFFFFFFFF
SERIAL=0x39E0FA71

def coff(cluster: int) -> int:
    return (HEAP_OFF + cluster - 2) * BPS

def u32(data: bytes, off: int) -> int:
    return struct.unpack_from('<I', data, off)[0]

def expected_payload(mult: int, add: int, clusters: int) -> bytes:
    return bytes((i * mult + add) & 255 for i in range(clusters * CS))

def main() -> None:
    ap=argparse.ArgumentParser()
    ap.add_argument('worker', type=Path)
    ap.add_argument('image', type=Path)
    ap.add_argument('mode', choices=('defrag','growth'))
    args=ap.parse_args()
    image=args.image.read_bytes()
    assert u32(image,100)==SERIAL
    assert u32(image,96)==4
    fat=image[24*BPS:32*BPS]
    assert u32(fat,2*4)==EOC
    assert u32(fat,3*4)==EOC
    assert u32(fat,4*4)==5 and u32(fat,5*4)==EOC

    root=image[coff(4):coff(6)]
    assert u32(root,20)==2
    assert u32(root,32+20)==3
    assert u32(root,96+20)==6
    a_first=8
    b_first=12 if args.mode=='growth' else 11
    c_first=17 if args.mode=='growth' else 15
    assert u32(root,192+20)==a_first
    assert u32(root,288+20)==b_first
    assert root[96+1] & 0x02
    assert root[192+1] & 0x02
    assert root[288+1] & 0x02
    sub=image[coff(6):coff(8)]
    assert u32(sub,32+20)==c_first
    assert sub[32+1] & 0x02

    assert image[coff(a_first):coff(a_first+3)] == expected_payload(7,3,3)
    assert image[coff(b_first):coff(b_first+4)] == expected_payload(11,5,4)
    assert image[coff(c_first):coff(c_first+2)] == expected_payload(13,9,2)

    bitmap=image[coff(2):coff(3)]
    def allocated(cluster: int) -> bool:
        bit=cluster-2
        return bool(bitmap[bit>>3] & (1 << (bit & 7)))
    if args.mode=='growth':
        expected=set(range(2,11)) | set(range(12,16)) | set(range(17,19))
        for cluster in range(2,20):
            assert allocated(cluster) == (cluster in expected), cluster
    else:
        for cluster in range(2,17): assert allocated(cluster), cluster
        assert not allocated(17)

    analysed=subprocess.run([str(args.worker),'analyse-json',str(args.image)],
                             check=True,text=True,stdout=subprocess.PIPE).stdout
    payload=json.loads(analysed)
    assert payload['fragmented_files']==0
    assert payload['fragmented_directories']==0
    if args.mode=='growth': assert payload['growth_10_satisfied'] is True
    print(f'verified exFAT {args.mode} canonical layout, metadata and payload integrity')

if __name__=='__main__':
    main()
''', encoding='utf-8')

(ROOT / 'tests/test_exfat_relayout_engine.sh').write_text(r'''#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORKER=${1:?exFAT worker path required}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-exfat-relayout.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail(){ echo "TEST FAILURE: $*" >&2; exit 1; }

python3 "$ROOT/tests/make_exfat_image.py" "$WORK/defrag.img"
"$WORKER" defrag "$WORK/defrag.img" --write --confirm "$WORK/defrag.img" \
    --journal "$WORK/defrag.journal" --ram-buffer 128M --live-updates \
    >"$WORK/defrag.log" 2>&1
python3 "$ROOT/tests/verify_exfat_relayout.py" "$WORKER" "$WORK/defrag.img" defrag
grep -q 'exFAT unified workspace layout:' "$WORK/defrag.log" || fail 'Defrag did not use the unified terminal workspace fast path'
if grep -q 'working image\|shadow-image compatibility' "$WORK/defrag.log"; then
    fail 'Defrag fell back to the old full-filesystem working-image path'
fi
[[ ! -e "$WORK/defrag.journal" ]] || fail 'Defrag left its recovery journal behind'
"$WORKER" defrag "$WORK/defrag.img" --write --confirm "$WORK/defrag.img" \
    --journal "$WORK/defrag-second.journal" >"$WORK/defrag-second.log" 2>&1
grep -q 'Not needed; canonical exFAT layout with 0% post-file reserve policy verified' "$WORK/defrag-second.log" || fail 'Defrag is not idempotent'

python3 "$ROOT/tests/make_exfat_image.py" "$WORK/growth.img"
"$WORKER" growth-defrag "$WORK/growth.img" --write --confirm "$WORK/growth.img" \
    --journal "$WORK/growth.journal" --growth-percent 10 --ram-buffer 128M \
    >"$WORK/growth.log" 2>&1
python3 "$ROOT/tests/verify_exfat_relayout.py" "$WORKER" "$WORK/growth.img" growth
grep -q 'exFAT unified workspace layout:' "$WORK/growth.log" || fail 'Growth Defrag did not use the unified terminal workspace fast path'
if grep -q 'working image\|shadow-image compatibility' "$WORK/growth.log"; then
    fail 'Growth Defrag fell back to the old full-filesystem working-image path'
fi
"$WORKER" growth-defrag "$WORK/growth.img" --write --confirm "$WORK/growth.img" \
    --journal "$WORK/growth-second.journal" --growth-percent 10 \
    >"$WORK/growth-second.log" 2>&1
grep -q 'Not needed; canonical exFAT layout with 10% post-file reserve policy verified' "$WORK/growth-second.log" || fail 'Growth Defrag is not idempotent'

"$WORKER" defrag "$WORK/growth.img" --write --confirm "$WORK/growth.img" \
    --journal "$WORK/growth-to-packed.journal" --ram-buffer 128M \
    >"$WORK/growth-to-packed.log" 2>&1
python3 "$ROOT/tests/verify_exfat_relayout.py" "$WORKER" "$WORK/growth.img" defrag
grep -q 'exFAT unified workspace layout:' "$WORK/growth-to-packed.log" || fail 'Growth-to-packed conversion missed unified workspace path'

printf 'Unified exFAT in-place relayout tests passed.\n'
''', encoding='utf-8')
(ROOT / 'tests/test_exfat_relayout_engine.sh').chmod(0o755)
(ROOT / 'tests/verify_exfat_relayout.py').chmod(0o755)

plugin_path = ROOT / 'gui/filesystems/exfat/plugin.py'
plugin = plugin_path.read_text(encoding='utf-8')
plugin = plugin.replace('raw staging, verification, commit and recovery',
                        'canonical dependency-aware relayout, verification, journalling and recovery')
plugin_path.write_text(plugin, encoding='utf-8')

print('Applied exFAT in-place canonical relayout implementation')
