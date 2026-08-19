// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_io.h"
#include "ld_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

ssize_t ld_pread_full(int fd, void *buffer, size_t length, uint64_t offset) {
    uint8_t *cursor = buffer;
    size_t completed = 0;
    while (completed < length) {
        ssize_t count = pread(fd, cursor + completed, length - completed,
                              (off_t)(offset + completed));
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) break;
        completed += (size_t)count;
    }
    return (ssize_t)completed;
}

ssize_t ld_pwrite_full(int fd, const void *buffer, size_t length, uint64_t offset) {
    const uint8_t *cursor = buffer;
    size_t completed = 0;
    while (completed < length) {
        ssize_t count = pwrite(fd, cursor + completed, length - completed,
                               (off_t)(offset + completed));
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            return -1;
        }
        completed += (size_t)count;
    }
    return (ssize_t)completed;
}

void ld_pread_exact(int fd, void *buffer, size_t length, uint64_t offset, const char *what) {
    ssize_t count = ld_pread_full(fd, buffer, length, offset);
    if (count < 0) ld_die_errno(what);
    if ((size_t)count != length) {
        errno = EIO;
        ld_die_errno(what);
    }
}

void ld_pwrite_exact(int fd, const void *buffer, size_t length, uint64_t offset, const char *what) {
    ssize_t count = ld_pwrite_full(fd, buffer, length, offset);
    if (count < 0 || (size_t)count != length) {
        if (count >= 0) errno = EIO;
        ld_die_errno(what);
    }
}

uint64_t ld_available_memory_bytes(void) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file != NULL) {
        char key[64];
        unsigned long long value = 0;
        char unit[16];
        while (fscanf(file, "%63s %llu %15s", key, &value, unit) == 3) {
            if (strcmp(key, "MemAvailable:") == 0) {
                fclose(file);
                return (uint64_t)value * UINT64_C(1024);
            }
        }
        fclose(file);
    }
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0)
        return (uint64_t)(unsigned long)pages * (uint64_t)(unsigned long)page_size;
    return UINT64_C(512) * 1024 * 1024;
}

size_t ld_default_ram_limit(void) {
    uint64_t available = ld_available_memory_bytes();
    const uint64_t minimum = UINT64_C(64) * 1024 * 1024;
    const uint64_t reserve = UINT64_C(8) * 1024 * 1024 * 1024;
    const uint64_t maximum = UINT64_C(16) * 1024 * 1024 * 1024;
    uint64_t selected = available > reserve + minimum ? available - reserve : available / 4;
    if (selected < minimum) selected = minimum;
    if (selected > maximum) selected = maximum;
    if (selected > SIZE_MAX) selected = SIZE_MAX;
    return (size_t)selected;
}

size_t ld_online_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (size_t)count : 1;
}

size_t ld_default_worker_count(bool rotational, bool serial_flash) {
    size_t cpus = ld_online_cpu_count();
    if (rotational) return 1;
    if (serial_flash) return cpus > 1 ? 2 : 1;
    if (cpus > 8) cpus = 8;
    return cpus == 0 ? 1 : cpus;
}
