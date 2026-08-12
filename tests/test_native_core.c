// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stage.h"
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
    if (!ld_u64_add(10, 20, &result) || result != 30) return fail("checked addition");
    if (ld_u64_add(UINT64_MAX, 1, &result)) return fail("addition overflow");
    if (!ld_u64_mul(9, 7, &result) || result != 63) return fail("checked multiplication");
    if (ld_u64_mul(UINT64_MAX, 2, &result)) return fail("multiplication overflow");

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
    LdStageStore stage;
    LdMemoryInfo memory_info;
    if (!ld_stage_init_memory(&stage, 1024U * 1024U, &memory_info))
        return fail("memory staging initialization");
    const char staged[] = "native-memory-stage";
    char staged_back[sizeof(staged)];
    ld_stage_write(&stage, staged, sizeof(staged), 8192);
    memset(staged_back, 0, sizeof(staged_back));
    ld_stage_read(&stage, staged_back, sizeof(staged_back), 8192);
    if (memcmp(staged, staged_back, sizeof(staged)) != 0)
        return fail("memory staging payload");
    if (ld_stage_is_persistent(&stage)) return fail("memory stage persistence flag");
    ld_stage_destroy(&stage);
    close(fd);

    ld_stop_clear();
    if (ld_stop_requested()) return fail("stop clear");
    puts("native core tests passed");
    return 0;
}
