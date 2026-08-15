#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

path = Path("test_media/test_media_worker.c")
text = path.read_text(encoding="utf-8")

needle = '''static int create_regular_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                       const char *work, FILE *state) {
'''
insert = '''static int create_amiga_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                     FILE *state) {
    const char *const wipe_argv[] = {"wipefs", "--all", "--force", partition, NULL};
    const uint8_t dostype = strcmp(spec->key, "ofs") == 0 ? 0U : 1U;
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    char detail[512];
    if (run_process(wipe_argv, NULL, 0) != 0) {
        emit_status(spec->key, "format-failed", "could not clear old filesystem signatures");
        (void)state_write_status(state, spec, "format-failed", "wipefs failed");
        return 0;
    }
    printf("+ built-in raw C Amiga DOS\\\\%u formatter/populator %s\\n", dostype, partition);
    fflush(stdout);
    if (ldtm_format_amiga_volume(partition, dostype, spec->label) != 0) {
        emit_status(spec->key, "format-failed", "built-in C Amiga formatter failed");
        (void)state_write_status(state, spec, "format-failed", "built-in C formatter failed");
        return 0;
    }
    if (ldtm_populate_amiga_volume(partition, dostype, &profile) != 0) {
        emit_status(spec->key, "formatted-unpopulated", "raw C Amiga fragmentation payload generation failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "raw C payload generation failed");
        return 0;
    }
    detail[0] = '\\0';
    if (ldtm_verify_amiga_payload(partition, dostype, &profile, detail, sizeof(detail)) != 0) {
        emit_status(spec->key, "formatted-unpopulated",
                    detail[0] != '\\0' ? detail : "raw C Amiga payload self-check failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "raw C payload self-check failed");
        return 0;
    }
    if (state_write_status(state, spec, "populated",
                           "raw C fragmented deterministic payload created without kernel mounting") != 0) return -1;
    emit_status(spec->key, "populated",
                "raw C fragmented deterministic payload created without kernel mounting");
    return 0;
}

static int create_regular_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                       const char *work, FILE *state) {
'''
if text.count(needle) != 1:
    raise SystemExit("create_regular_and_populate anchor mismatch")
text = text.replace(needle, insert, 1)

old_prepare = '''        if (spec->creator == LDTM_CREATOR_ZFS) {
            if (create_zfs_and_populate(spec, partition, work, canonical, state) != 0) goto cleanup;
        } else {
            if (create_regular_and_populate(spec, partition, work, state) != 0) goto cleanup;
        }
'''
new_prepare = '''        if (spec->creator == LDTM_CREATOR_AFFS) {
            if (create_amiga_and_populate(spec, partition, state) != 0) goto cleanup;
        } else if (spec->creator == LDTM_CREATOR_ZFS) {
            if (create_zfs_and_populate(spec, partition, work, canonical, state) != 0) goto cleanup;
        } else {
            if (create_regular_and_populate(spec, partition, work, state) != 0) goto cleanup;
        }
'''
if text.count(old_prepare) != 1:
    raise SystemExit("prepare dispatch anchor mismatch")
text = text.replace(old_prepare, new_prepare, 1)

old_verify = '''        if (spec->creator == LDTM_CREATOR_ZFS) {
            (void)verify_zfs(spec, work, &expected[index]);
            continue;
        }
        partition = partition_for_label(&map, spec->label);
        if (partition == NULL) {
            emit_status(spec->key, "verify-failed", "partition label is missing");
            continue;
        }
        if (snprintf(mountpoint, sizeof(mountpoint), "%s/%s", work, spec->key) <= 0 ||
            ensure_directory(mountpoint, 0755) != 0) continue;
'''
new_verify = '''        if (spec->creator == LDTM_CREATOR_ZFS) {
            (void)verify_zfs(spec, work, &expected[index]);
            continue;
        }
        partition = partition_for_label(&map, spec->label);
        if (partition == NULL) {
            emit_status(spec->key, "verify-failed", "partition label is missing");
            continue;
        }
        if (spec->creator == LDTM_CREATOR_AFFS) {
            const uint8_t dostype = strcmp(spec->key, "ofs") == 0 ? 0U : 1U;
            const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
            char detail[512];
            detail[0] = '\\0';
            if (ldtm_verify_amiga_payload(partition, dostype, &profile, detail, sizeof(detail)) == 0) {
                emit_status(spec->key, "verified",
                            detail[0] != '\\0' ? detail : "raw C Amiga payload verified");
            } else {
                emit_status(spec->key, "verify-failed",
                            detail[0] != '\\0' ? detail : "raw C Amiga payload verification failed");
            }
            continue;
        }
        if (snprintf(mountpoint, sizeof(mountpoint), "%s/%s", work, spec->key) <= 0 ||
            ensure_directory(mountpoint, 0755) != 0) continue;
'''
if text.count(old_verify) != 1:
    raise SystemExit("verify dispatch anchor mismatch")
text = text.replace(old_verify, new_verify, 1)

path.write_text(text, encoding="utf-8")
