// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define IMAGE_BYTES 4096U
#define SUPER_OFFSET 1024U

static void put16le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffU);
    p[1] = (uint8_t)(value >> 8);
}

static void put32le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffU);
    p[1] = (uint8_t)((value >> 8) & 0xffU);
    p[2] = (uint8_t)((value >> 16) & 0xffU);
    p[3] = (uint8_t)(value >> 24);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "Usage: make_minix_fixture IMAGE\n");
        return 2;
    }

    uint8_t image[IMAGE_BYTES];
    memset(image, 0, sizeof(image));
    uint8_t *sb = image + SUPER_OFFSET;
    put32le(sb + 0U, 300U);
    put16le(sb + 6U, 1U);
    put16le(sb + 8U, 2U);
    put16le(sb + 10U, 20U);
    put16le(sb + 12U, 1U);
    put32le(sb + 16U, 0x01000000U);
    put32le(sb + 20U, 12000U);
    put16le(sb + 24U, 0x4d5aU);
    put16le(sb + 28U, 4096U);

    const int fd = open(argv[1], O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    const ssize_t count = write(fd, image, sizeof(image));
    const int saved = count == (ssize_t)sizeof(image) ? 0 : 1;
    if (close(fd) != 0 || saved != 0) {
        (void)fprintf(stderr, "failed to write Minix fixture\n");
        return 1;
    }
    return 0;
}
