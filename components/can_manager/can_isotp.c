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
 * @file can_isotp.c
 * @brief The ISO-TP provider slot (see can_isotp.h). Registration is
 *        ext-init-phase only (single writer before any reader starts),
 *        so a plain pointer suffices.
 */
#include "can_isotp.h"

static const can_isotp_ops_t *s_ops;

esp_err_t can_isotp_provide(const can_isotp_ops_t *ops)
{
    if (ops == NULL || ops->open == NULL || ops->close == NULL ||
        ops->send == NULL || ops->recv == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_ops = ops;
    return ESP_OK;
}

const can_isotp_ops_t *can_isotp(void)
{
    return s_ops;
}
