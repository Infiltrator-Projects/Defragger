// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_IO_H
#define LD_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

ssize_t ld_pread_full(int fd, void *buffer, size_t length, uint64_t offset);
ssize_t ld_pwrite_full(int fd, const void *buffer, size_t length, uint64_t offset);
void ld_pread_exact(int fd, void *buffer, size_t length, uint64_t offset, const char *what);
void ld_pwrite_exact(int fd, const void *buffer, size_t length, uint64_t offset, const char *what);

uint64_t ld_available_memory_bytes(void);
size_t ld_default_ram_limit(void);
size_t ld_online_cpu_count(void);
size_t ld_default_worker_count(bool rotational, bool serial_flash);

#endif
