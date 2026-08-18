#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Repair source anchors in the temporary NTFS patch generator."""
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
path.write_text(text, encoding="utf-8")
print("Adjusted NTFS patch-generator anchors to current main")
