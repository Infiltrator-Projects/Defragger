// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_program_name = "linux-defragger";

void ld_runtime_set_program_name(const char *name) {
    if (name != NULL && *name != '\0') g_program_name = name;
}


_Noreturn void ld_die_errno(const char *what) {
    fprintf(stderr, "%s: %s: %s\n", g_program_name, what, strerror(errno));
    exit(EXIT_FAILURE);
}

_Noreturn void ld_die(const char *message) {
    fprintf(stderr, "%s: %s\n", g_program_name, message);
    exit(EXIT_FAILURE);
}

void ld_warn_errno(const char *what) {
    fprintf(stderr, "%s: warning: %s: %s\n", g_program_name, what, strerror(errno));
}

void *ld_xmalloc(size_t size) {
    void *pointer = malloc(size == 0 ? 1 : size);
    if (pointer == NULL) ld_die_errno("malloc");
    return pointer;
}

void *ld_xcalloc(size_t count, size_t size) {
    void *pointer = calloc(count == 0 ? 1 : count, size == 0 ? 1 : size);
    if (pointer == NULL) ld_die_errno("calloc");
    return pointer;
}

void *ld_xrealloc(void *pointer, size_t size) {
    void *replacement = realloc(pointer, size == 0 ? 1 : size);
    if (replacement == NULL) ld_die_errno("realloc");
    return replacement;
}

char *ld_xstrdup(const char *text) {
    char *copy = strdup(text);
    if (copy == NULL) ld_die_errno("strdup");
    return copy;
}

char *ld_xstrndup(const char *text, size_t length) {
    char *copy = ld_xmalloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

bool ld_u64_add(uint64_t left, uint64_t right, uint64_t *result) {
    if (left > UINT64_MAX - right) return false;
    *result = left + right;
    return true;
}

