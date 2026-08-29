// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * RAM-bounded, extent-coalesced FAT cluster relocation I/O.
 */

#include "fat_io.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ld_io.h"
#include "ld_runtime.h"
#include "infiltratr/arithmetic.h"

typedef struct {
    uint64_t disk_offset;
    size_t buffer_offset;
    size_t length;
} IoExtent;

typedef struct {
    IoExtent *items;
    size_t length;
    size_t capacity;
} IoExtentList;

static void extent_list_push(
    IoExtentList *list,
    IoExtent extent
) {
    if (list->length == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&list->items, &list->capacity,
                                  sizeof(*list->items), list->length + 1U, 16U))
        ld_die("cannot grow FAT I/O extent list");
    list->items[list->length++] = extent;
}

static void extent_list_free(IoExtentList *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int compare_extent_disk_offset(
    const void *left,
    const void *right
) {
    const IoExtent *left_extent = left;
    const IoExtent *right_extent = right;
    if (left_extent->disk_offset < right_extent->disk_offset) {
        return -1;
    }
    if (left_extent->disk_offset > right_extent->disk_offset) {
        return 1;
    }
    return 0;
}

static IoExtentList build_cluster_extents(
    const Fat32 *fs,
    const uint32_t *clusters,
    size_t count
) {
    IoExtentList list = {0};
    if (count == 0) {
        return list;
    }
    size_t first = 0;
    for (size_t index = 1; index <= count; index++) {
        bool end =
            index == count
            || clusters[index] != clusters[index - 1] + 1;
        if (!end) {
            continue;
        }
        size_t cluster_count = index - first;
        if (cluster_count > SIZE_MAX / (size_t)fs->cluster_size) {
            extent_list_free(&list);
            ld_die("I/O extent is too large for this build");
        }
        extent_list_push(
            &list,
            (IoExtent){
                .disk_offset = cluster_offset(
                    fs,
                    clusters[first]
                ),
                .buffer_offset =
                    first * (size_t)fs->cluster_size,
                .length =
                    cluster_count * (size_t)fs->cluster_size,
            }
        );
        first = index;
    }
    return list;
}

typedef struct {
    int file_descriptor;
    uint8_t *buffer;
    const IoExtent *extents;
    size_t extent_count;
    size_t next_extent;
    pthread_mutex_t lock;
    int error_number;
} ReadQueue;

static void *read_extent_worker(void *argument) {
    ReadQueue *queue = argument;
    for (;;) {
        pthread_mutex_lock(&queue->lock);
        size_t index = queue->next_extent++;
        pthread_mutex_unlock(&queue->lock);
        if (index >= queue->extent_count) {
            break;
        }
        const IoExtent *extent = &queue->extents[index];
        if (ld_pread_full(
                queue->file_descriptor,
                queue->buffer + extent->buffer_offset,
                extent->length,
                extent->disk_offset
            ) != (ssize_t)extent->length) {
            int saved = errno == 0 ? EIO : errno;
            pthread_mutex_lock(&queue->lock);
            if (queue->error_number == 0) {
                queue->error_number = saved;
            }
            pthread_mutex_unlock(&queue->lock);
            break;
        }
    }
    return NULL;
}

static void read_extents(
    Fat32 *fs,
    uint8_t *buffer,
    IoExtentList *extents,
    FatIoConfig *config
) {
    if (extents->length == 0) {
        return;
    }
    qsort(
        extents->items,
        extents->length,
        sizeof(extents->items[0]),
        compare_extent_disk_offset
    );
    size_t workers = config->workers;
    if (workers > extents->length) {
        workers = extents->length;
    }
    if (workers < 1) {
        workers = 1;
    }

    ReadQueue queue = {
        .file_descriptor = fs->dev.fd,
        .buffer = buffer,
        .extents = extents->items,
        .extent_count = extents->length,
    };
    if (pthread_mutex_init(&queue.lock, NULL) != 0) {
        ld_die("cannot initialise I/O worker lock");
    }

    if (workers == 1) {
        (void)read_extent_worker(&queue);
    } else {
        pthread_t *threads =
            ld_xmalloc(workers * sizeof(*threads));
        size_t started = 0;
        for (; started < workers; started++) {
            int result = pthread_create(
                &threads[started],
                NULL,
                read_extent_worker,
                &queue
            );
            if (result != 0) {
                queue.error_number = result;
                break;
            }
        }
        for (size_t index = 0; index < started; index++) {
            (void)pthread_join(threads[index], NULL);
        }
        free(threads);
    }
    pthread_mutex_destroy(&queue.lock);
    if (queue.error_number != 0) {
        errno = queue.error_number;
        ld_die_errno("read source extent");
    }
    for (size_t index = 0; index < extents->length; index++) {
        config->bytes_read += extents->items[index].length;
    }
    config->read_extents += extents->length;
}

static void write_extents(
    Fat32 *fs,
    const uint8_t *buffer,
    IoExtentList *extents,
    FatIoConfig *config
) {
    if (extents->length == 0) {
        return;
    }
    qsort(
        extents->items,
        extents->length,
        sizeof(extents->items[0]),
        compare_extent_disk_offset
    );
    for (size_t index = 0; index < extents->length; index++) {
        const IoExtent *extent = &extents->items[index];
        if (ld_pwrite_full(
                fs->dev.fd,
                buffer + extent->buffer_offset,
                extent->length,
                extent->disk_offset
            ) != (ssize_t)extent->length) {
            ld_die_errno("write destination extent");
        }
        config->bytes_written += extent->length;
    }
    config->write_extents += extents->length;
}

void fat_io_copy_clusters(
    Fat32 *fs,
    const uint32_t *sources,
    const uint32_t *destinations,
    size_t count,
    FatIoConfig *config
) {
    if (count == 0) {
        return;
    }
    size_t cluster_size = (size_t)fs->cluster_size;
    size_t clusters_per_chunk =
        config->ram_limit / cluster_size;
    if (clusters_per_chunk == 0) {
        clusters_per_chunk = 1;
    }
    if (clusters_per_chunk > count) {
        clusters_per_chunk = count;
    }
    if (clusters_per_chunk > SIZE_MAX / cluster_size) {
        ld_die("RAM buffer size overflow");
    }
    size_t allocation = clusters_per_chunk * cluster_size;
    void *raw = NULL;
    int result = posix_memalign(&raw, 4096, allocation);
    if (result != 0) {
        errno = result;
        ld_die_errno("allocate aligned RAM buffer");
    }
    uint8_t *buffer = raw;

    for (size_t base = 0; base < count; base += clusters_per_chunk) {
        size_t chunk = count - base;
        if (chunk > clusters_per_chunk) {
            chunk = clusters_per_chunk;
        }
        IoExtentList source_extents = build_cluster_extents(
            fs,
            sources + base,
            chunk
        );
        IoExtentList destination_extents = build_cluster_extents(
            fs,
            destinations + base,
            chunk
        );
        read_extents(
            fs,
            buffer,
            &source_extents,
            config
        );
        write_extents(
            fs,
            buffer,
            &destination_extents,
            config
        );
        extent_list_free(&source_extents);
        extent_list_free(&destination_extents);
    }
    free(buffer);
}
