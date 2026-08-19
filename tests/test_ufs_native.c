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
#define UFS_DISK_STRUCT_BYTES 1376U
#define UFS_DISK_MAGIC_OFFSET 1372U

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

static void put_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *data, uint64_t value)
{
    for (unsigned int index = 0U; index < 8U; ++index)
        data[index] = (uint8_t)(value >> (index * 8U));
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
    CHECK(!summary.allocation_totals_known);
    CHECK(ufs_probe(path));
}

static void test_ufs2_recorded_allocation(int fd, const char *path)
{
    clear_image(fd);
    uint8_t superblock[UFS_DISK_STRUCT_BYTES];
    memset(superblock, 0, sizeof(superblock));

    put_le32(superblock + 48U, 8192U);
    put_le32(superblock + 52U, 1024U);
    put_le32(superblock + 56U, 8U);
    put_le64(superblock + 1016U, 100U);
    put_le64(superblock + 1032U, 3U);
    put_le64(superblock + 1080U, 1000U);
    put_le64(superblock + 1088U, 900U);
    static const uint8_t ufs2_le[4] = {0x19U, 0x01U, 0x54U, 0x19U};
    memcpy(superblock + UFS_DISK_MAGIC_OFFSET, ufs2_le, sizeof(ufs2_le));
    write_all(fd, superblock, sizeof(superblock), 65536);

    LdUfsSummary summary;
    char error[256];
    CHECK(ufs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.variant == LD_UFS_VARIANT_UFS2_LE);
    CHECK(summary.allocation_totals_known);
    CHECK(summary.block_size == 8192U);
    CHECK(summary.fragment_size == 1024U);
    CHECK(summary.fragments_per_block == 8U);
    CHECK(summary.data_fragments == 900U);
    CHECK(summary.free_blocks == 100U);
    CHECK(summary.free_fragments == 3U);
    CHECK(ufs_recorded_data_bytes(&summary) == UINT64_C(921600));
    CHECK(ufs_recorded_free_bytes(&summary) == UINT64_C(822272));
    CHECK(ufs_recorded_used_bytes(&summary) == UINT64_C(99328));
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

    test_ufs2_recorded_allocation(fd, path);

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
    (void)puts("UFS native parser and recorded-allocation tests passed");
    return 0;
}
