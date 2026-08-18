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

# A Growth pass immediately after canonical Defrag may need no payload cluster
# movement at all. That must not fall back to a filesystem-sized stage image:
# the durable SQLite plan is sufficient to make the metadata update idempotent
# and recoverable.
zero_move_old = '''    if (state->move_clusters == 0) {\n        /* No payload movement is necessary. Let the mature fallback preserve\n           legacy behaviour for unusual metadata-only plans. */\n        result = 2;\n        goto fallback;\n    }\n'''
zero_move_new = '''    if (state->move_clusters == 0) {\n        printf("Raw userspace native-C NTFS relayout engine %s\\n", LD_VERSION);\n        printf("NTFS direct metadata layout: %zu supported streams already have their canonical payload placement; no payload relocation is required.\\n",\n               placements.count);\n        fflush(stdout);\n        if (ld_stop_requested()) { result = 130; goto stopped_unchanged; }\n        if (check_unchanged_target(device, state, error) != 0) goto done;\n        if (journal_phase(journal_path, state, "direct-metadata", error) != 0) goto done;\n        if (mark_source_dirty(device, error) != 0) goto metadata_recover_required;\n        if (ntfs_apply_stage_metadata(device, db, true, error) != 0) goto metadata_recover_required;\n        if (journal_phase(journal_path, state, "direct-verifying-source", error) != 0)\n            goto metadata_recover_required;\n        if (ntfs_verify_stage(device, db, growth, true, error) != 0)\n            goto metadata_recover_required;\n        if (clear_source_dirty(device, error) != 0) goto metadata_recover_required;\n        if (live_updates) emit_committed_live_reset(device, error);\n        transaction_cleanup(journal_path, state);\n        puts("NTFS direct metadata layout: canonical metadata verified without payload relocation or a filesystem-sized working image.");\n        printf("NTFS %s completed with serial and full device capacity preserved.\\n",\n               growth ? "Growth Defrag" : "Defragment");\n        emit_result(operation, "completed", "");\n        result = 0;\n        goto done;\n    }\n'''
if text.count(zero_move_old) != 1:
    raise SystemExit("cannot replace zero-move NTFS fallback")
text = text.replace(zero_move_old, zero_move_new, 1)

recover_label = '''recover_required:\n    /* Durable workspace and plan are deliberately retained. Recovery can\n       replay placement and metadata from their checksummed copies. */\n    result = -1;\n\ndone:\n'''
recover_label_new = '''metadata_recover_required:\n    /* No payload was moved. Keep the durable plan and journal so Recover can\n       idempotently finish the metadata transaction. */\n    result = -1;\n    goto done;\n\nrecover_required:\n    /* Durable workspace and plan are deliberately retained. Recovery can\n       replay placement and metadata from their checksummed copies. */\n    result = -1;\n\ndone:\n'''
if text.count(recover_label) != 1:
    raise SystemExit("cannot add metadata-only NTFS recovery label")
text = text.replace(recover_label, recover_label_new, 1)

recovery_anchor = "recovery_block = r'''\n    if (state.workspace_clusters != 0 && strncmp(state.phase, \"workspace-\", 10) == 0) {\n"
metadata_recovery = """recovery_block = r'''\n    if (strcmp(state.phase, "direct-metadata") == 0 ||\n        strcmp(state.phase, "direct-verifying-source") == 0) {\n        sqlite3 *metadata_db = NULL;\n        if (ntfs_open_plan_db(state.plan, &metadata_db, error) != 0) goto done;\n        bool growth = strcmp(state.operation, "growth-defrag") == 0;\n        puts("Recovering an NTFS metadata-only canonical relayout.");\n        fflush(stdout);\n        if (mark_source_dirty(device, error) != 0 ||\n            journal_phase(journal_path, &state, "direct-metadata", error) != 0 ||\n            ntfs_apply_stage_metadata(device, metadata_db, true, error) != 0 ||\n            journal_phase(journal_path, &state, "direct-verifying-source", error) != 0 ||\n            ntfs_verify_stage(device, metadata_db, growth, true, error) != 0 ||\n            clear_source_dirty(device, error) != 0) {\n            sqlite3_close(metadata_db);\n            goto done;\n        }\n        sqlite3_close(metadata_db);\n        transaction_cleanup(journal_path, &state);\n        puts("NTFS metadata-only recovery completed successfully.");\n        emit_result("recover", "completed", "");\n        result = 0;\n        goto done;\n    }\n\n    if (state.workspace_clusters != 0 && strncmp(state.phase, "workspace-", 10) == 0) {\n"""
if text.count(recovery_anchor) != 1:
    raise SystemExit("cannot add NTFS metadata-only recovery block")
text = text.replace(recovery_anchor, metadata_recovery, 1)

# The Growth regression after a canonical Defrag is intentionally a zero-payload
# metadata path, while the first Defrag must exercise the durable workspace.
growth_test_old = '''    "    output = mutate(worker, image, \\\"growth-defrag\\\", work / \\\"ntfs-growth.journal\\\")\\n"\n    "    assert \\\"NTFS unified workspace layout:\\\" in output, output\\n"\n    "    assert \\\"internally verified raw NTFS working image\\\" not in output, output\\n"\n'''
growth_test_new = '''    "    output = mutate(worker, image, \\\"growth-defrag\\\", work / \\\"ntfs-growth.journal\\\")\\n"\n    "    assert \\\"NTFS direct metadata layout:\\\" in output, output\\n"\n    "    assert \\\"internally verified raw NTFS working image\\\" not in output, output\\n"\n'''
if text.count(growth_test_old) != 1:
    raise SystemExit("cannot adjust NTFS metadata-only Growth regression")
text = text.replace(growth_test_old, growth_test_new, 1)

path.write_text(text, encoding="utf-8")
print("Adjusted NTFS patch generator to current main, per-write locking and direct metadata-only relayout")
