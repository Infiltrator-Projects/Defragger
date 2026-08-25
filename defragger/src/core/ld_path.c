// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_path.h"

#include "ld_runtime.h"

#include <errno.h>
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

bool ld_path_is_derived_from(const char *candidate, const char *base,
                             const char *suffix)
{
    if (candidate == NULL || base == NULL || suffix == NULL) return false;
    const size_t candidate_length = strlen(candidate);
    const size_t base_length = strlen(base);
    const size_t suffix_length = strlen(suffix);
    if (base_length > SIZE_MAX - suffix_length ||
        candidate_length != base_length + suffix_length)
        return false;
    return memcmp(candidate, base, base_length) == 0 &&
           memcmp(candidate + base_length, suffix, suffix_length) == 0;
}

FILE *ld_path_open_atomic_temp(const char *target_path, char **temporary_path)
{
    if (target_path == NULL || temporary_path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    *temporary_path = NULL;
    char *temporary = ld_path_append_suffix(target_path, ".tmp");
    if (unlink(temporary) != 0 && errno != ENOENT) {
        free(temporary);
        return NULL;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(temporary, flags, 0600);
    if (fd < 0) {
        free(temporary);
        return NULL;
    }
    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
        int saved_errno = errno;
        close(fd);
        (void)unlink(temporary);
        free(temporary);
        errno = saved_errno;
        return NULL;
    }
    *temporary_path = temporary;
    return file;
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
