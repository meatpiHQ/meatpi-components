/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file lwip_guard.h
 * @brief The lwIP resolver guard (issue 1024, 2026-10-31).
 *
 * IDF's lwIP port gives every task that uses sockets or the resolver a
 * semaphore of its own, allocated from internal RAM on the task's first
 * call. When internal RAM is exhausted the allocation fails, the port's
 * getter returns NULL, and lwIP's validity check (a memcmp without a NULL
 * guard in 6.0.2 and 6.1) dereferences it: a LoadProhibited panic in ROM
 * memcmp at the first name lookup of any task, three in a row and the
 * crash-loop brake parks the device. This component wraps the getter at
 * link time (CMakeLists.txt): a NULL answer becomes a static zeroed
 * semaphore, which lwIP reads as "none", so the call fails with ERR_MEM
 * (getaddrinfo EAI_FAIL, socket operations ENOMEM) and the task lives.
 *
 * What it does not do: give the device its memory back. A refusal is a
 * genuine fault (one E line per 10 s with the task name and the internal
 * free bytes) and the cause must be found: the 2026-10-31 one was the
 * Tailscale DERP relay's TLS context leaking on every failed handshake.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The log descriptor. The wrap itself is link-time and needs no init. */
esp_err_t lwip_guard_init(void);

/** How many times a task was refused its resolver semaphore since boot. */
uint32_t lwip_guard_refusals(void);

#ifdef __cplusplus
}
#endif
