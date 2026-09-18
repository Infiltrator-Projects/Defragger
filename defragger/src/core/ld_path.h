// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_PATH_H
#define LD_PATH_H

#include <stdbool.h>

char *ld_path_append_suffix(const char *base, const char *suffix);
bool ld_path_is_derived_from(const char *candidate, const char *base,
                             const char *suffix);
char *ld_path_parent_directory(const char *path);
int ld_path_ensure_trusted_directory_tree(const char *path);

#endif
