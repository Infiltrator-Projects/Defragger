#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

core_path = Path("test_media/test_media_core.c")
core = core_path.read_text(encoding="utf-8")
core = core.replace(
'''    {"ufs", "LD_UFS", 2048U, 200U, LDTM_CREATOR_UFS, "",
     "Created only when mkfs.ufs is installed."},''',
'''    {"ufs", "LD_UFS", 2048U, 200U, LDTM_CREATOR_UFS, "makefs",
     "Creates a genuine UFS2/FFS image with makefs; exact fragmentation is not asserted yet."},''')
core = core.replace('case LDTM_CREATOR_UFS: return "mkfs.ufs";',
                    'case LDTM_CREATOR_UFS: return "makefs";')
core = core.replace('"Built-in C creator: Amiga DOS\\\\0 OFS"',
                    '"Built-in raw C creator + payload engine: Amiga DOS\\\\0 OFS"')
core = core.replace('"Built-in C creator: Amiga DOS\\\\1 FFS"',
                    '"Built-in raw C creator + payload engine: Amiga DOS\\\\1 FFS"')
if 'case LDTM_CREATOR_UFS: return "makefs";' not in core:
    raise SystemExit("UFS creator replacement failed")
core_path.write_text(core, encoding="utf-8")

worker_path = Path("test_media/test_media_worker.c")
worker = worker_path.read_text(encoding="utf-8")
worker = worker.replace('#include "test_media.h"\n', '#include "test_media.h"\n#include "ufs_native.h"\n', 1)
worker = worker.replace('const uint8_t dostype = strcmp(spec->key, "ofs") == 0 ? 0U : 1U;',
                        'const uint8_t dostype = strcmp(spec->key, "ofs") == 0 ? (uint8_t)0 : (uint8_t)1;')

anchor = '''static int create_zfs_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   const char *work, const char *device, FILE *state) {
'''
insert = r'''static int copy_image_to_partition(const char *image, const char *partition) {
    int input = -1;
    int output = -1;
    unsigned char *buffer = NULL;
    int result = -1;
    input = open(image, O_RDONLY | O_CLOEXEC);
    if (input < 0) goto cleanup;
    output = open(partition, O_WRONLY | O_CLOEXEC);
    if (output < 0) goto cleanup;
    buffer = malloc(1024U * 1024U);
    if (buffer == NULL) goto cleanup;
    for (;;) {
        ssize_t got = read(input, buffer, 1024U * 1024U);
        if (got < 0) {
            if (errno == EINTR) continue;
            goto cleanup;
        }
        if (got == 0) break;
        if (write_all(output, buffer, (size_t)got) != 0) goto cleanup;
    }
    if (fsync(output) != 0) goto cleanup;
    result = 0;
cleanup:
    free(buffer);
    if (output >= 0) (void)close(output);
    if (input >= 0) (void)close(input);
    return result;
}

static int ufs2_summary_ok(const char *path) {
    LdUfsSummary summary;
    char error[256];
    return ufs_read_summary(path, &summary, error, sizeof(error)) == 0 &&
           summary.variant == LD_UFS_VARIANT_UFS2_LE;
}

static int create_ufs_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   const char *work, FILE *state) {
    char source[PATH_MAX];
    char image[PATH_MAX];
    LdtmTargetRecord records[LDTM_MAX_TARGET_FILES];
    size_t record_count = 0U;
    uint32_t directory_entries = 0U;
    const char *const wipe_argv[] = {"wipefs", "--all", "--force", partition, NULL};
    const char *makefs_argv[12];
    if (!ldtm_program_available("makefs")) {
        emit_status(spec->key, "skipped", "makefs is not installed");
        (void)state_write_status(state, spec, "skipped", "makefs is not installed");
        return 0;
    }
    if (run_process(wipe_argv, NULL, 0) != 0 ||
        snprintf(source, sizeof(source), "%s/ufs-source", work) <= 0 ||
        snprintf(image, sizeof(image), "%s/ufs.img", work) <= 0 ||
        ensure_directory(source, 0755) != 0) {
        emit_status(spec->key, "format-failed", "could not prepare UFS image workspace");
        (void)state_write_status(state, spec, "format-failed", "UFS workspace preparation failed");
        return 0;
    }
    if (generate_fragmented_data(spec, source, records, &record_count, &directory_entries) != 0) {
        emit_status(spec->key, "format-failed", "could not build deterministic UFS source tree");
        (void)state_write_status(state, spec, "format-failed", "UFS source tree generation failed");
        return 0;
    }
    makefs_argv[0] = "makefs";
    makefs_argv[1] = "-t";
    makefs_argv[2] = "ffs";
    makefs_argv[3] = "-B";
    makefs_argv[4] = "little";
    makefs_argv[5] = "-s";
    makefs_argv[6] = "512m";
    makefs_argv[7] = "-o";
    makefs_argv[8] = "version=2,bsize=8192,fsize=1024,minfree=5";
    makefs_argv[9] = image;
    makefs_argv[10] = source;
    makefs_argv[11] = NULL;
    if (run_process(makefs_argv, NULL, 0) != 0 || !ufs2_summary_ok(image)) {
        emit_status(spec->key, "format-failed", "makefs did not produce a recognised UFS2 image");
        (void)state_write_status(state, spec, "format-failed", "makefs UFS2 validation failed");
        return 0;
    }
    printf("+ copy verified UFS2 image %s -> %s\n", image, partition);
    fflush(stdout);
    if (copy_image_to_partition(image, partition) != 0 || !ufs2_summary_ok(partition)) {
        emit_status(spec->key, "format-failed", "UFS2 image copy could not be independently validated");
        (void)state_write_status(state, spec, "format-failed", "UFS2 partition validation failed");
        return 0;
    }
    if (state_write_status(state, spec, "populated",
                           "deterministic UFS2 payload created with makefs; exact fragmentation not asserted") != 0 ||
        state_write_targets(state, spec, records, record_count, directory_entries) != 0) return -1;
    emit_status(spec->key, "populated",
                "deterministic UFS2 payload created with makefs; exact fragmentation not asserted");
    return 0;
}

static int create_zfs_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   const char *work, const char *device, FILE *state) {
'''
if worker.count(anchor) != 1:
    raise SystemExit("ZFS function anchor mismatch")
worker = worker.replace(anchor, insert, 1)

old_dispatch = '''        if (spec->creator == LDTM_CREATOR_AFFS) {
            if (create_amiga_and_populate(spec, partition, state) != 0) goto cleanup;
        } else if (spec->creator == LDTM_CREATOR_ZFS) {
            if (create_zfs_and_populate(spec, partition, work, canonical, state) != 0) goto cleanup;
        } else {
            if (create_regular_and_populate(spec, partition, work, state) != 0) goto cleanup;
        }
'''
new_dispatch = '''        if (spec->creator == LDTM_CREATOR_AFFS) {
            if (create_amiga_and_populate(spec, partition, state) != 0) goto cleanup;
        } else if (spec->creator == LDTM_CREATOR_UFS) {
            if (create_ufs_and_populate(spec, partition, work, state) != 0) goto cleanup;
        } else if (spec->creator == LDTM_CREATOR_ZFS) {
            if (create_zfs_and_populate(spec, partition, work, canonical, state) != 0) goto cleanup;
        } else {
            if (create_regular_and_populate(spec, partition, work, state) != 0) goto cleanup;
        }
'''
if worker.count(old_dispatch) != 1:
    raise SystemExit("prepare dispatch anchor mismatch")
worker = worker.replace(old_dispatch, new_dispatch, 1)
if worker.count('const uint8_t dostype = strcmp(spec->key, "ofs") == 0 ? (uint8_t)0 : (uint8_t)1;') != 2:
    raise SystemExit("uint8_t dispatch fixes did not apply twice")
worker_path.write_text(worker, encoding="utf-8")
