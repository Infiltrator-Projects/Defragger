// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/core.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *message) {
    fprintf(stderr, "native-core test failed: %s\n", message);
    return 1;
}

int main(void) {
    uint8_t encoded[8] = {0};
    ld_write_le16(encoded, UINT16_C(0xa55a));
    ld_write_le32(encoded + 2, UINT32_C(0x89abcdef));
    if (ld_read_le16(encoded) != UINT16_C(0xa55a)) return fail("le16 codec");
    if (ld_read_le32(encoded + 2) != UINT32_C(0x89abcdef)) return fail("le32 codec");

    uint64_t result = 0;
    if (!infiltratr_u64_add_checked(10, 20, &result) || result != 30)
        return fail("checked addition");
    if (infiltratr_u64_add_checked(UINT64_MAX, 1, &result))
        return fail("addition overflow");

    char path[] = "/tmp/linux-defragger-core-test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return fail("mkstemp");
    unlink(path);
    const char payload[] = "shared-native-core";
    ld_pwrite_exact(fd, payload, sizeof(payload), 4096, "test write");
    char readback[sizeof(payload)];
    memset(readback, 0, sizeof(readback));
    ld_pread_exact(fd, readback, sizeof(readback), 4096, "test read");
    if (memcmp(payload, readback, sizeof(payload)) != 0) return fail("exact I/O payload");
    close(fd);

    ld_stop_clear();
    if (ld_stop_requested()) return fail("stop clear");
    puts("native core tests passed");
    return 0;
}
