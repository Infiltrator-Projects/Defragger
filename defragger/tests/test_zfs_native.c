// SPDX-License-Identifier: GPL-3.0-or-later
#include "zfs_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SMALL_BYTES (1024U * 1024U)
#define LARGE_BYTES (10U * 1024U * 1024U)

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                          __LINE__, #expr);                                   \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

static const uint8_t MAGIC_LE[8] = {
    0x0cU, 0xb1U, 0xbaU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
static const uint8_t MAGIC_BE[8] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xbaU, 0xb1U, 0x0cU,
};

static void reset_image(int fd, off_t size)
{
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, size) == 0);
}

static void write_all(int fd, const void *buffer, size_t length, off_t offset)
{
    const uint8_t *bytes = buffer;
    size_t done = 0U;
    while (done < length) {
        const ssize_t count = pwrite(fd, bytes + done, length - done,
                                     offset + (off_t)done);
        CHECK(count > 0);
        done += (size_t)count;
    }
}

int main(void)
{
    char path[] = "/tmp/linux-defragger-zfs-test-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);

    LdZfsSummary summary;
    char error[256];

    reset_image(fd, SMALL_BYTES);
    write_all(fd, MAGIC_LE, sizeof(MAGIC_LE), 12345);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.size_bytes == SMALL_BYTES);
    CHECK(summary.uberblock_magic_offset == 12345U);
    CHECK(summary.byte_order == LD_ZFS_BYTE_ORDER_LITTLE);
    CHECK(strcmp(zfs_byte_order_name(&summary), "little") == 0);
    CHECK(zfs_probe(path));

    reset_image(fd, LARGE_BYTES);
    const off_t near_end = (off_t)LARGE_BYTES - 8192;
    write_all(fd, MAGIC_BE, sizeof(MAGIC_BE), near_end);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.size_bytes == LARGE_BYTES);
    CHECK(summary.uberblock_magic_offset == (uint64_t)near_end);
    CHECK(summary.byte_order == LD_ZFS_BYTE_ORDER_BIG);
    CHECK(strcmp(zfs_byte_order_name(&summary), "big") == 0);

    /* The first 4 MiB is authoritative for search order, matching the old backend. */
    write_all(fd, MAGIC_LE, sizeof(MAGIC_LE), 4096);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.uberblock_magic_offset == 4096U);
    CHECK(summary.byte_order == LD_ZFS_BYTE_ORDER_LITTLE);

    reset_image(fd, LARGE_BYTES);
    static const uint8_t junk[8] = {0xdeU, 0xadU, 0xbeU, 0xefU, 1U, 2U, 3U, 4U};
    write_all(fd, junk, sizeof(junk), 1000);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) != 0);
    CHECK(!zfs_probe(path));
    CHECK(strstr(error, "not a recognised ZFS member") != NULL);

    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
    (void)puts("ZFS native parser tests passed");
    return 0;
}
