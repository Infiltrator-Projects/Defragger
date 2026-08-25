// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/core.h"
#include "ld_io.h"
#include "ld_path.h"
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
    if (!ld_path_is_derived_from("/tmp/a.journal.ext-stage.img",
                                 "/tmp/a.journal", ".ext-stage.img"))
        return fail("derived recovery path");
    if (ld_path_is_derived_from("/tmp/other.ext-stage.img",
                                "/tmp/a.journal", ".ext-stage.img"))
        return fail("unbound recovery path");

    char victim_path[] = "/tmp/linux-defragger-core-victim.XXXXXX";
    int victim_fd = mkstemp(victim_path);
    if (victim_fd < 0) return fail("victim mkstemp");
    const char victim_payload[] = "must-not-be-truncated";
    if (write(victim_fd, victim_payload, sizeof(victim_payload)) !=
        (ssize_t)sizeof(victim_payload))
        return fail("victim write");
    close(victim_fd);
    char journal_path[] = "/tmp/linux-defragger-core-journal.XXXXXX";
    int journal_fd = mkstemp(journal_path);
    if (journal_fd < 0) return fail("journal mkstemp");
    close(journal_fd);
    unlink(journal_path);
    char journal_link[128];
    if (snprintf(journal_link, sizeof(journal_link), "%s.tmp", journal_path) < 0)
        return fail("journal temp path");
    if (symlink(victim_path, journal_link) != 0) return fail("journal temp symlink");
    char *atomic_path = NULL;
    FILE *atomic_file = ld_path_open_atomic_temp(journal_path, &atomic_path);
    if (atomic_file == NULL || atomic_path == NULL) return fail("safe atomic journal temp");
    if (fputs("new-journal\n", atomic_file) < 0 || fclose(atomic_file) != 0)
        return fail("atomic journal write");
    victim_fd = open(victim_path, O_RDONLY | O_CLOEXEC);
    if (victim_fd < 0) return fail("victim reopen");
    char victim_readback[sizeof(victim_payload)] = {0};
    ssize_t victim_read = read(victim_fd, victim_readback, sizeof(victim_readback));
    close(victim_fd);
    if (victim_read != (ssize_t)sizeof(victim_payload) ||
        memcmp(victim_readback, victim_payload, sizeof(victim_payload)) != 0)
        return fail("atomic journal temp followed symlink");
    unlink(atomic_path);
    unlink(victim_path);
    free(atomic_path);

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
