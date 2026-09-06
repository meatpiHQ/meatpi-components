/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file elm327_err.h
 * @brief Shared return codes of the CAN-core stack (can_core and the
 *        ISO-TP layer that builds on it).
 *
 * Lives in can_manager because that is the root of the stack's
 * dependency graph. The historical ELM327_* names are kept so the whole
 * stack keeps one error currency across in-tree and add-on layers.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    ELM327_OK               =  0,
    ELM327_ERR_INVALID_ARG  = -1,
    ELM327_ERR_NO_MEM       = -2,
    ELM327_ERR_NVS          = -3,
    ELM327_ERR_CAN          = -4,
    ELM327_ERR_TIMEOUT      = -5,
    ELM327_ERR_NOT_INIT     = -6,
    ELM327_ERR_OVERFLOW     = -7,
    ELM327_ERR_BUSY         = -8
} elm327_err_t;

#ifdef __cplusplus
}
#endif
