// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_PATH_H
#define LD_PATH_H

char *ld_path_append_suffix(const char *base, const char *suffix);
char *ld_path_parent_directory(const char *path);
void ld_path_fsync_parent(const char *path);

#endif
