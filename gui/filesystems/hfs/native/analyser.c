// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux Defragger - classic HFS read-only analyser
 * Author: Shannon Smith
 *
 * The bundled libhfs code opens the partition or image as a raw medium.  This
 * helper never asks the Linux HFS driver to mount the filesystem and never
 * writes to the medium.  Its only purpose is to walk catalog records and fork
 * extent chains so the standard HFS plugin can report fragmentation exactly.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libhfs.h"
#include "file.h"
#include "volume.h"
#include "btree.h"

#define MAX_EXTENTS 512U
#define MAX_PATH_LEN 2048U

typedef struct {
    unsigned int start;
    unsigned int count;
} scan_extent;

typedef struct {
    char path[MAX_PATH_LEN];
    int fork;
    scan_extent extents[MAX_EXTENTS];
    unsigned int extent_count;
    unsigned int fragments;
    unsigned int total_blocks;
} fork_info;

typedef struct {
    fork_info *items;
    size_t count;
    size_t capacity;
    unsigned long files;
    unsigned long directories;
} fork_list;

static int list_append(fork_list *list, const fork_info *info)
{
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2U : 64U;
        fork_info *items = (fork_info *)realloc(list->items, next * sizeof(*items));
        if (!items)
            return -1;
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = *info;
    return 0;
}

static int collect_fork(hfsfile *file, int fork, fork_info *info)
{
    ExtDataRec current;
    ExtDataRec *inline_extents;
    unsigned long *logical_length;
    unsigned long *physical_length;
    unsigned int logical_block = 0U;
    unsigned int total_blocks;
    unsigned int i;
    node n;

    if (hfs_setfork(file, fork) == -1)
        return -1;
    f_getptrs(file, &inline_extents, &logical_length, &physical_length);
    (void)logical_length;
    total_blocks = (unsigned int)(*physical_length / file->vol->mdb.drAlBlkSiz);
    info->fork = fork;
    info->extent_count = 0U;
    info->fragments = 0U;
    info->total_blocks = total_blocks;
    memcpy(&current, inline_extents, sizeof(current));

    while (logical_block < total_blocks) {
        for (i = 0U; i < 3U && logical_block < total_blocks; ++i) {
            unsigned int count = current[i].xdrNumABlks;
            unsigned int start = current[i].xdrStABN;
            scan_extent *previous;
            if (!count || info->extent_count >= MAX_EXTENTS)
                return -1;
            info->extents[info->extent_count].start = start;
            info->extents[info->extent_count].count = count;
            if (info->extent_count == 0U) {
                info->fragments = 1U;
            } else {
                previous = &info->extents[info->extent_count - 1U];
                if (previous->start + previous->count != start)
                    ++info->fragments;
            }
            ++info->extent_count;
            logical_block += count;
        }
        if (logical_block < total_blocks) {
            memset(&n, 0, sizeof(n));
            if (v_extsearch(file, logical_block, &current, &n) <= 0)
                return -1;
        }
    }
    return 0;
}

static int scan_directory(hfsvol *volume, const char *path, fork_list *list)
{
    hfsdir *directory;
    hfsdirent entry;
    char child[MAX_PATH_LEN];

    directory = hfs_opendir(volume, path);
    if (!directory)
        return -1;
    if (!strcmp(path, ":"))
        list->directories = 1UL;

    while (hfs_readdir(directory, &entry) == 0) {
        hfsfile *file;
        fork_info info;
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, ".."))
            continue;
        if (!strcmp(path, ":")) {
            if (snprintf(child, sizeof(child), ":%s", entry.name) >= (int)sizeof(child))
                continue;
        } else if (snprintf(child, sizeof(child), "%s:%s", path, entry.name) >= (int)sizeof(child)) {
            continue;
        }

        if (entry.flags & HFS_ISDIR) {
            ++list->directories;
            if (scan_directory(volume, child, list) == -1) {
                hfs_closedir(directory);
                return -1;
            }
            continue;
        }

        ++list->files;
        file = hfs_open(volume, child);
        if (!file) {
            hfs_closedir(directory);
            return -1;
        }

        memset(&info, 0, sizeof(info));
        strncpy(info.path, child, sizeof(info.path) - 1U);
        if (collect_fork(file, 0, &info) == 0 && info.total_blocks &&
            list_append(list, &info) == -1) {
            hfs_close(file);
            hfs_closedir(directory);
            return -1;
        }

        memset(&info, 0, sizeof(info));
        strncpy(info.path, child, sizeof(info.path) - 1U);
        if (collect_fork(file, 1, &info) == 0 && info.total_blocks &&
            list_append(list, &info) == -1) {
            hfs_close(file);
            hfs_closedir(directory);
            return -1;
        }
        if (hfs_close(file) == -1) {
            hfs_closedir(directory);
            return -1;
        }
    }
    hfs_closedir(directory);
    return 0;
}

static int scan_json(const char *device)
{
    hfsvol *volume;
    fork_list list;
    size_t i;
    size_t j;
    unsigned long fragmented_files = 0UL;
    int first = 1;

    memset(&list, 0, sizeof(list));
    volume = hfs_mount(device, 0, HFS_MODE_RDONLY | HFS_OPT_NOCACHE);
    if (!volume) {
        fprintf(stderr, "hfs-analyser: %s\n", hfs_error ? hfs_error : "open failed");
        return -1;
    }
    if (scan_directory(volume, ":", &list) == -1) {
        fprintf(stderr, "hfs-analyser: scan: %s\n", hfs_error ? hfs_error : "failed");
        hfs_umount(volume);
        free(list.items);
        return -1;
    }

    for (i = 0U; i < list.count; ++i) {
        if (list.items[i].fragments <= 1U)
            continue;
        for (j = 0U; j < i; ++j) {
            if (!strcmp(list.items[j].path, list.items[i].path) &&
                list.items[j].fragments > 1U)
                break;
        }
        if (j == i)
            ++fragmented_files;
    }

    printf("{\"files\":%lu,\"directories\":%lu,\"fragmented_files\":%lu,"
           "\"fragmented_directories\":0,\"fragmented_extents\":[",
           list.files, list.directories, fragmented_files);
    for (i = 0U; i < list.count; ++i) {
        if (list.items[i].fragments <= 1U)
            continue;
        for (j = 0U; j < list.items[i].extent_count; ++j) {
            if (!first)
                putchar(',');
            first = 0;
            printf("[%u,%u]", list.items[i].extents[j].start,
                   list.items[i].extents[j].count);
        }
    }
    printf("]}\n");

    hfs_umount(volume);
    free(list.items);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "scan-json")) {
        fprintf(stderr, "usage: hfs-analyser scan-json DEVICE\n");
        return 2;
    }
    return scan_json(argv[2]) == 0 ? 0 : 1;
}
