// SPDX-License-Identifier: GPL-3.0-or-later
#include "exfat_native.h"

#include "ld_io.h"
#include "ld_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define COPY_CHUNK (4U * 1024U * 1024U)
#define EXFAT_LIVE_BATCH 128U

typedef struct {
    uint32_t source;
    uint32_t target;
    uint32_t count;
} ExfatLiveMove;

static void emit_live_moves(const ExfatLiveMove *moves, size_t count,
                            uint32_t cluster_size, uint64_t moved_clusters,
                            uint64_t objects_done, uint64_t objects_total,
                            uint64_t *sequence) {
    if (count == 0U) return;
    printf("@@LIVE_RANGES {\"ranges\":[");
    for (size_t i = 0; i < count; ++i) {
        if (i != 0U) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]",
               (uint64_t)(moves[i].source - 2U) * cluster_size,
               (uint64_t)(moves[i].target - 2U) * cluster_size,
               (uint64_t)moves[i].count * cluster_size);
    }
    printf("],\"moved_total_bytes\":%" PRIu64
           ",\"pass\":1,\"objects_done\":%" PRIu64
           ",\"objects_total\":%" PRIu64
           ",\"sequence\":%" PRIu64 "}\n",
           moved_clusters * (uint64_t)cluster_size,
           objects_done, objects_total, ++(*sequence));
    fflush(stdout);
}

static void queue_live_cluster(bool enabled, ExfatLiveMove *moves, size_t *count,
                               uint32_t source, uint32_t target,
                               uint32_t cluster_size, uint64_t moved_clusters,
                               uint64_t objects_done, uint64_t objects_total,
                               uint64_t *sequence) {
    if (!enabled || source == target) return;
    if (*count != 0U) {
        ExfatLiveMove *last = &moves[*count - 1U];
        if (last->source + last->count == source && last->target + last->count == target) {
            last->count++;
            return;
        }
    }
    moves[(*count)++] = (ExfatLiveMove){source, target, 1U};
    if (*count == EXFAT_LIVE_BATCH) {
        emit_live_moves(moves, *count, cluster_size, moved_clusters,
                        objects_done, objects_total, sequence);
        *count = 0U;
    }
}

static int object_order(const void *left, const void *right) {
    const ExfatObject *const *a = left;
    const ExfatObject *const *b = right;
    int rank_a = (*a)->kind == EXFAT_OBJ_BITMAP ? 0 :
                 (*a)->kind == EXFAT_OBJ_UPCASE ? 1 :
                 (*a)->kind == EXFAT_OBJ_ROOT ? 2 :
                 (*a)->kind == EXFAT_OBJ_DIRECTORY ? 3 : 4;
    int rank_b = (*b)->kind == EXFAT_OBJ_BITMAP ? 0 :
                 (*b)->kind == EXFAT_OBJ_UPCASE ? 1 :
                 (*b)->kind == EXFAT_OBJ_ROOT ? 2 :
                 (*b)->kind == EXFAT_OBJ_DIRECTORY ? 3 : 4;
    if (rank_a != rank_b) return rank_a < rank_b ? -1 : 1;
    return strcasecmp((*a)->path, (*b)->path);
}

static void bitmap_set(uint8_t *bitmap, uint32_t cluster, bool allocated) {
    uint32_t bit = cluster - 2U;
    uint8_t mask = (uint8_t)(1U << (bit & 7U));
    if (allocated) bitmap[bit >> 3U] |= mask;
    else bitmap[bit >> 3U] &= (uint8_t)~mask;
}

int exfat_build_plan(ExfatVolume *volume, ExfatCatalogue *catalogue,
                     bool growth, ExfatPlan *plan, char **error) {
    memset(plan, 0, sizeof(*plan));
    plan->growth = growth;
    plan->bitmap_length = catalogue->bitmap_length;
    plan->expected_bitmap = calloc(plan->bitmap_length, 1U);
    if (plan->expected_bitmap == NULL) {
        exfat_set_error(error, "allocating exFAT canonical bitmap failed");
        return -1;
    }
    ExfatObject **order = ld_xmalloc(catalogue->objects.count * sizeof(*order));
    for (size_t i = 0; i < catalogue->objects.count; ++i) order[i] = &catalogue->objects.items[i];
    qsort(order, catalogue->objects.count, sizeof(*order), object_order);

    uint64_t cursor = 2U;
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        ExfatObject *object = order[i];
        object->target_start = (uint32_t)cursor;
        object->reserve_clusters = growth && object->regular_file
            ? (uint32_t)(((uint64_t)object->clusters.count * 10U + 99U) / 100U)
            : 0U;
        uint64_t end = cursor + object->clusters.count;
        uint64_t after_reserve = end + object->reserve_clusters;
        if (end > (uint64_t)volume->cluster_count + 2ULL ||
            after_reserve > (uint64_t)volume->cluster_count + 2ULL) {
            free(order);
            exfat_plan_free(plan);
            exfat_set_error(error, "canonical exFAT layout does not fit in the cluster heap");
            return -1;
        }
        for (size_t j = 0; j < object->clusters.count; ++j)
            bitmap_set(plan->expected_bitmap, object->target_start + (uint32_t)j, true);
        cursor = after_reserve;
    }
    plan->final_cluster = cursor == 2U ? 1U : (uint32_t)(cursor - 1U);
    free(order);
    return 0;
}

void exfat_plan_free(ExfatPlan *plan) {
    if (plan == NULL) return;
    free(plan->expected_bitmap);
    memset(plan, 0, sizeof(*plan));
}

static int digest_stream(const ExfatVolume *volume, const ExfatObject *object,
                         uint8_t digest[32], char **error) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context);
        exfat_set_error(error, "initialising exFAT SHA-256 failed");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc(volume->cluster_size);
    uint64_t remaining = object->data_length;
    for (size_t i = 0; i < object->clusters.count && remaining != 0; ++i) {
        if (exfat_read_cluster(volume, object->clusters.items[i], buffer, error) != 0) {
            free(buffer); EVP_MD_CTX_free(context); return -1;
        }
        size_t length = remaining < volume->cluster_size ? (size_t)remaining : volume->cluster_size;
        if (EVP_DigestUpdate(context, buffer, length) != 1) {
            free(buffer); EVP_MD_CTX_free(context);
            exfat_set_error(error, "updating exFAT SHA-256 failed"); return -1;
        }
        remaining -= length;
    }
    free(buffer);
    if (remaining != 0) {
        EVP_MD_CTX_free(context); exfat_set_error(error, "short exFAT stream while hashing"); return -1;
    }
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context, digest, &size) != 1 || size != 32U) {
        EVP_MD_CTX_free(context); exfat_set_error(error, "finalising exFAT SHA-256 failed"); return -1;
    }
    EVP_MD_CTX_free(context);
    return 0;
}

static int copy_prefix(int source_fd, int target_fd, uint64_t length, char **error) {
    uint8_t *buffer = ld_xmalloc(COPY_CHUNK);
    uint64_t offset = 0;
    while (offset < length) {
        size_t chunk = (size_t)((length - offset) < COPY_CHUNK ? (length - offset) : COPY_CHUNK);
        ssize_t got = ld_pread_full(source_fd, buffer, chunk, offset);
        if (got < 0 || (size_t)got != chunk) {
            free(buffer); exfat_set_error(error, "short exFAT stage prefix read: %s", strerror(errno)); return -1;
        }
        ssize_t wrote = ld_pwrite_full(target_fd, buffer, chunk, offset);
        if (wrote < 0 || (size_t)wrote != chunk) {
            free(buffer); exfat_set_error(error, "short exFAT stage prefix write: %s", strerror(errno)); return -1;
        }
        offset += chunk;
    }
    free(buffer); return 0;
}

static ExfatObject *object_by_kind(ExfatCatalogue *catalogue, ExfatObjectKind kind) {
    for (size_t i = 0; i < catalogue->objects.count; ++i)
        if (catalogue->objects.items[i].kind == kind) return &catalogue->objects.items[i];
    return NULL;
}

static void patch_entry_set_checksum(uint8_t *payload, size_t offset, uint32_t count) {
    exfat_put_u16(payload, offset + 2U,
                  exfat_entry_checksum(payload + offset, (size_t)count * 32U));
}

static int patch_directory(ExfatCatalogue *catalogue, size_t directory_index,
                           uint8_t *payload, size_t length, char **error) {
    ExfatObject *directory = &catalogue->objects.items[directory_index];
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        ExfatObject *child = &catalogue->objects.items[i];
        if (child->kind == EXFAT_OBJ_BITMAP || child->kind == EXFAT_OBJ_UPCASE ||
            child->kind == EXFAT_OBJ_ROOT || child->parent_index != directory_index) continue;
        if (child->entry_count < 2U || child->entry_offset + (uint64_t)child->entry_count * 32U > length) {
            exfat_set_error(error, "missing exFAT directory reference for %s", child->path); return -1;
        }
        size_t stream = SIZE_MAX;
        for (uint32_t e = 1U; e < child->entry_count; ++e) {
            size_t candidate = (size_t)child->entry_offset + (size_t)e * 32U;
            if (payload[candidate] == 0xc0U) { stream = candidate; break; }
        }
        if (stream == SIZE_MAX) {
            exfat_set_error(error, "stream extension disappeared for %s", child->path); return -1;
        }
        exfat_put_u32(payload, stream + 20U, child->target_start);
        payload[stream + 1U] |= 0x02U;
        patch_entry_set_checksum(payload, (size_t)child->entry_offset, child->entry_count);
    }
    if (directory->kind == EXFAT_OBJ_ROOT) {
        ExfatObject *bitmap = object_by_kind(catalogue, EXFAT_OBJ_BITMAP);
        ExfatObject *upcase = object_by_kind(catalogue, EXFAT_OBJ_UPCASE);
        if (bitmap == NULL || upcase == NULL ||
            bitmap->system_entry_offset + 32U > length || upcase->system_entry_offset + 32U > length) {
            exfat_set_error(error, "exFAT root system entries disappeared"); return -1;
        }
        exfat_put_u32(payload, (size_t)bitmap->system_entry_offset + 20U, bitmap->target_start);
        exfat_put_u32(payload, (size_t)upcase->system_entry_offset + 20U, upcase->target_start);
    }
    return 0;
}

static int read_object_payload(ExfatVolume *source, ExfatObject *object,
                               uint8_t **payload, size_t *allocation_length, char **error) {
    uint64_t allocation = (uint64_t)object->clusters.count * source->cluster_size;
    if (allocation > SIZE_MAX) { exfat_set_error(error, "exFAT object is too large"); return -1; }
    if (exfat_read_stream(source, &object->clusters, allocation, payload, error) != 0) return -1;
    *allocation_length = (size_t)allocation;
    return 0;
}

static int write_target_payload(ExfatVolume *stage, const ExfatObject *object,
                                const uint8_t *payload, size_t length, char **error) {
    size_t allocation = object->clusters.count * (size_t)stage->cluster_size;
    if (length > allocation) { exfat_set_error(error, "exFAT payload exceeds target allocation"); return -1; }
    uint8_t *cluster = calloc(stage->cluster_size, 1U);
    if (cluster == NULL) { exfat_set_error(error, "allocating exFAT cluster buffer failed"); return -1; }
    size_t position = 0;
    for (size_t i = 0; i < object->clusters.count; ++i) {
        memset(cluster, 0, stage->cluster_size);
        size_t remain = length > position ? length - position : 0U;
        size_t chunk = remain < stage->cluster_size ? remain : stage->cluster_size;
        if (chunk != 0) memcpy(cluster, payload + position, chunk);
        if (exfat_write_cluster(stage, object->target_start + (uint32_t)i, cluster, error) != 0) {
            free(cluster); return -1;
        }
        position += chunk;
    }
    free(cluster); return 0;
}

static void fat_put(uint8_t *fat, uint32_t cluster, uint32_t value) {
    exfat_put_u32(fat, (size_t)cluster * 4U, value);
}

static int build_fat(const ExfatVolume *source, const ExfatCatalogue *catalogue,
                     uint8_t **fat_out, char **error) {
    if (source->fat_length > SIZE_MAX) { exfat_set_error(error, "exFAT FAT is too large"); return -1; }
    uint8_t *fat = calloc((size_t)source->fat_length, 1U);
    if (fat == NULL) { exfat_set_error(error, "allocating exFAT FAT failed"); return -1; }
    memcpy(fat, source->fat, source->fat_length < 8U ? (size_t)source->fat_length : 8U);
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        const ExfatObject *object = &catalogue->objects.items[i];
        if (object->kind != EXFAT_OBJ_BITMAP && object->kind != EXFAT_OBJ_UPCASE && object->kind != EXFAT_OBJ_ROOT) continue;
        for (size_t j = 0; j < object->clusters.count; ++j) {
            uint32_t cluster = object->target_start + (uint32_t)j;
            uint32_t value = j + 1U < object->clusters.count ? cluster + 1U : EXFAT_EOC;
            if ((uint64_t)cluster * 4ULL + 4ULL > source->fat_length) {
                free(fat); exfat_set_error(error, "exFAT FAT target is out of bounds"); return -1;
            }
            fat_put(fat, cluster, value);
        }
    }
    *fat_out = fat; return 0;
}

static void fill_boot_checksum_sector(uint8_t *region, uint32_t bps) {
    uint32_t checksum = exfat_boot_checksum(region, bps);
    uint8_t *sector = region + (size_t)11U * bps;
    for (uint32_t offset = 0; offset + 4U <= bps; offset += 4U)
        exfat_put_u32(sector, offset, checksum);
}

static int make_boot(const ExfatVolume *source, const ExfatCatalogue *catalogue,
                     const ExfatPlan *plan, bool dirty, uint8_t **boot_out,
                     size_t *boot_length, char **error) {
    size_t length = (size_t)24U * source->bytes_per_sector;
    uint8_t *boot = ld_xmalloc(length);
    memcpy(boot, source->boot_regions, length);
    const ExfatObject *root = NULL;
    for (size_t i = 0; i < catalogue->objects.count; ++i)
        if (catalogue->objects.items[i].kind == EXFAT_OBJ_ROOT) root = &catalogue->objects.items[i];
    if (root == NULL) { free(boot); exfat_set_error(error, "exFAT root object is missing"); return -1; }
    uint64_t allocated = 0;
    for (size_t i = 0; i < plan->bitmap_length; ++i) allocated += (uint64_t)__builtin_popcount((unsigned int)plan->expected_bitmap[i]);
    uint8_t percent = (uint8_t)((allocated * 100U) / (source->cluster_count == 0 ? 1U : source->cluster_count));
    if (percent > 100U) percent = 100U;
    for (size_t region = 0; region < 2U; ++region) {
        uint8_t *base = boot + region * (size_t)12U * source->bytes_per_sector;
        exfat_put_u32(base, 96U, root->target_start);
        uint16_t flags = source->volume_flags;
        if (dirty) flags |= EXFAT_VOLUME_DIRTY; else flags &= (uint16_t)~EXFAT_VOLUME_DIRTY;
        exfat_put_u16(base, 106U, flags);
        base[112U] = percent;
        fill_boot_checksum_sector(base, source->bytes_per_sector);
    }
    *boot_out = boot; *boot_length = length; return 0;
}

int exfat_build_stage(const char *source_path, const char *stage_path, ExfatVolume *source,
                      ExfatCatalogue *catalogue, const ExfatPlan *plan,
                      bool live_updates, char **error) {
    (void)source_path;
    int fd = open(stage_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { exfat_set_error(error, "cannot create exFAT working image: %s", strerror(errno)); return -1; }
    if (ftruncate(fd, (off_t)source->volume_bytes) != 0) {
        exfat_set_error(error, "cannot size exFAT working image: %s", strerror(errno)); close(fd); return -1;
    }
    if (copy_prefix(source->fd, fd, source->heap_offset, error) != 0) { close(fd); return -1; }
    if (fsync(fd) != 0) { exfat_set_error(error, "cannot sync exFAT stage prefix: %s", strerror(errno)); close(fd); return -1; }
    close(fd);

    ExfatVolume stage;
    if (exfat_open_volume(stage_path, true, true, &stage, error) != 0) return -1;
    int result = -1;
    ExfatLiveMove live_moves[EXFAT_LIVE_BATCH];
    size_t live_count = 0U;
    uint64_t live_sequence = 0U;
    uint64_t moved_clusters = 0U;
    uint64_t objects_done = 0U;
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        ExfatObject *object = &catalogue->objects.items[i];
        if ((object->regular_file || object->kind == EXFAT_OBJ_UPCASE) &&
            digest_stream(source, object, object->source_sha256, error) != 0) goto done;
        if (object->regular_file || object->kind == EXFAT_OBJ_UPCASE) object->have_hash = true;
        if (object->kind == EXFAT_OBJ_BITMAP) {
            if (write_target_payload(&stage, object, plan->expected_bitmap, plan->bitmap_length, error) != 0) goto done;
            for (size_t j = 0; j < object->clusters.count; ++j) {
                uint32_t source_cluster = object->clusters.items[j];
                uint32_t target_cluster = object->target_start + (uint32_t)j;
                if (source_cluster != target_cluster) moved_clusters++;
                queue_live_cluster(live_updates, live_moves, &live_count,
                                   source_cluster, target_cluster, source->cluster_size,
                                   moved_clusters, objects_done + 1U, catalogue->objects.count,
                                   &live_sequence);
            }
            objects_done++;
            if (live_updates && live_count != 0U) {
                emit_live_moves(live_moves, live_count, source->cluster_size, moved_clusters,
                                objects_done, catalogue->objects.count, &live_sequence);
                live_count = 0U;
            }
            continue;
        }
        uint8_t *payload = NULL; size_t allocation = 0;
        if (read_object_payload(source, object, &payload, &allocation, error) != 0) goto done;
        if (object->directory) {
            size_t index = (size_t)(object - catalogue->objects.items);
            if (patch_directory(catalogue, index, payload, allocation, error) != 0) { free(payload); goto done; }
        }
        if (write_target_payload(&stage, object, payload, allocation, error) != 0) { free(payload); goto done; }
        free(payload);
        for (size_t j = 0; j < object->clusters.count; ++j) {
            uint32_t source_cluster = object->clusters.items[j];
            uint32_t target_cluster = object->target_start + (uint32_t)j;
            if (source_cluster != target_cluster) moved_clusters++;
            queue_live_cluster(live_updates, live_moves, &live_count,
                               source_cluster, target_cluster, source->cluster_size,
                               moved_clusters, objects_done + 1U, catalogue->objects.count,
                               &live_sequence);
        }
        objects_done++;
        if (live_updates && live_count != 0U) {
            emit_live_moves(live_moves, live_count, source->cluster_size, moved_clusters,
                            objects_done, catalogue->objects.count, &live_sequence);
            live_count = 0U;
        }
    }
    uint8_t *fat = NULL;
    if (build_fat(source, catalogue, &fat, error) != 0) goto done;
    if (ld_pwrite_full(stage.fd, fat, (size_t)source->fat_length, source->fat_offset) != (ssize_t)source->fat_length) {
        free(fat); exfat_set_error(error, "short exFAT stage FAT write"); goto done;
    }
    free(fat);
    uint8_t *boot = NULL; size_t boot_length = 0;
    if (make_boot(source, catalogue, plan, false, &boot, &boot_length, error) != 0) goto done;
    if (ld_pwrite_full(stage.fd, boot, boot_length, 0) != (ssize_t)boot_length) {
        free(boot); exfat_set_error(error, "short exFAT stage boot write"); goto done;
    }
    free(boot);
    if (fsync(stage.fd) != 0) { exfat_set_error(error, "cannot sync exFAT working image: %s", strerror(errno)); goto done; }
    result = 0;
done:
    exfat_close_volume(&stage);
    return result;
}

static ExfatObject *find_object(ExfatCatalogue *catalogue, const ExfatObject *source) {
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        ExfatObject *candidate = &catalogue->objects.items[i];
        if (candidate->kind == source->kind && strcmp(candidate->path, source->path) == 0) return candidate;
    }
    return NULL;
}

int exfat_verify_stage(const char *path, ExfatCatalogue *source_catalogue,
                       const ExfatPlan *plan, bool allow_dirty, char **error) {
    ExfatVolume volume; ExfatCatalogue current;
    if (exfat_scan(path, allow_dirty, &volume, &current, error) != 0) return -1;
    int result = -1;
    if (current.bitmap_length != plan->bitmap_length ||
        memcmp(current.bitmap, plan->expected_bitmap, plan->bitmap_length) != 0) {
        exfat_set_error(error, "final exFAT allocation bitmap differs from the canonical plan"); goto done;
    }
    for (size_t i = 0; i < source_catalogue->objects.count; ++i) {
        ExfatObject *planned = &source_catalogue->objects.items[i];
        ExfatObject *actual = find_object(&current, planned);
        if (actual == NULL || actual->clusters.count != planned->clusters.count) {
            exfat_set_error(error, "final exFAT layout is missing %s", planned->path); goto done;
        }
        for (size_t j = 0; j < actual->clusters.count; ++j) {
            if (actual->clusters.items[j] != planned->target_start + (uint32_t)j) {
                exfat_set_error(error, "%s is not in its planned contiguous allocation", planned->path); goto done;
            }
        }
        if (planned->have_hash) {
            uint8_t digest[32];
            if (digest_stream(&volume, actual, digest, error) != 0) goto done;
            if (memcmp(digest, planned->source_sha256, 32U) != 0) {
                exfat_set_error(error, "payload verification failed for %s", planned->path); goto done;
            }
        }
        if (plan->growth && planned->regular_file) {
            uint32_t start = planned->target_start + (uint32_t)planned->clusters.count;
            for (uint32_t r = 0; r < planned->reserve_clusters; ++r) {
                if (exfat_allocated(&current, start + r)) {
                    exfat_set_error(error, "growth reserve after %s is allocated", planned->path); goto done;
                }
            }
        }
    }
    result = 0;
done:
    exfat_catalogue_free(&current); exfat_close_volume(&volume); return result;
}

int exfat_set_stage_boot_dirty(const char *stage_path, bool dirty, uint8_t **boot,
                               size_t *length, char **error) {
    ExfatVolume volume; ExfatCatalogue catalogue;
    if (exfat_scan(stage_path, true, &volume, &catalogue, error) != 0) return -1;
    ExfatPlan plan = {0};
    plan.expected_bitmap = catalogue.bitmap;
    plan.bitmap_length = catalogue.bitmap_length;
    int result = make_boot(&volume, &catalogue, &plan, dirty, boot, length, error);
    plan.expected_bitmap = NULL;
    exfat_catalogue_free(&catalogue); exfat_close_volume(&volume); return result;
}
