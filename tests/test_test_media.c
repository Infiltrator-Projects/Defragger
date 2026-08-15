// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "test-media check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int test_amiga_formatters(void) {
    char path[] = "/tmp/linux-defragger-amiga-media.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)(128U * LDTM_MIB)) != 0) {
        (void)close(fd);
        (void)unlink(path);
        return 1;
    }
    if (close(fd) != 0) {
        (void)unlink(path);
        return 1;
    }
    if (ldtm_format_amiga_volume(path, 0U, "LD_OFS") != 0 ||
        ldtm_validate_amiga_volume(path, 0U) != 0 ||
        ldtm_validate_amiga_volume(path, 1U) == 0) {
        (void)unlink(path);
        return 1;
    }
    if (ldtm_format_amiga_volume(path, 1U, "LD_FFS") != 0 ||
        ldtm_validate_amiga_volume(path, 1U) != 0 ||
        ldtm_validate_amiga_volume(path, 0U) == 0) {
        (void)unlink(path);
        return 1;
    }
    return unlink(path) == 0 ? 0 : 1;
}

int main(void) {
    char script[8192];
    const LdtmFilesystemSpec *fat12;
    const LdtmFilesystemSpec *fat16;
    const LdtmFilesystemSpec *ofs;
    const LdtmFilesystemSpec *ffs;
    const LdtmFilesystemSpec *sfs;
    const LdtmFilesystemSpec *pfs3;
    const LdtmFilesystemSpec *ufs;
    const LdtmFilesystemSpec *zfs;
    const LdtmFilesystemSpec *apfs;
    LdtmFragmentProfile small;
    LdtmFragmentProfile normal;
    size_t count = 0U;
    const char *cursor;

    CHECK(ldtm_spec_count() == 21U);
    CHECK(ldtm_allocated_capacity_bytes() == UINT64_C(36416) * LDTM_MIB);
    CHECK(ldtm_required_capacity_bytes() == (UINT64_C(36416) * LDTM_MIB) + LDTM_GIB);

    fat12 = ldtm_find_spec("fat12");
    fat16 = ldtm_find_spec("fat16");
    ofs = ldtm_find_spec("ofs");
    ffs = ldtm_find_spec("ffs");
    sfs = ldtm_find_spec("sfs");
    pfs3 = ldtm_find_spec("pfs3");
    ufs = ldtm_find_spec("ufs");
    zfs = ldtm_find_spec("zfs");
    apfs = ldtm_find_spec("apfs");
    CHECK(fat12 != NULL && fat16 != NULL);
    CHECK(ofs != NULL && ffs != NULL && sfs != NULL && pfs3 != NULL);
    CHECK(ufs != NULL && zfs != NULL && apfs != NULL);

    small = ldtm_fragment_profile(fat12);
    normal = ldtm_fragment_profile(fat16);
    CHECK(ldtm_target_payload_bytes(fat12) == UINT64_C(4) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(fat16) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(ofs) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(ffs) == UINT64_C(200) * LDTM_MIB);
    CHECK(small.directory_initial == 128U);
    CHECK(small.directory_second == 128U);
    CHECK(normal.directory_initial == 4096U);
    CHECK(normal.directory_second == 4096U);
    CHECK((small.directory_initial / 2U + small.directory_second) == 192U);

    CHECK(ofs->creator == LDTM_CREATOR_AFFS);
    CHECK(ffs->creator == LDTM_CREATOR_AFFS);
    CHECK(strcmp(ldtm_creator_program(ofs), "/usr/lib/linux-defragger/test-media-mkfs-ofs") == 0);
    CHECK(strcmp(ldtm_creator_program(ffs), "/usr/lib/linux-defragger/test-media-mkfs-ffs") == 0);
    CHECK(sfs->creator == LDTM_CREATOR_MANUAL);
    CHECK(pfs3->creator == LDTM_CREATOR_MANUAL);
    CHECK(strstr(sfs->note, "SFS/SFS2") != NULL);
    CHECK(strstr(pfs3->note, "PFS3") != NULL);
    CHECK(strcmp(ldtm_creator_program(ufs), "mkfs.ufs") == 0);
    CHECK(ufs->package_hint != NULL && ufs->package_hint[0] == '\0');
    CHECK(zfs->package_hint != NULL && strcmp(zfs->package_hint, "zfsutils-linux") == 0);
    CHECK(apfs->creator == LDTM_CREATOR_MANUAL);
    CHECK(ldtm_creator_program(apfs) == NULL);
    CHECK(test_amiga_formatters() == 0);

    CHECK(ldtm_transport_is_field_media(0, "mmc") == 1);
    CHECK(ldtm_transport_is_field_media(0, "usb") == 1);
    CHECK(ldtm_transport_is_field_media(1, "unknown") == 1);
    CHECK(ldtm_transport_is_field_media(0, "nvme") == 0);

    CHECK(ldtm_build_sfdisk_script(script, sizeof(script)) == 0);
    CHECK(strstr(script, "label: gpt") != NULL);
    CHECK(strstr(script, "name=\"LD_FAT12\"") != NULL);
    CHECK(strstr(script, "name=\"LD_OFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_FFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_SFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_PFS3\"") != NULL);
    CHECK(strstr(script, "name=\"LD_ZFS\"") != NULL);
    CHECK(strstr(script, "type=swap, name=\"LD_SWAP\"") != NULL);
    cursor = script;
    while ((cursor = strstr(cursor, "name=\"LD_")) != NULL) {
        ++count;
        cursor += 6;
    }
    CHECK(count == 21U);

    puts("test-media core tests passed");
    return 0;
}
