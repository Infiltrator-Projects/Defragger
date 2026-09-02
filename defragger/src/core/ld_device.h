// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_DEVICE_H
#define LD_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include <sys/types.h>

typedef struct {
    int fd;
    char *path;
    bool writable;
    bool is_block;
    uint64_t size_bytes;
    dev_t device_number;
    dev_t host_device;
    ino_t inode;
} LdDevice;

bool ld_device_number_is_mounted(dev_t device_number);
bool ld_path_is_mounted(const char *path);
int ld_device_try_open(const char *path, bool writable, LdDevice *device);
bool ld_device_matches_identity(const LdDevice *device,
                                const char *expected_identity,
                                uint64_t expected_size);
bool ld_fd_matches_identity(int fd, const char *expected_identity,
                            uint64_t expected_size);
int ld_device_open_verified_fd(const char *path, bool writable,
                               const char *expected_identity,
                               uint64_t expected_size);
LdDevice ld_device_open(const char *path, bool writable);
void ld_device_close(LdDevice *device);
bool ld_device_is_rotational(const LdDevice *device);
bool ld_device_is_serial_flash(const LdDevice *device);

#endif
