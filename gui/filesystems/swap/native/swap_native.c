// SPDX-License-Identifier: GPL-3.0-or-later
#include "swap_native.h"

#include "ld_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define LD_SWAP_SIGNATURE_BYTES 10U
#define LD_SWAP_BOOT_BYTES 1024U
#define LD_SWAP_HEADER_FIXED_BYTES 1536U
#define LD_SWAP_TUXONICE_BYTES 8U

static const uint32_t ld_swap_page_sizes[] = {
    4096U, 8192U, 16384U, 32768U, 65536U,
};
static const unsigned char ld_swap_tuxonice_magic[LD_SWAP_TUXONICE_BYTES] = {
    0xedU, 0xc3U, 0x02U, 0xe9U, 0x98U, 0x56U, 0xe5U, 0x0cU,
};

static void ld_swap_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static void ld_swap_errno_error(char *error, size_t error_size,
                                const char *operation)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s: %s", operation, strerror(errno));
}

static uint32_t ld_swap_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t ld_swap_u32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t ld_swap_u32(const unsigned char *p, bool little_endian)
{
    return little_endian ? ld_swap_u32le(p) : ld_swap_u32be(p);
}

static int ld_swap_u32_compare(const void *left, const void *right)
{
    const uint32_t a = *(const uint32_t *)left;
    const uint32_t b = *(const uint32_t *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int ld_swap_device_size(const char *path, uint64_t *size_bytes,
                               bool *regular_file,
                               char *error, size_t error_size)
{
    struct stat status;
    if (stat(path, &status) != 0) {
        ld_swap_errno_error(error, error_size, "stat");
        return -1;
    }
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            ld_swap_error(error, error_size, "negative swap image size");
            return -1;
        }
        *size_bytes = (uint64_t)status.st_size;
        *regular_file = true;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        ld_swap_error(error, error_size,
                      "target must be a block device or regular swap image");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ld_swap_errno_error(error, error_size, "open");
        return -1;
    }
    uint64_t bytes = 0U;
    const int result = ioctl(fd, BLKGETSIZE64, &bytes);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    if (result != 0) {
        ld_swap_errno_error(error, error_size, "BLKGETSIZE64");
        return -1;
    }
    *size_bytes = bytes;
    *regular_file = false;
    return 0;
}

static bool ld_swap_uuid_nonzero(const unsigned char *uuid)
{
    for (size_t index = 0U; index < LD_SWAP_UUID_BYTES; ++index) {
        if (uuid[index] != 0U)
            return true;
    }
    return false;
}

static int ld_swap_parse_current(const unsigned char *page, uint32_t page_size,
                                 uint64_t container_bytes, bool regular_file,
                                 LdSwapSummary *summary,
                                 char *error, size_t error_size)
{
    const uint32_t version_le = ld_swap_u32le(page + LD_SWAP_BOOT_BYTES);
    const uint32_t version_be = ld_swap_u32be(page + LD_SWAP_BOOT_BYTES);
    bool little_endian = true;
    if (version_le == 1U) {
        little_endian = true;
    } else if (version_be == 1U) {
        little_endian = false;
    } else {
        ld_swap_error(error, error_size, "unsupported Linux swap header version");
        return -1;
    }

    const uint32_t last_page = ld_swap_u32(page + 1028U, little_endian);
    const uint32_t bad_page_count = ld_swap_u32(page + 1032U, little_endian);
    if (last_page == 0U) {
        ld_swap_error(error, error_size, "empty Linux swap area");
        return -1;
    }
    if (page_size <= LD_SWAP_HEADER_FIXED_BYTES + LD_SWAP_SIGNATURE_BYTES) {
        ld_swap_error(error, error_size, "Linux swap page size is too small");
        return -1;
    }
    const uint32_t max_bad_pages =
        (page_size - LD_SWAP_HEADER_FIXED_BYTES - LD_SWAP_SIGNATURE_BYTES) / 4U;
    if (bad_page_count > max_bad_pages) {
        ld_swap_error(error, error_size, "Linux swap bad-page count exceeds header capacity");
        return -1;
    }

    const uint64_t filesystem_pages = (uint64_t)last_page + 1U;
    if (filesystem_pages > UINT64_MAX / (uint64_t)page_size) {
        ld_swap_error(error, error_size, "Linux swap extent overflows address space");
        return -1;
    }
    const uint64_t filesystem_bytes = filesystem_pages * (uint64_t)page_size;
    if (filesystem_bytes > container_bytes) {
        ld_swap_error(error, error_size,
                      "Linux swap area is shorter than its header declares");
        return -1;
    }

    uint32_t *bad_pages = NULL;
    if (bad_page_count != 0U) {
        bad_pages = calloc((size_t)bad_page_count, sizeof(*bad_pages));
        if (bad_pages == NULL) {
            ld_swap_errno_error(error, error_size, "calloc");
            return -1;
        }
        for (uint32_t index = 0U; index < bad_page_count; ++index) {
            const uint32_t page_number =
                ld_swap_u32(page + LD_SWAP_HEADER_FIXED_BYTES + (size_t)index * 4U,
                            little_endian);
            if (page_number == 0U || page_number > last_page) {
                free(bad_pages);
                ld_swap_error(error, error_size,
                              "Linux swap bad-page offset lies outside the declared area");
                return -1;
            }
            bad_pages[index] = page_number;
        }
        qsort(bad_pages, (size_t)bad_page_count, sizeof(*bad_pages),
              ld_swap_u32_compare);
        for (uint32_t index = 1U; index < bad_page_count; ++index) {
            if (bad_pages[index] == bad_pages[index - 1U]) {
                free(bad_pages);
                ld_swap_error(error, error_size,
                              "Linux swap header contains duplicate bad-page offsets");
                return -1;
            }
        }
    }

    summary->format = LD_SWAP_FORMAT_CURRENT;
    summary->page_size = page_size;
    summary->little_endian = little_endian;
    summary->version = 1U;
    summary->last_page = last_page;
    summary->filesystem_pages = filesystem_pages;
    summary->bad_page_count = bad_page_count;
    summary->bad_pages = bad_pages;
    summary->usable_pages = filesystem_pages - 1U - (uint64_t)bad_page_count;
    summary->regular_file = regular_file;
    summary->container_bytes = container_bytes;
    summary->filesystem_bytes = filesystem_bytes;
    summary->outside_bytes = container_bytes - filesystem_bytes;

    memcpy(summary->uuid, page + 1036U, LD_SWAP_UUID_BYTES);
    summary->uuid_present = ld_swap_uuid_nonzero(summary->uuid);
    memcpy(summary->label, page + 1052U, LD_SWAP_LABEL_BYTES);
    summary->label_length = 0U;
    while (summary->label_length < LD_SWAP_LABEL_BYTES &&
           summary->label[summary->label_length] != 0U)
        summary->label_length++;

    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

static int ld_swap_parse_legacy(uint32_t page_size, uint64_t container_bytes,
                                bool regular_file, LdSwapSummary *summary,
                                char *error, size_t error_size)
{
    uint64_t container_pages = container_bytes / (uint64_t)page_size;
    if (container_bytes % (uint64_t)page_size != 0U)
        container_pages++;
    const uint64_t legacy_capacity_pages = (uint64_t)page_size * 8U;
    const uint64_t possible_pages =
        container_pages < legacy_capacity_pages ? container_pages : legacy_capacity_pages;
    if (possible_pages == 0U) {
        ld_swap_error(error, error_size, "empty legacy Linux swap area");
        return -1;
    }

    summary->format = LD_SWAP_FORMAT_LEGACY;
    summary->page_size = page_size;
    summary->little_endian = true;
    summary->version = 0U;
    summary->last_page = 0U;
    summary->filesystem_pages = possible_pages;
    summary->usable_pages = 0U;
    summary->bad_page_count = 0U;
    summary->regular_file = regular_file;
    summary->container_bytes = container_bytes;
    summary->filesystem_bytes = possible_pages * (uint64_t)page_size;
    if (summary->filesystem_bytes > container_bytes)
        summary->filesystem_bytes = container_bytes;
    summary->outside_bytes = container_bytes - summary->filesystem_bytes;

    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

int ld_swap_read_summary(const char *path, LdSwapSummary *summary,
                         char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        ld_swap_error(error, error_size, "invalid swap summary request");
        return -1;
    }
    memset(summary, 0, sizeof(*summary));

    uint64_t container_bytes = 0U;
    bool regular_file = false;
    if (ld_swap_device_size(path, &container_bytes, &regular_file,
                            error, error_size) != 0)
        return -1;

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ld_swap_errno_error(error, error_size, "open");
        return -1;
    }

    unsigned char first_bytes[LD_SWAP_TUXONICE_BYTES];
    const ssize_t first_count = ld_pread_full(fd, first_bytes, sizeof(first_bytes), 0);
    if (first_count == (ssize_t)sizeof(first_bytes) &&
        memcmp(first_bytes, ld_swap_tuxonice_magic, sizeof(first_bytes)) == 0) {
        (void)close(fd);
        ld_swap_error(error, error_size,
                      "TuxOnIce image preserves a swap signature but is not an active swap header");
        return -1;
    }

    int result = -1;
    bool found_signature = false;
    for (size_t index = 0U;
         index < sizeof(ld_swap_page_sizes) / sizeof(ld_swap_page_sizes[0]);
         ++index) {
        const uint32_t page_size = ld_swap_page_sizes[index];
        if ((uint64_t)page_size > container_bytes)
            continue;
        unsigned char signature[LD_SWAP_SIGNATURE_BYTES];
        const uint64_t signature_offset =
            (uint64_t)page_size - LD_SWAP_SIGNATURE_BYTES;
        const ssize_t count = ld_pread_full(fd, signature, sizeof(signature),
                                            signature_offset);
        if (count != (ssize_t)sizeof(signature))
            continue;

        const bool current = memcmp(signature, "SWAPSPACE2", 10U) == 0;
        const bool legacy = memcmp(signature, "SWAP-SPACE", 10U) == 0;
        if (!current && !legacy)
            continue;
        found_signature = true;

        unsigned char *page = malloc(page_size);
        if (page == NULL) {
            ld_swap_errno_error(error, error_size, "malloc");
            break;
        }
        const ssize_t page_count = ld_pread_full(fd, page, page_size, 0);
        if (page_count != (ssize_t)page_size) {
            free(page);
            ld_swap_error(error, error_size, "short read of Linux swap header page");
            break;
        }
        if (current) {
            result = ld_swap_parse_current(page, page_size, container_bytes,
                                           regular_file, summary,
                                           error, error_size);
        } else {
            result = ld_swap_parse_legacy(page_size, container_bytes,
                                          regular_file, summary,
                                          error, error_size);
        }
        free(page);
        break;
    }

    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    if (!found_signature && result != 0)
        ld_swap_error(error, error_size, "not a recognised Linux swap area");
    return result;
}

void ld_swap_summary_destroy(LdSwapSummary *summary)
{
    if (summary == NULL)
        return;
    free(summary->bad_pages);
    memset(summary, 0, sizeof(*summary));
}

bool ld_swap_probe(const char *path)
{
    LdSwapSummary summary;
    const int result = ld_swap_read_summary(path, &summary, NULL, 0U);
    if (result == 0)
        ld_swap_summary_destroy(&summary);
    return result == 0;
}

const char *ld_swap_format_name(const LdSwapSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return summary->format == LD_SWAP_FORMAT_CURRENT ? "linux-v1" : "linux-v0-legacy";
}

const char *ld_swap_byte_order_name(const LdSwapSummary *summary)
{
    if (summary == NULL || summary->format != LD_SWAP_FORMAT_CURRENT)
        return "unknown";
    return summary->little_endian ? "little" : "big";
}

void ld_swap_uuid_string(const LdSwapSummary *summary, char output[37])
{
    if (output == NULL)
        return;
    output[0] = '\0';
    if (summary == NULL || !summary->uuid_present)
        return;
    (void)snprintf(output, 37,
                   "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                   summary->uuid[0], summary->uuid[1], summary->uuid[2], summary->uuid[3],
                   summary->uuid[4], summary->uuid[5], summary->uuid[6], summary->uuid[7],
                   summary->uuid[8], summary->uuid[9], summary->uuid[10], summary->uuid[11],
                   summary->uuid[12], summary->uuid[13], summary->uuid[14], summary->uuid[15]);
}

static size_t ld_swap_lower_bound(const uint32_t *values, size_t count,
                                  uint64_t needle)
{
    size_t low = 0U;
    size_t high = count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if ((uint64_t)values[middle] < needle)
            low = middle + 1U;
        else
            high = middle;
    }
    return low;
}

uint64_t ld_swap_bad_pages_in_range(const LdSwapSummary *summary,
                                    uint64_t start_page,
                                    uint64_t end_page)
{
    if (summary == NULL || summary->bad_pages == NULL ||
        end_page <= start_page)
        return 0U;
    const size_t count = (size_t)summary->bad_page_count;
    const size_t first = ld_swap_lower_bound(summary->bad_pages, count, start_page);
    const size_t last = ld_swap_lower_bound(summary->bad_pages, count, end_page);
    return (uint64_t)(last - first);
}

static void ld_swap_decode_proc_path(const char *source, char *target,
                                     size_t target_size)
{
    size_t in = 0U;
    size_t out = 0U;
    if (target_size == 0U)
        return;
    while (source[in] != '\0' && out + 1U < target_size) {
        if (source[in] == '\\' && source[in + 1U] >= '0' && source[in + 1U] <= '7' &&
            source[in + 2U] >= '0' && source[in + 2U] <= '7' &&
            source[in + 3U] >= '0' && source[in + 3U] <= '7') {
            const unsigned int value =
                (unsigned int)(source[in + 1U] - '0') * 64U +
                (unsigned int)(source[in + 2U] - '0') * 8U +
                (unsigned int)(source[in + 3U] - '0');
            target[out++] = (char)value;
            in += 4U;
        } else {
            target[out++] = source[in++];
        }
    }
    target[out] = '\0';
}

static void ld_swap_resolve_path(const char *path, char *resolved,
                                 size_t resolved_size)
{
    char real[PATH_MAX];
    if (realpath(path, real) != NULL) {
        (void)snprintf(resolved, resolved_size, "%s", real);
        return;
    }
    (void)snprintf(resolved, resolved_size, "%s", path);
}

int ld_swap_runtime_usage(const char *path, const char *proc_swaps_path,
                          LdSwapRuntimeUsage *usage,
                          char *error, size_t error_size)
{
    if (path == NULL || usage == NULL) {
        ld_swap_error(error, error_size, "invalid swap runtime-usage request");
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    usage->priority = 0;
    if (proc_swaps_path == NULL)
        proc_swaps_path = "/proc/swaps";

    FILE *file = fopen(proc_swaps_path, "r");
    if (file == NULL) {
        if (error != NULL && error_size != 0U)
            error[0] = '\0';
        return 0;
    }

    char wanted[PATH_MAX];
    ld_swap_resolve_path(path, wanted, sizeof(wanted));
    char *line = NULL;
    size_t capacity = 0U;
    (void)getline(&line, &capacity, file);
    int found = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char encoded_path[PATH_MAX];
        char type[LD_SWAP_PROC_TYPE_BYTES];
        unsigned long long total_kib = 0ULL;
        unsigned long long used_kib = 0ULL;
        int priority = 0;
        if (sscanf(line, "%4095s %31s %llu %llu %d",
                   encoded_path, type, &total_kib, &used_kib, &priority) != 5)
            continue;

        char decoded_path[PATH_MAX];
        char candidate[PATH_MAX];
        ld_swap_decode_proc_path(encoded_path, decoded_path, sizeof(decoded_path));
        ld_swap_resolve_path(decoded_path, candidate, sizeof(candidate));
        if (strcmp(candidate, wanted) != 0)
            continue;
        if (total_kib > UINT64_MAX / 1024ULL || used_kib > UINT64_MAX / 1024ULL) {
            free(line);
            (void)fclose(file);
            ld_swap_error(error, error_size, "swap runtime usage overflows byte counter");
            return -1;
        }
        usage->active = true;
        usage->total_bytes = (uint64_t)total_kib * 1024U;
        usage->used_bytes = (uint64_t)used_kib * 1024U;
        if (usage->used_bytes > usage->total_bytes)
            usage->used_bytes = usage->total_bytes;
        usage->priority = priority;
        (void)snprintf(usage->type, sizeof(usage->type), "%s", type);
        found = 1;
        break;
    }
    free(line);
    (void)fclose(file);
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return found;
}
