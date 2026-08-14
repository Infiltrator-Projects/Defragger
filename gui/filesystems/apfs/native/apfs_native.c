// SPDX-License-Identifier: GPL-3.0-or-later
#include "apfs_native.h"

#include "ld_io.h"
#include "ld_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define APFS_NX_BLOCK_BYTES 4096U
#define APFS_MAGIC_OFFSET 32U
#define APFS_BLOCK_SIZE_OFFSET 36U
#define APFS_BLOCK_COUNT_OFFSET 40U
#define APFS_UUID_OFFSET 72U

static uint64_t apfs_read_le64(const uint8_t *p)
{
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static void apfs_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

int apfs_read_summary(const char *path, ApfsSummary *summary,
                      char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        apfs_error(error, error_size, "invalid APFS summary request");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    uint8_t block[APFS_NX_BLOCK_BYTES];
    const ssize_t count = ld_pread_full(fd, block, sizeof(block), 0U);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;

    if (count < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "read: %s", strerror(errno));
        return -1;
    }
    if ((size_t)count != sizeof(block)) {
        apfs_error(error, error_size, "APFS container is shorter than one NX superblock");
        return -1;
    }
    if (memcmp(block + APFS_MAGIC_OFFSET, "NXSB", 4U) != 0) {
        apfs_error(error, error_size, "not an APFS container");
        return -1;
    }

    const uint32_t block_size = ld_read_le32(block + APFS_BLOCK_SIZE_OFFSET);
    const uint64_t block_count = apfs_read_le64(block + APFS_BLOCK_COUNT_OFFSET);
    if (block_size < APFS_NX_BLOCK_BYTES ||
        (block_size & (block_size - 1U)) != 0U || block_count == 0U) {
        apfs_error(error, error_size, "invalid APFS container geometry");
        return -1;
    }

    summary->block_size = block_size;
    summary->block_count = block_count;
    memcpy(summary->container_uuid, block + APFS_UUID_OFFSET,
           sizeof(summary->container_uuid));
    if (error != NULL && error_size != 0U) error[0] = '\0';
    return 0;
}

bool apfs_probe(const char *path)
{
    ApfsSummary summary;
    return apfs_read_summary(path, &summary, NULL, 0U) == 0;
}
