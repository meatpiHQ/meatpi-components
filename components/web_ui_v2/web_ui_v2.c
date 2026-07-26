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
 * @file web_ui_v2.c
 * @brief Registers the embedded, gzipped v2 single-page app as an asset
 *        table. Hash-routed SPA, so the browser only ever requests "/" (and
 *        "/index.html"); both map to the one blob. API/WS routes always win
 *        because the catch-all is installed last (http_server_manager).
 */
#include "web_ui_v2.h"

#include "sdkconfig.h"

#if CONFIG_WICAN_WEBUI_V2

#include "esp_log.h"
#include "http_server_manager.h"

static const char *TAG = "web_ui_v2";

extern const uint8_t index_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_gz_end[]   asm("_binary_index_html_gz_end");

static const http_asset_t ASSETS[] =
{
    { .uri = "/", .content_type = "text/html",
      .data_start = index_gz_start, .data_end = index_gz_end,
      .content_encoding = "gzip" },
    { .uri = "/index.html", .content_type = "text/html",
      .data_start = index_gz_start, .data_end = index_gz_end,
      .content_encoding = "gzip" },
    { 0 }
};

esp_err_t web_ui_v2_register(void)
{
    esp_err_t err = http_server_manager_register_assets(ASSETS);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "v2 web UI registered (%u bytes gzipped)",
                 (unsigned)(index_gz_end - index_gz_start));
    }
    return err;
}

#else /* !CONFIG_WICAN_WEBUI_V2 */

esp_err_t web_ui_v2_register(void)
{
    return ESP_OK; /* another UI (or none) is selected */
}

#endif
