// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_STOP_H
#define LD_STOP_H

#include <stdbool.h>

/*
 * Process-wide cooperative Stop state.
 *
 * Signal handlers only publish the request flag. Filesystem engines decide
 * where it is safe to observe that flag; an authoritative write is never
 * abandoned at an arbitrary instruction boundary.
 */
void ld_stop_install_handlers(void);
bool ld_stop_requested(void);
void ld_stop_clear(void);

#endif
