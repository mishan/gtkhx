/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * hl_code — Hotline's symmetric XOR-with-0xff obfuscation used on
 * LOGIN / PASSWORD chunks (and the equivalent admin-edit forms in
 * usermod.c). The "encryption" is a single bitwise NOT per byte:
 * encode == decode == toggling every bit, which means a round-trip
 * is the identity.
 *
 * Lives in its own translation unit (rather than inside network.c)
 * specifically so the Tier 1 test can link it without dragging in
 * the GTK + GSocket + threads pile that network.c carries.
 */

#include "config.h"
#include <stddef.h>
#include <glib.h>
#include "hl_code.h"

void
hl_code (void *__dst, const void *__src, size_t len)
{
    guint8 *dst = (guint8 *)__dst, *src = (guint8 *)__src;

    for (; len; len--) {
        *dst++ = ~*src++;
    }
}
