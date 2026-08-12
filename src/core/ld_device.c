// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_device.h"
#include "ld_runtime.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

enum { LD_MAX_RELATED_DEVICES = 1024 };

typedef struct {
    dev_t value;
    bool exact_mapping;
    bool relations_scanned;
    bool children_scanned;
} LdRelatedDevice;

static bool ld_add_related_device(LdRelatedDevice *devices, size_t *count,
                                  dev_t value, bool exact_mapping) {
    for (size_t index = 0; index < *count; ++index) {
        if (devices[index].value != value) continue;
        if (exact_mapping) devices[index].exact_mapping = true;
        return true;
    }
    if (*count >= LD_MAX_RELATED_DEVICES) return false;
    devices[*count] = (LdRelatedDevice){
        .value = value,
        .exact_mapping = exact_mapping,
        .relations_scanned = false,
        .children_scanned = false,
    };
    (*count)++;
    return true;
}

static bool ld_read_sysfs_device(const char *path, dev_t *result) {
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    unsigned found_major = 0;
    unsigned found_minor = 0;
    bool valid = fscanf(file, "%u:%u", &found_major, &found_minor) == 2;
    fclose(file);
    if (valid) *result = makedev(found_major, found_minor);
    return valid;
}

static bool ld_resolve_sysfs_device(dev_t device, char *path, size_t size) {
    char link_path[PATH_MAX];
    int length = snprintf(link_path, sizeof(link_path), "/sys/dev/block/%u:%u",
                          major(device), minor(device));
    if (length < 0 || (size_t)length >= sizeof(link_path)) return false;
    return realpath(link_path, path) != NULL && strlen(path) < size;
}

static bool ld_collect_sysfs_directory(const char *path,
                                       LdRelatedDevice *devices,
                                       size_t *count, bool exact_mapping) {
    DIR *directory = opendir(path);
    if (directory == NULL) return true;
    struct dirent *entry = NULL;
    bool complete = true;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char device_path[PATH_MAX];
        int length = snprintf(device_path, sizeof(device_path), "%s/%s/dev",
                              path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(device_path)) {
            complete = false;
            break;
        }
        dev_t related = 0;
        if (ld_read_sysfs_device(device_path, &related) &&
            !ld_add_related_device(devices, count, related, exact_mapping)) {
            complete = false;
            break;
        }
    }
    closedir(directory);
    return complete;
}

static bool ld_add_parent_device(const char *sysfs_path,
                                 LdRelatedDevice *devices, size_t *count) {
    char parent_path[PATH_MAX];
    int length = snprintf(parent_path, sizeof(parent_path), "%s", sysfs_path);
    if (length < 0 || (size_t)length >= sizeof(parent_path)) return false;
    char *separator = strrchr(parent_path, '/');
    if (separator == NULL) return true;
    *separator = '\0';
    size_t used = strlen(parent_path);
    if (used + sizeof("/dev") > sizeof(parent_path)) return false;
    memcpy(parent_path + used, "/dev", sizeof("/dev"));
    dev_t parent = 0;
    if (ld_read_sysfs_device(parent_path, &parent) &&
        !ld_add_related_device(devices, count, parent, false)) return false;
    return true;
}

static bool ld_collect_related_devices(dev_t target,
                                       LdRelatedDevice *devices,
                                       size_t *count) {
    /*
     * exact_mapping=true means this node itself maps the selected storage
     * range.  Child partitions of such a whole-device mapping overlap it and
     * must be checked.  A parent device that merely contains the selected
     * partition is exact_mapping=false: the parent itself overlaps the target,
     * but its sibling partitions do not.
     */
    if (!ld_add_related_device(devices, count, target, true)) return false;

    for (size_t index = 0; index < *count; ++index) {
        char sysfs_path[PATH_MAX];
        if (!ld_resolve_sysfs_device(devices[index].value,
                                     sysfs_path, sizeof(sysfs_path))) continue;

        if (!devices[index].relations_scanned) {
            devices[index].relations_scanned = true;
            if (!ld_add_parent_device(sysfs_path, devices, count)) return false;

            char relation_path[PATH_MAX];
            for (size_t relation = 0; relation < 2; ++relation) {
                const char *name = relation == 0 ? "holders" : "slaves";
                int length = snprintf(relation_path, sizeof(relation_path), "%s/%s",
                                      sysfs_path, name);
                if (length < 0 || (size_t)length >= sizeof(relation_path)) return false;
                if (!ld_collect_sysfs_directory(relation_path, devices, count, true))
                    return false;
            }
        }

        if (devices[index].exact_mapping && !devices[index].children_scanned) {
            devices[index].children_scanned = true;
            if (!ld_collect_sysfs_directory(sysfs_path, devices, count, true))
                return false;
        }
    }
    return true;
}

bool ld_device_number_is_mounted(dev_t device_number) {
    LdRelatedDevice related[LD_MAX_RELATED_DEVICES];
    size_t related_count = 0;
    if (!ld_collect_related_devices(device_number, related, &related_count))
        ld_die("block-device topology is too large to validate safely");

    FILE *file = fopen("/proc/self/mountinfo", "r");
    if (file == NULL) ld_die_errno("open /proc/self/mountinfo");
    char *line = NULL;
    size_t capacity = 0;
    bool mounted = false;
    while (getline(&line, &capacity, file) >= 0) {
        unsigned found_major = 0;
        unsigned found_minor = 0;
        char *cursor = line;
        int field = 0;
        while (*cursor != '\0') {
            while (*cursor == ' ') cursor++;
            if (*cursor == '\0') break;
            field++;
            char *end = strchr(cursor, ' ');
            if (field == 3 && sscanf(cursor, "%u:%u", &found_major, &found_minor) == 2) {
                dev_t found = makedev(found_major, found_minor);
                for (size_t index = 0; index < related_count; ++index) {
                    if (related[index].value == found) {
                        mounted = true;
                        break;
                    }
                }
                if (mounted) break;
            }
            if (end == NULL) break;
            cursor = end + 1;
        }
        if (mounted) break;
    }
    free(line);
    fclose(file);
    return mounted;
}

bool ld_path_is_mounted(const char *path) {
    struct stat status;
    if (stat(path, &status) != 0) ld_die_errno("stat device");
    return S_ISBLK(status.st_mode) && ld_device_number_is_mounted(status.st_rdev);
}

LdDevice ld_device_open(const char *path, bool writable) {
    struct stat status;
    if (stat(path, &status) != 0) ld_die_errno("stat device");
    bool block = S_ISBLK(status.st_mode);
    if (!block && !S_ISREG(status.st_mode))
        ld_die("target must be a block device or a regular filesystem image");
    if (block && writable && ld_device_number_is_mounted(status.st_rdev))
        ld_die("refusing to open a mounted block device for writing; unmount it first");

    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (block && writable) flags |= O_EXCL;
    int fd = open(path, flags);
    if (fd < 0) ld_die_errno("open target");

    uint64_t size = 0;
    if (block) {
        if (ioctl(fd, BLKGETSIZE64, &size) != 0) ld_die_errno("BLKGETSIZE64");
    } else {
        size = (uint64_t)status.st_size;
    }

    LdDevice device = {
        .fd = fd,
        .path = ld_xstrdup(path),
        .writable = writable,
        .is_block = block,
        .size_bytes = size,
        .device_number = block ? status.st_rdev : 0,
    };
    return device;
}

void ld_device_close(LdDevice *device) {
    if (device == NULL) return;
    if (device->fd >= 0 && close(device->fd) != 0) ld_warn_errno("close target");
    free(device->path);
    memset(device, 0, sizeof(*device));
    device->fd = -1;
}

bool ld_device_is_rotational(const LdDevice *device) {
    if (!device->is_block) return false;
    char path[128];
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/queue/rotational",
             major(device->device_number), minor(device->device_number));
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    int value = 0;
    bool result = fscanf(file, "%d", &value) == 1 && value != 0;
    fclose(file);
    return result;
}

bool ld_device_is_serial_flash(const LdDevice *device) {
    const char *base = strrchr(device->path, '/');
    base = base == NULL ? device->path : base + 1;
    return strncmp(base, "mmcblk", 6) == 0;
}
