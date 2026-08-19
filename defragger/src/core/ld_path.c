// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_path.h"

#include "ld_runtime.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *ld_path_append_suffix(const char *base, const char *suffix)
{
    const size_t base_length = strlen(base);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > SIZE_MAX - base_length - 1U)
        ld_die("path suffix length overflow");
    char *result = ld_xmalloc(base_length + suffix_length + 1U);
    memcpy(result, base, base_length);
    memcpy(result + base_length, suffix, suffix_length + 1U);
    return result;
}

char *ld_path_parent_directory(const char *path)
{
    char *copy = ld_xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) {
        free(copy);
        return ld_xstrdup(".");
    }
    if (slash == copy) slash[1] = '\0';
    else *slash = '\0';
    return copy;
}

void ld_path_fsync_parent(const char *path)
{
    char *parent = ld_path_parent_directory(path);
    const int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync(fd);
        (void)close(fd);
    }
    free(parent);
}
