// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char script[8192];
    const LdtmFilesystemSpec *fat12;
    const LdtmFilesystemSpec *fat16;
    LdtmFragmentProfile small;
    LdtmFragmentProfile normal;
    size_t count = 0U;
    const char *cursor;

    assert(ldtm_spec_count() == 18U);
    assert(ldtm_allocated_capacity_bytes() == UINT64_C(33344) * LDTM_MIB);
    assert(ldtm_required_capacity_bytes() == (UINT64_C(33344) * LDTM_MIB) + LDTM_GIB);

    fat12 = ldtm_find_spec("fat12");
    fat16 = ldtm_find_spec("fat16");
    assert(fat12 != NULL && fat16 != NULL);
    small = ldtm_fragment_profile(fat12);
    normal = ldtm_fragment_profile(fat16);
    assert(ldtm_target_payload_bytes(fat12) == UINT64_C(4) * LDTM_MIB);
    assert(ldtm_target_payload_bytes(fat16) == UINT64_C(200) * LDTM_MIB);
    assert(small.directory_initial == 128U);
    assert(small.directory_second == 128U);
    assert(normal.directory_initial == 4096U);
    assert(normal.directory_second == 4096U);
    assert((small.directory_initial / 2U + small.directory_second) == 192U);

    assert(ldtm_transport_is_field_media(0, "mmc") == 1);
    assert(ldtm_transport_is_field_media(0, "usb") == 1);
    assert(ldtm_transport_is_field_media(1, "unknown") == 1);
    assert(ldtm_transport_is_field_media(0, "nvme") == 0);

    assert(ldtm_build_sfdisk_script(script, sizeof(script)) == 0);
    assert(strstr(script, "label: gpt") != NULL);
    assert(strstr(script, "name=\"LD_FAT12\"") != NULL);
    assert(strstr(script, "name=\"LD_ZFS\"") != NULL);
    assert(strstr(script, "type=swap, name=\"LD_SWAP\"") != NULL);
    cursor = script;
    while ((cursor = strstr(cursor, "name=\"LD_")) != NULL) {
        ++count;
        cursor += 6;
    }
    assert(count == 18U);

    puts("test-media core tests passed");
    return 0;
}
