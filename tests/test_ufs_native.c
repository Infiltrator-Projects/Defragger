// SPDX-License-Identifier: GPL-3.0-or-later
#include "ufs_native.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE_BYTES 300000

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                          __LINE__, #expr);                                   \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

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

static void clear_image(int fd)
{
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, IMAGE_BYTES) == 0);
}

static void check_variant(int fd, const char *path, const uint8_t magic[4],
                          uint64_t candidate, uint64_t position,
                          LdUfsVariant expected, const char *name,
                          const char *byte_order, unsigned int version)
{
    clear_image(fd);
    write_all(fd, magic, 4U, (off_t)(candidate + position));

    LdUfsSummary summary;
    char error[256];
    CHECK(ufs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.variant == expected);
    CHECK(summary.superblock_offset == candidate);
    CHECK(summary.magic_offset == candidate + position);
    CHECK(strcmp(ufs_variant_name(&summary), name) == 0);
    CHECK(strcmp(ufs_byte_order_name(&summary), byte_order) == 0);
    CHECK(ufs_version(&summary) == version);
    CHECK(ufs_probe(path));
}

int main(void)
{
    char path[] = "/tmp/linux-defragger-ufs-test-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);

    static const uint8_t ufs1_le[4] = {0x54U, 0x19U, 0x01U, 0x00U};
    static const uint8_t ufs1_be[4] = {0x00U, 0x01U, 0x19U, 0x54U};
    static const uint8_t ufs2_le[4] = {0x19U, 0x01U, 0x54U, 0x19U};
    static const uint8_t ufs2_be[4] = {0x19U, 0x54U, 0x01U, 0x19U};

    check_variant(fd, path, ufs1_le, 8192U, 64U, LD_UFS_VARIANT_UFS1_LE,
                  "ufs1-le", "little", 1U);
    check_variant(fd, path, ufs1_be, 65536U, 128U, LD_UFS_VARIANT_UFS1_BE,
                  "ufs1-be", "big", 1U);
    check_variant(fd, path, ufs2_le, 262144U, 256U, LD_UFS_VARIANT_UFS2_LE,
                  "ufs2-le", "little", 2U);
    check_variant(fd, path, ufs2_be, 8192U, 4096U, LD_UFS_VARIANT_UFS2_BE,
                  "ufs2-be", "big", 2U);

    clear_image(fd);
    static const uint8_t junk[4] = {0xdeU, 0xadU, 0xbeU, 0xefU};
    write_all(fd, junk, sizeof(junk), 8192 + 100);
    LdUfsSummary summary;
    char error[256];
    CHECK(ufs_read_summary(path, &summary, error, sizeof(error)) != 0);
    CHECK(!ufs_probe(path));
    CHECK(strstr(error, "not a recognised UFS volume") != NULL);

    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
    (void)puts("UFS native parser tests passed");
    return 0;
}
