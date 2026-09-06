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
 * @file autopid_transport_obd.c
 * @brief The OBD transport: a passthrough to the MIC3624 via obd_chip.
 */
#include "autopid_transport.h"

#include "obd_chip.h"

static esp_err_t obd_claim_monitor(TickType_t timeout)
{
    return obd_chip_claim(OBD_CHIP_CLAIM_MONITOR, timeout);
}

static const ap_transport_t BE_OBD =
{
    .request       = obd_chip_request,
    .claim_monitor = obd_claim_monitor,
    .release       = obd_chip_release,
    .send          = obd_chip_send,
    .subscribe     = obd_chip_subscribe,
    .unsubscribe   = obd_chip_unsubscribe,
    .monitor_stop  = obd_chip_monitor_stop,
};

const ap_transport_t *ap_be(void)
{
    return &BE_OBD;
}
