// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <stdio.h>
#include <stdint.h>

#ifndef LDTM_AMIGA_DOSTYPE
#error LDTM_AMIGA_DOSTYPE must be defined as 0 or 1
#endif

#ifndef LDTM_AMIGA_LABEL
#define LDTM_AMIGA_LABEL "LD_AMIGA"
#endif

int main(int argc, char **argv) {
    const uint8_t dostype = (uint8_t)LDTM_AMIGA_DOSTYPE;
    if (argc != 2) {
        fprintf(stderr, "usage: %s DEVICE_OR_IMAGE\n", argv[0]);
        return 2;
    }
    if (ldtm_format_amiga_volume(argv[1], dostype, LDTM_AMIGA_LABEL) != 0) {
        fprintf(stderr, "failed to format %s as Amiga DOS\\%u\n", argv[1], (unsigned)dostype);
        return 1;
    }
    if (ldtm_validate_amiga_volume(argv[1], dostype) != 0) {
        fprintf(stderr, "Amiga DOS\\%u post-format validation failed\n", (unsigned)dostype);
        return 1;
    }
    printf("Formatted %s as Amiga %s (DOS\\%u)\n",
           argv[1], dostype == 0U ? "OFS" : "FFS", (unsigned)dostype);
    return 0;
}
