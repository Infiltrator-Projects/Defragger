// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_stage.h"

#include "ld_io.h"
#include "ld_runtime.h"

#include "infiltratr/core.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define LD_MEMORY_RESERVE_BYTES (UINT64_C(2) * 1024U * 1024U * 1024U)
#define LD_MEMORY_TOTAL_PERCENT 70U

static bool stage_range_valid(const LdStageStore *store, uint64_t offset, size_t length) {
    uint64_t end = 0;
    return ld_u64_add(offset, (uint64_t)length, &end) && end <= store->size_bytes;
}

bool ld_memory_info(LdMemoryInfo *info) {
    if (info == NULL) return false;
    memset(info, 0, sizeof(*info));
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) return false;
    char key[64];
    uint64_t value_kb = 0;
    char unit[16];
    while (fscanf(file, "%63s %" SCNu64 " %15s", key, &value_kb, unit) == 3) {
        uint64_t bytes = infiltratr_u64_multiply_saturating(value_kb, UINT64_C(1024));
        if (strcmp(key, "MemTotal:") == 0) info->total_bytes = bytes;
        else if (strcmp(key, "MemAvailable:") == 0) info->available_bytes = bytes;
    }
    fclose(file);
    if (info->total_bytes == 0 || info->available_bytes == 0) return false;

    uint64_t after_reserve = info->available_bytes > LD_MEMORY_RESERVE_BYTES
                                 ? info->available_bytes - LD_MEMORY_RESERVE_BYTES
                                 : info->available_bytes / 2U;
    uint64_t total_limit = (info->total_bytes / 100U) * LD_MEMORY_TOTAL_PERCENT;
    info->safe_budget_bytes = after_reserve < total_limit ? after_reserve : total_limit;
    return true;
}

void ld_stage_init_raw(LdStageStore *store, int raw_fd,
                       uint64_t base_offset, uint64_t size_bytes) {
    if (store == NULL || raw_fd < 0) ld_die("invalid raw staging store");
    memset(store, 0, sizeof(*store));
    store->kind = LD_STAGE_RAW_RANGE;
    store->raw_fd = raw_fd;
    store->base_offset = base_offset;
    store->size_bytes = size_bytes;
}

bool ld_stage_init_memory(LdStageStore *store, uint64_t size_bytes,
                          LdMemoryInfo *memory_info) {
    if (store == NULL || size_bytes == 0 || size_bytes > (uint64_t)SIZE_MAX) return false;
    LdMemoryInfo local_info;
    if (!ld_memory_info(&local_info)) return false;
    if (memory_info != NULL) *memory_info = local_info;
    if (size_bytes > local_info.safe_budget_bytes) return false;

    void *mapping = mmap(NULL, (size_t)size_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
#ifdef MADV_SEQUENTIAL
    (void)madvise(mapping, (size_t)size_bytes, MADV_SEQUENTIAL);
#endif
#ifdef MADV_DONTDUMP
    (void)madvise(mapping, (size_t)size_bytes, MADV_DONTDUMP);
#endif
    memset(store, 0, sizeof(*store));
    store->kind = LD_STAGE_MEMORY;
    store->raw_fd = -1;
    store->size_bytes = size_bytes;
    store->memory = mapping;
    return true;
}

void ld_stage_read(const LdStageStore *store, void *buffer,
                   size_t length, uint64_t offset) {
    if (store == NULL || buffer == NULL || !stage_range_valid(store, offset, length))
        ld_die("staging read is outside the configured store");
    if (store->kind == LD_STAGE_MEMORY) {
        memcpy(buffer, store->memory + (size_t)offset, length);
        return;
    }
    if (store->kind == LD_STAGE_RAW_RANGE) {
        uint64_t physical = 0;
        if (!ld_u64_add(store->base_offset, offset, &physical))
            ld_die("staging read offset overflow");
        ld_pread_exact(store->raw_fd, buffer, length, physical, "reading raw staging data");
        return;
    }
    ld_die("staging store is not initialized");
}

void ld_stage_write(LdStageStore *store, const void *buffer,
                    size_t length, uint64_t offset) {
    if (store == NULL || buffer == NULL || !stage_range_valid(store, offset, length))
        ld_die("staging write is outside the configured store");
    if (store->kind == LD_STAGE_MEMORY) {
        memcpy(store->memory + (size_t)offset, buffer, length);
        return;
    }
    if (store->kind == LD_STAGE_RAW_RANGE) {
        uint64_t physical = 0;
        if (!ld_u64_add(store->base_offset, offset, &physical))
            ld_die("staging write offset overflow");
        ld_pwrite_exact(store->raw_fd, buffer, length, physical, "writing raw staging data");
        return;
    }
    ld_die("staging store is not initialized");
}

void ld_stage_sync(LdStageStore *store) {
    if (store == NULL) return;
    if (store->kind == LD_STAGE_RAW_RANGE && fsync(store->raw_fd) != 0)
        ld_die_errno("syncing raw staging area");
}

void ld_stage_destroy(LdStageStore *store) {
    if (store == NULL) return;
    if (store->kind == LD_STAGE_MEMORY && store->memory != NULL) {
#ifdef MADV_DONTNEED
        (void)madvise(store->memory, (size_t)store->size_bytes, MADV_DONTNEED);
#endif
        if (munmap(store->memory, (size_t)store->size_bytes) != 0)
            ld_warn_errno("unmapping staging memory");
    }
    memset(store, 0, sizeof(*store));
    store->raw_fd = -1;
}

const char *ld_stage_kind_name(const LdStageStore *store) {
    if (store == NULL) return "uninitialized staging";
    switch (store->kind) {
        case LD_STAGE_RAW_RANGE: return "persistent raw partition staging";
        case LD_STAGE_MEMORY: return "anonymous RAM/swap staging";
        default: return "uninitialized staging";
    }
}

bool ld_stage_is_persistent(const LdStageStore *store) {
    return store != NULL && store->kind == LD_STAGE_RAW_RANGE;
}
