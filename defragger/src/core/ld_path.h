// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_PATH_H
#define LD_PATH_H

#include <stdbool.h>
#include <stdio.h>

char *ld_path_append_suffix(const char *base, const char *suffix);
bool ld_path_is_derived_from(const char *candidate, const char *base,
                             const char *suffix);
FILE *ld_path_open_atomic_temp(const char *target_path, char **temporary_path);
char *ld_path_parent_directory(const char *path);
int ld_path_ensure_trusted_directory_tree(const char *path);
void ld_path_fsync_parent(const char *path);

#endif
