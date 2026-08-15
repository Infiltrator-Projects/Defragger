// SPDX-License-Identifier: GPL-3.0-or-later
#include "ufs_native.h"

#include "ld_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define UFS_WINDOW_BYTES 8192U
#define UFS_MIN_WINDOW 512U

static const uint64_t UFS_CANDIDATES[] = {8192U, 65536U, 262144U};

static const uint8_t UFS1_LE_MAGIC[4] = {0x54U, 0x19U, 0x01U, 0x00U};
static const uint8_t UFS1_BE_MAGIC[4] = {0x00U, 0x01U, 0x19U, 0x54U};
static const uint8_t UFS2_LE_MAGIC[4] = {0x19U, 0x01U, 0x54U, 0x19U};
static const uint8_t UFS2_BE_MAGIC[4] = {0x19U, 0x54U, 0x01U, 0x19U};

typedef struct {
    const uint8_t *magic;
    LdUfsVariant variant;
} UfsMagic;

static const UfsMagic UFS_MAGICS[] = {
    {UFS1_LE_MAGIC, LD_UFS_VARIANT_UFS1_LE},
    {UFS1_BE_MAGIC, LD_UFS_VARIANT_UFS1_BE},
    {UFS2_LE_MAGIC, LD_UFS_VARIANT_UFS2_LE},
    {UFS2_BE_MAGIC, LD_UFS_VARIANT_UFS2_BE},
};

static void ufs_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static bool find_magic(const uint8_t *data, size_t length, const uint8_t magic[4],
                       size_t *position)
{
    if (length < 4U)
        return false;
    for (size_t index = 0U; index <= length - 4U; ++index) {
        if (memcmp(data + index, magic, 4U) == 0) {
            *position = index;
            return true;
        }
    }
    return false;
}

int ufs_read_summary(const char *path, LdUfsSummary *summary,
                     char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        ufs_error(error, error_size, "invalid UFS summary request");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    uint8_t window[UFS_WINDOW_BYTES];
    int result = -1;
    for (size_t candidate_index = 0U;
         candidate_index < sizeof(UFS_CANDIDATES) / sizeof(UFS_CANDIDATES[0]);
         ++candidate_index) {
        const uint64_t offset = UFS_CANDIDATES[candidate_index];
        const ssize_t count = ld_pread_full(fd, window, sizeof(window), offset);
        if (count < 0) {
            if (errno == EINVAL || errno == EIO)
                continue;
            if (error != NULL && error_size != 0U)
                (void)snprintf(error, error_size, "read: %s", strerror(errno));
            (void)close(fd);
            return -1;
        }
        if ((size_t)count < UFS_MIN_WINDOW)
            continue;

        for (size_t magic_index = 0U;
             magic_index < sizeof(UFS_MAGICS) / sizeof(UFS_MAGICS[0]);
             ++magic_index) {
            size_t position = 0U;
            if (!find_magic(window, (size_t)count, UFS_MAGICS[magic_index].magic,
                            &position))
                continue;
            summary->variant = UFS_MAGICS[magic_index].variant;
            summary->superblock_offset = offset;
            summary->magic_offset = offset + (uint64_t)position;
            result = 0;
            break;
        }
        if (result == 0)
            break;
    }

    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;

    if (result != 0) {
        ufs_error(error, error_size, "not a recognised UFS volume");
        return -1;
    }
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

bool ufs_probe(const char *path)
{
    LdUfsSummary summary;
    return ufs_read_summary(path, &summary, NULL, 0U) == 0;
}

const char *ufs_variant_name(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    switch (summary->variant) {
    case LD_UFS_VARIANT_UFS1_LE:
        return "ufs1-le";
    case LD_UFS_VARIANT_UFS1_BE:
        return "ufs1-be";
    case LD_UFS_VARIANT_UFS2_LE:
        return "ufs2-le";
    case LD_UFS_VARIANT_UFS2_BE:
        return "ufs2-be";
    default:
        return "unknown";
    }
}

const char *ufs_byte_order_name(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return summary->variant == LD_UFS_VARIANT_UFS1_LE ||
                   summary->variant == LD_UFS_VARIANT_UFS2_LE
               ? "little"
               : "big";
}

unsigned int ufs_version(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return 0U;
    return summary->variant == LD_UFS_VARIANT_UFS1_LE ||
                   summary->variant == LD_UFS_VARIANT_UFS1_BE
               ? 1U
               : 2U;
}
