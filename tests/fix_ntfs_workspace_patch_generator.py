#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Repair source anchors and validation-only generator details."""
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
replacements = {
    "int ntfs_apply_stage_metadata(const char *stage,sqlite3 *db,char **error){NtfsVolume volume;NtfsLayout layout;NtfsCatalogue catalogue;if(ntfs_open_volume(stage,true,&volume,error)<0)return -1;if(ntfs_read_layout(&volume,false,&layout,error)<0){ntfs_close_volume(&volume);return -1;}":
    "int ntfs_apply_stage_metadata(const char *stage,sqlite3 *db,char **error){NtfsVolume volume;NtfsLayout layout;if(ntfs_open_volume(stage,true,&volume,error)!=0)return -1;if(ntfs_read_layout(&volume,false,&layout,error)!=0){ntfs_close_volume(&volume);return -1;}",
    "int ntfs_apply_stage_metadata(const char *stage,sqlite3 *db,bool allow_dirty,char **error){NtfsVolume volume;NtfsLayout layout;NtfsCatalogue catalogue;if(ntfs_open_volume(stage,true,&volume,error)<0)return -1;if(ntfs_read_layout(&volume,allow_dirty,&layout,error)<0){ntfs_close_volume(&volume);return -1;}":
    "int ntfs_apply_stage_metadata(const char *stage,sqlite3 *db,bool allow_dirty,char **error){NtfsVolume volume;NtfsLayout layout;if(ntfs_open_volume(stage,true,&volume,error)!=0)return -1;if(ntfs_read_layout(&volume,allow_dirty,&layout,error)!=0){ntfs_close_volume(&volume);return -1;}",
    "static NtfsStream *find_stream(NtfsCatalogue *catalogue,uint64_t record,uint32_t offset)":
    "static NtfsStream *find_stream(NtfsCatalogue *catalogue,uint64_t record,uint32_t attr)",
}
for old, new in replacements.items():
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"temporary NTFS generator anchor count {count}, expected 1: {old[:100]!r}")
    text = text.replace(old, new, 1)

# The direct path originally held a second long-lived flock while existing
# native metadata helpers opened and flocked the same target themselves. Linux
# flock locks belong to open file descriptions, so that self-conflicted. Keep
# the mature per-write locking model instead: each raw workspace write takes an
# exclusive lock, and the existing dirty/metadata writers take their own lock.
anchor = 'plan = ROOT / "gui/filesystems/ntfs/native/ntfs_plan.c"\nreplace_once(\n'
replacement = (
    'plan = ROOT / "gui/filesystems/ntfs/native/ntfs_plan.c"\n'
    'replace_once(plan, "#include <strings.h>\\n", "#include <strings.h>\\n#include <sys/file.h>\\n")\n'
    'replace_once(\n'
)
if text.count(anchor) != 1:
    raise SystemExit("cannot add sys/file.h to generated NTFS planner")
text = text.replace(anchor, replacement, 1)

open_anchor = '''    int fd = open(device, O_RDWR | O_CLOEXEC);\n    if (fd < 0) {\n        ntfs_set_error(error, "cannot open NTFS source for terminal-workspace I/O: %s",\n                       strerror(errno));\n        return -1;\n    }\n    sqlite3_stmt *rows = NULL, *update = NULL;\n'''
open_replacement = '''    int fd = open(device, O_RDWR | O_CLOEXEC);\n    if (fd < 0) {\n        ntfs_set_error(error, "cannot open NTFS source for terminal-workspace I/O: %s",\n                       strerror(errno));\n        return -1;\n    }\n    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {\n        ntfs_set_error(error, "cannot lock NTFS source for terminal-workspace I/O: %s",\n                       strerror(errno));\n        close(fd);\n        return -1;\n    }\n    sqlite3_stmt *rows = NULL, *update = NULL;\n'''
if text.count(open_anchor) != 1:
    raise SystemExit("cannot add per-write NTFS workspace flock")
text = text.replace(open_anchor, open_replacement, 1)

for old in (
    '    int lock_fd = -1;\n',
    '''    lock_fd = open(device, O_RDWR | O_CLOEXEC);\n    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {\n        ntfs_set_error(error, "cannot lock NTFS source for direct relayout: %s", strerror(errno));\n        goto done;\n    }\n\n''',
    '    if (lock_fd >= 0) { (void)flock(lock_fd, LOCK_UN); close(lock_fd); }\n',
):
    if text.count(old) != 1:
        raise SystemExit(f"cannot remove conflicting long-lived NTFS lock: {old[:70]!r}")
    text = text.replace(old, "", 1)

path.write_text(text, encoding="utf-8")
print("Adjusted NTFS patch generator to current main and per-write locking")
