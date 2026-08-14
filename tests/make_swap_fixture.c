// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SIZE_BYTES 4096U
#define PHYSICAL_PAGES 16U
#define LAST_PAGE 11U

static void put_u32le(unsigned char *buffer, size_t offset, uint32_t value)
{
    buffer[offset] = (unsigned char)(value & 0xffU);
    buffer[offset + 1U] = (unsigned char)((value >> 8) & 0xffU);
    buffer[offset + 2U] = (unsigned char)((value >> 16) & 0xffU);
    buffer[offset + 3U] = (unsigned char)((value >> 24) & 0xffU);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "Usage: %s IMAGE\n", argv[0]);
        return 2;
    }
    const int fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        (void)fprintf(stderr, "open: %s\n", strerror(errno));
        return 1;
    }
    const uint64_t bytes = (uint64_t)PAGE_SIZE_BYTES * PHYSICAL_PAGES;
    if (ftruncate(fd, (off_t)bytes) != 0) {
        (void)fprintf(stderr, "ftruncate: %s\n", strerror(errno));
        (void)close(fd);
        return 1;
    }
    unsigned char page[PAGE_SIZE_BYTES];
    memset(page, 0, sizeof(page));
    put_u32le(page, 1024U, 1U);
    put_u32le(page, 1028U, LAST_PAGE);
    put_u32le(page, 1032U, 2U);
    for (size_t index = 0U; index < 16U; ++index)
        page[1036U + index] = (unsigned char)(index + 1U);
    memcpy(page + 1052U, "Native Swap", 11U);
    put_u32le(page, 1536U, 3U);
    put_u32le(page, 1540U, 7U);
    memcpy(page + PAGE_SIZE_BYTES - 10U, "SWAPSPACE2", 10U);
    const ssize_t written = pwrite(fd, page, sizeof(page), 0);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    if (written != (ssize_t)sizeof(page)) {
        (void)fprintf(stderr, "write: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
