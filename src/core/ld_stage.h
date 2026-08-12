// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_STAGE_H
#define LD_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LD_STAGE_NONE = 0,
    LD_STAGE_RAW_RANGE,
    LD_STAGE_MEMORY,
} LdStageKind;

typedef struct {
    LdStageKind kind;
    int raw_fd;
    uint64_t base_offset;
    uint64_t size_bytes;
    unsigned char *memory;
} LdStageStore;

typedef struct {
    uint64_t total_bytes;
    uint64_t available_bytes;
    uint64_t safe_budget_bytes;
} LdMemoryInfo;

bool ld_memory_info(LdMemoryInfo *info);
void ld_stage_init_raw(LdStageStore *store, int raw_fd,
                       uint64_t base_offset, uint64_t size_bytes);
bool ld_stage_init_memory(LdStageStore *store, uint64_t size_bytes,
                          LdMemoryInfo *memory_info);
void ld_stage_read(const LdStageStore *store, void *buffer,
                   size_t length, uint64_t offset);
void ld_stage_write(LdStageStore *store, const void *buffer,
                    size_t length, uint64_t offset);
void ld_stage_sync(LdStageStore *store);
void ld_stage_destroy(LdStageStore *store);
const char *ld_stage_kind_name(const LdStageStore *store);
bool ld_stage_is_persistent(const LdStageStore *store);

#endif
