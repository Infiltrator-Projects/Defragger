// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_RUNTIME_H
#define LD_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ld_runtime_set_program_name(const char *name);

_Noreturn void ld_die_errno(const char *what);
_Noreturn void ld_die(const char *message);
void ld_warn_errno(const char *what);

void *ld_xmalloc(size_t size);
void *ld_xcalloc(size_t count, size_t size);
void *ld_xrealloc(void *pointer, size_t size);
char *ld_xstrdup(const char *text);
char *ld_xstrndup(const char *text, size_t length);

bool ld_u64_add(uint64_t left, uint64_t right, uint64_t *result);

static inline uint16_t ld_read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ld_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}


static inline void ld_write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & UINT16_C(0x00ff));
    p[1] = (uint8_t)((value >> 8) & UINT16_C(0x00ff));
}

static inline void ld_write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & UINT32_C(0x000000ff));
    p[1] = (uint8_t)((value >> 8) & UINT32_C(0x000000ff));
    p[2] = (uint8_t)((value >> 16) & UINT32_C(0x000000ff));
    p[3] = (uint8_t)((value >> 24) & UINT32_C(0x000000ff));
}


#endif
