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
 * @file ble_manager_settings.c
 * @brief settings_manager descriptor for ble_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_apply (parses into
 *        the boot-applied config) and the v2/v3 migration hook.
 */
#include "esp_attr.h"

#include "settings_manager.h"

#include "ble_manager_private.h"

/* enabled=false by default: BLE is opt-in (it shares the radio with WiFi;
 * coexistence policy is the composition root's business — README). */
static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_INT("passkey", 0, 999999, 123456),
    SETTINGS_INT("tx_power_dbm", -12, 9, 9),
    SETTINGS_BOOL("pairing_at_boot", true),
    /* v2: bonding = keep long-term keys so a paired phone reconnects
       without re-entering the passkey. Disable for a stricter posture:
       the link is still encrypted+MITM-protected per session, but no
       long-term key is exchanged/kept, so every reconnect re-pairs and
       nothing reusable is left on the device (meatpi 2026-07-08). */
    SETTINGS_BOOL("bonding", true),
    /* v3: sc_only = require LE Secure Connections. Default true refuses a
       peer that tries to downgrade to weak LEGACY pairing (whose static-
       passkey exchange is brute-forceable) — closes the downgrade attack
       at a negligible interop cost (every phone since ~2014 does SC).
       Disable only for a genuinely pre-4.2 accessory (meatpi 2026-07-09). */
    SETTINGS_BOOL("sc_only", true),
    /* the connection window the device REQUESTS on connect: iOS-friendly
       legacy default, or short-interval max performance (user's choice) */
    SETTINGS_STR_ENUM("conn_profile", "ios,android_fast", "ios"),
    /* v4 (2026-09-21): BLE 5 as a user choice. `phy` = the PHY the device
       prefers on a link (requested after connect; the central decides, a
       4.2 phone keeps 1M): 1m = today's behaviour, 2m = double the on-air
       bit rate, coded = long range, auto = 1M or 2M. `advertising` = which
       advertising set(s) run: legacy = the 4.2-visible set every scanner
       sees, extended = the 5.0 set only (invisible to 4.2 phones), both. */
    SETTINGS_STR_ENUM("phy", "1m,2m,coded,auto", "1m"),
    SETTINGS_STR_ENUM("advertising", "legacy,extended,both", "legacy"),
};

static blm_config_t s_cfg EXT_RAM_BSS_ATTR;
static bool s_configured;

const blm_config_t *blm_core_config(void)
{
    return &s_cfg;
}

bool blm_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *v;

    s_cfg.enabled =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    s_cfg.pairing_at_boot = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "pairing_at_boot"));
    s_cfg.bonding = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "bonding"));
    s_cfg.sc_only = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "sc_only"));
    v = cJSON_GetObjectItemCaseSensitive(settings, "passkey");
    s_cfg.passkey = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 123456;
    v = cJSON_GetObjectItemCaseSensitive(settings, "tx_power_dbm");
    s_cfg.tx_power_dbm =
        blm_ident_clamp_tx_power(cJSON_IsNumber(v) ? (int)v->valuedouble : 9);
    v = cJSON_GetObjectItemCaseSensitive(settings, "conn_profile");
    blm_ident_conn_window(cJSON_IsString(v) ? v->valuestring : NULL,
                          &s_cfg.conn_min_units, &s_cfg.conn_max_units);
    v = cJSON_GetObjectItemCaseSensitive(settings, "phy");
    s_cfg.phy_mask = blm_ident_phy_mask(cJSON_IsString(v) ? v->valuestring : NULL);
    v = cJSON_GetObjectItemCaseSensitive(settings, "advertising");
    s_cfg.adv_mode = blm_ident_adv_mode(cJSON_IsString(v) ? v->valuestring : NULL);
    s_configured = true;
    return ESP_OK;
}

/* v1 -> v2 added the `bonding` flag, v3 `sc_only`, v4 `phy` +
   `advertising`; every one is covered by its fill-missing default (the
   historic behaviour), so nothing to do here. The hook must exist though:
   without it a version bump discards ALL stored settings and resets to
   defaults (settings_manager_boot §2). */
static esp_err_t on_migrate(uint32_t from_version, cJSON *settings)
{
    (void)from_version;
    (void)settings;
    return ESP_OK;
}

esp_err_t blm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "ble_manager",
        .version = 4, /* v4: +phy, +advertising (BLE 5 as a choice, 4.2
                         defaults); v3: +sc_only; v2: +bonding. fill-missing
                         defaults cover all, so on_migrate stays a no-op */
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_migrate = on_migrate,
    };

    return settings_manager_register(&DESC);
}
