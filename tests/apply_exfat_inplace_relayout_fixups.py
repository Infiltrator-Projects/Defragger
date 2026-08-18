#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hardening fixups applied after the generated exFAT relayout patch."""

from pathlib import Path

root = Path('.')
relayout_path = root / 'gui/filesystems/exfat/native/exfat_relayout.c'
text = relayout_path.read_text(encoding='utf-8')

boot_anchor = '''static int write_boot_state(ExfatVolume *volume, uint32_t root_cluster,\n                            bool dirty, uint8_t percent_in_use,\n                            char **error) {\n'''
dirty_writer = r'''static int write_dirty_flag_only(ExfatVolume *volume, bool dirty, char **error) {
    uint16_t flags = volume->volume_flags;
    if (dirty) flags |= EXFAT_VOLUME_DIRTY;
    else flags &= (uint16_t)~EXFAT_VOLUME_DIRTY;
    uint8_t bytes[2] = {(uint8_t)flags, (uint8_t)(flags >> 8)};
    uint64_t backup = (uint64_t)12U * volume->bytes_per_sector + 106U;
    if (ld_pwrite_full(volume->fd, bytes, sizeof(bytes), backup) !=
            (ssize_t)sizeof(bytes) ||
        fsync(volume->fd) != 0 ||
        ld_pwrite_full(volume->fd, bytes, sizeof(bytes), 106U) !=
            (ssize_t)sizeof(bytes) ||
        fsync(volume->fd) != 0) {
        exfat_set_error(error, "cannot publish exFAT dirty transaction flag");
        return -1;
    }
    volume->volume_flags = flags;
    return 0;
}

'''
if dirty_writer not in text:
    if boot_anchor not in text:
        raise SystemExit('write_boot_state anchor not found')
    text = text.replace(boot_anchor, dirty_writer + boot_anchor, 1)

old_dirty_call = '''    if (write_boot_state(&volume, volume.root_cluster, true,\n                         volume.percent_in_use, error) != 0) {\n'''
new_dirty_call = '''    if (write_dirty_flag_only(&volume, true, error) != 0) {\n'''
if old_dirty_call not in text:
    raise SystemExit('initial dirty-state call anchor not found')
text = text.replace(old_dirty_call, new_dirty_call, 1)

recover_anchor = '''int exfat_relayout_recover(const char *device, const char *journal_path,\n                           size_t ram_bytes, size_t batch_clusters,\n                           bool live_updates, bool *handled, char **error) {\n'''
repair_code = r'''static bool boot_region_valid(const uint8_t *region, uint32_t bytes_per_sector) {
    if (memcmp(region + 3U, "EXFAT   ", 8U) != 0) return false;
    uint32_t expected = exfat_boot_checksum(region, bytes_per_sector);
    const uint8_t *checksum_sector =
        region + (size_t)11U * bytes_per_sector;
    for (uint32_t offset = 0; offset + 4U <= bytes_per_sector; offset += 4U) {
        if (exfat_u32(checksum_sector, offset) != expected) return false;
    }
    return true;
}

/* A crash can interrupt publication of one of exFAT's two boot regions.  The
   relayout manifest contains the immutable sector geometry, so Recover repairs
   one torn copy from the other valid copy before the normal volume parser is
   allowed to inspect the transaction.  Data placement never begins until both
   boot copies have first been made valid and dirty. */
static int repair_boot_regions_from_survivor(const char *device,
                                             uint32_t bytes_per_sector,
                                             char **error) {
    if (bytes_per_sector < 512U || bytes_per_sector > 4096U ||
        (bytes_per_sector & (bytes_per_sector - 1U)) != 0U) {
        exfat_set_error(error, "exFAT recovery journal has invalid sector geometry");
        return -1;
    }
    char *real = realpath(device, NULL);
    if (real == NULL) {
        exfat_set_error(error, "cannot resolve exFAT recovery target: %s",
                        strerror(errno));
        return -1;
    }
    struct stat status;
    if (stat(real, &status) != 0 ||
        (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode))) {
        exfat_set_error(error, "exFAT recovery target is not a block device or image");
        free(real);
        return -1;
    }
    if (S_ISBLK(status.st_mode) && ld_path_is_mounted(real)) {
        exfat_set_error(error, "exFAT recovery target is mounted");
        free(real);
        return -1;
    }
    int flags = O_RDWR | O_CLOEXEC;
    if (S_ISBLK(status.st_mode)) flags |= O_EXCL;
    int fd = open(real, flags);
    if (fd < 0) {
        exfat_set_error(error, "cannot open exFAT recovery target: %s",
                        strerror(errno));
        free(real);
        return -1;
    }
    free(real);
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        exfat_set_error(error, "cannot lock exFAT recovery target: %s",
                        strerror(errno));
        close(fd);
        return -1;
    }

    size_t region_bytes = (size_t)12U * bytes_per_sector;
    uint8_t *regions = ld_xmalloc(region_bytes * 2U);
    if (ld_pread_full(fd, regions, region_bytes * 2U, 0U) !=
        (ssize_t)(region_bytes * 2U)) {
        exfat_set_error(error, "cannot read exFAT boot regions during recovery");
        free(regions);
        (void)flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    bool main_valid = boot_region_valid(regions, bytes_per_sector);
    bool backup_valid = boot_region_valid(regions + region_bytes,
                                          bytes_per_sector);
    if (!main_valid && !backup_valid) {
        exfat_set_error(error,
                        "both exFAT boot-region copies are invalid; automatic recovery cannot establish geometry");
        free(regions);
        (void)flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    if (!main_valid || !backup_valid) {
        const uint8_t *survivor = main_valid ? regions : regions + region_bytes;
        uint64_t destination = main_valid ? region_bytes : 0U;
        if (ld_pwrite_full(fd, survivor, region_bytes, destination) !=
                (ssize_t)region_bytes || fsync(fd) != 0) {
            exfat_set_error(error, "cannot repair the torn exFAT boot-region copy");
            free(regions);
            (void)flock(fd, LOCK_UN);
            close(fd);
            return -1;
        }
        fprintf(stderr,
                "exFAT recovery repaired one interrupted boot-region copy from its valid twin.\n");
    }
    free(regions);
    (void)flock(fd, LOCK_UN);
    close(fd);
    return 0;
}

'''
if repair_code not in text:
    if recover_anchor not in text:
        raise SystemExit('recover function anchor not found')
    text = text.replace(recover_anchor, repair_code + recover_anchor, 1)

recover_open = '''    ExfatVolume volume;\n    if (exfat_open_volume(device, true, true, &volume, error) != 0) {\n'''
recover_open_new = '''    if (repair_boot_regions_from_survivor(device, manifest.bytes_per_sector,\n                                          error) != 0) {\n        manifest_free(&manifest);\n        return 1;\n    }\n\n    ExfatVolume volume;\n    if (exfat_open_volume(device, true, true, &volume, error) != 0) {\n'''
# Only replace the recovery occurrence, not any earlier volume opening.
recover_pos = text.index(recover_anchor)
open_pos = text.find(recover_open, recover_pos)
if open_pos == -1:
    raise SystemExit('recovery volume-open anchor not found')
text = text[:open_pos] + text[open_pos:].replace(recover_open, recover_open_new, 1)
relayout_path.write_text(text, encoding='utf-8')

# The new fast-path status sentence intentionally says that no full-filesystem
# working image is used.  Reject only the actual legacy path, not that sentence.
test_path = root / 'tests/test_exfat_relayout_engine.sh'
test = test_path.read_text(encoding='utf-8')
test = test.replace(
    "if grep -q 'working image\\|shadow-image compatibility' \"$WORK/defrag.log\"; then",
    "if grep -Eq 'internally verified native-C exFAT working image|shadow-image compatibility path' \"$WORK/defrag.log\"; then")
test = test.replace(
    "if grep -q 'working image\\|shadow-image compatibility' \"$WORK/growth.log\"; then",
    "if grep -Eq 'internally verified native-C exFAT working image|shadow-image compatibility path' \"$WORK/growth.log\"; then")
test_path.write_text(test, encoding='utf-8')

print('Applied exFAT relayout hardening fixups')
