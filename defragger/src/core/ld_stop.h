// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_STOP_H
#define LD_STOP_H

#include <stdbool.h>

void ld_stop_install_handlers(void);
bool ld_stop_requested(void);
void ld_stop_clear(void);

#endif
