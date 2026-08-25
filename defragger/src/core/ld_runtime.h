// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_RUNTIME_H
#define LD_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "infiltratr/endian.h"

void ld_runtime_set_program_name(const char *name);

_Noreturn void ld_die_errno(const char *what);
_Noreturn void ld_die(const char *message);
void ld_warn_errno(const char *what);

void *ld_xmalloc(size_t size);
void *ld_xcalloc(size_t count, size_t size);
void *ld_xrealloc(void *pointer, size_t size);
char *ld_xstrdup(const char *text);
char *ld_xstrndup(const char *text, size_t length);

static inline uint16_t ld_read_le16(const uint8_t *p) {
    return infiltratr_load_le16(p);
}

static inline uint32_t ld_read_le32(const uint8_t *p) {
    return infiltratr_load_le32(p);
}


static inline void ld_write_le16(uint8_t *p, uint16_t value) {
    infiltratr_store_le16(p, value);
}

static inline void ld_write_le32(uint8_t *p, uint32_t value) {
    infiltratr_store_le32(p, value);
}


#endif
