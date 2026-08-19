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
} LdDevice;

bool ld_device_number_is_mounted(dev_t device_number);
bool ld_path_is_mounted(const char *path);
LdDevice ld_device_open(const char *path, bool writable);
void ld_device_close(LdDevice *device);
bool ld_device_is_rotational(const LdDevice *device);
bool ld_device_is_serial_flash(const LdDevice *device);

#endif
