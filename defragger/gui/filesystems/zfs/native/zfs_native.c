// SPDX-License-Identifier: GPL-3.0-or-later
#include "zfs_native.h"

#include "ld_io.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZFS_WINDOW_BYTES (4U * 1024U * 1024U)

static const uint8_t ZFS_MAGIC_LE[8] = {
    0x0cU, 0xb1U, 0xbaU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
static const uint8_t ZFS_MAGIC_BE[8] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xbaU, 0xb1U, 0x0cU,
};

static void zfs_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static int size_bytes_for_fd(int fd, const struct stat *status, uint64_t *size_bytes)
{
    if (S_ISREG(status->st_mode)) {
        if (status->st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        *size_bytes = (uint64_t)status->st_size;
        return 0;
    }
    if (S_ISBLK(status->st_mode)) {
        uint64_t bytes = 0U;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;
        *size_bytes = bytes;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static bool find_bytes(const uint8_t *data, size_t length,
                       const uint8_t magic[8], size_t *position)
{
    if (length < 8U)
        return false;
    for (size_t index = 0U; index <= length - 8U; ++index) {
        if (memcmp(data + index, magic, 8U) == 0) {
            *position = index;
            return true;
        }
    }
    return false;
}

static int scan_window(int fd, uint64_t offset, size_t length,
                       LdZfsSummary *summary)
{
    if (length < 8U)
        return 1;

    uint8_t *window = malloc(length);
    if (window == NULL)
        return -1;
    const ssize_t count = ld_pread_full(fd, window, length, offset);
    if (count < 0) {
        free(window);
        return -1;
    }

    const size_t actual = (size_t)count;
    size_t position = 0U;
    if (find_bytes(window, actual, ZFS_MAGIC_LE, &position)) {
        summary->uberblock_magic_offset = offset + (uint64_t)position;
        summary->byte_order = LD_ZFS_BYTE_ORDER_LITTLE;
        free(window);
        return 0;
    }
    if (find_bytes(window, actual, ZFS_MAGIC_BE, &position)) {
        summary->uberblock_magic_offset = offset + (uint64_t)position;
        summary->byte_order = LD_ZFS_BYTE_ORDER_BIG;
        free(window);
        return 0;
    }
    free(window);
    return 1;
}

int zfs_read_summary(const char *path, LdZfsSummary *summary,
                     char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        zfs_error(error, error_size, "invalid ZFS summary request");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    struct stat status;
    if (fstat(fd, &status) != 0 ||
        size_bytes_for_fd(fd, &status, &summary->size_bytes) != 0) {
        const int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        return -1;
    }

    const uint64_t first_length_u64 =
        summary->size_bytes < ZFS_WINDOW_BYTES ? summary->size_bytes : ZFS_WINDOW_BYTES;
    const size_t first_length = (size_t)first_length_u64;
    int result = scan_window(fd, 0U, first_length, summary);
    if (result == 1) {
        const uint64_t last_offset =
            summary->size_bytes > ZFS_WINDOW_BYTES
                ? summary->size_bytes - ZFS_WINDOW_BYTES
                : 0U;
        const uint64_t last_length_u64 =
            summary->size_bytes < ZFS_WINDOW_BYTES ? summary->size_bytes : ZFS_WINDOW_BYTES;
        result = scan_window(fd, last_offset, (size_t)last_length_u64, summary);
    }

    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;

    if (result < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "read: %s", strerror(errno));
        return -1;
    }
    if (result != 0) {
        zfs_error(error, error_size, "not a recognised ZFS member");
        return -1;
    }
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

bool zfs_probe(const char *path)
{
    LdZfsSummary summary;
    return zfs_read_summary(path, &summary, NULL, 0U) == 0;
}

const char *zfs_byte_order_name(const LdZfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return summary->byte_order == LD_ZFS_BYTE_ORDER_LITTLE ? "little" : "big";
}
