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
 * @file uds_transport_obd.c
 * @brief backend "obd_chip" — UDS over the MIC3624 via AT-hex.
 */
#include "uds_transport.h"

#include "obd_chip.h"

static esp_err_t obd_req(const char *cmd, char *resp, size_t resp_len,
                         uint32_t timeout_ms)
{
    return obd_chip_request(cmd, resp, resp_len,
                            pdMS_TO_TICKS(timeout_ms));
}

static esp_err_t obd_open(void)
{
    return ESP_OK; /* the MIC is always present + claim-arbitrated */
}

static esp_err_t obd_transceive(const uds_addr_t *addr,
                                const uint8_t *req, size_t req_len,
                                uint8_t *resp, size_t resp_cap,
                                size_t *resp_len, uint32_t p2_ms,
                                uint32_t p2star_ms, uint8_t *pending_out)
{
    return uds_at_transceive(obd_req, addr, req, req_len, resp, resp_cap,
                             resp_len, p2_ms, p2star_ms, pending_out);
}

const uds_transport_t *uds_transport_obd(void)
{
    static const uds_transport_t T =
    {
        .name       = "obd_chip",
        .open       = obd_open,
        .transceive = obd_transceive,
    };

    return &T;
}
