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
 * @file mqtt_manager_tls.c
 * @brief The mqtts:// verification material for the esp-mqtt client
 *        configuration: a cert_manager set (CA + optional client cert and
 *        key = mutual TLS), else a raw ca_file read into one PSRAM buffer,
 *        else the built-in certificate bundle. Split out of mqtt_manager.c
 *        on 2026-10-27 (the file passed 700 lines with the pre-restart
 *        hook); the behaviour is the 2026-07 one, unchanged.
 */
#include <stddef.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "cert_manager.h"
#include "filesystem.h"
#include "mqtt_manager_private.h"

static const char *TAG = "mqtt_manager";

#define MM_CA_PEM_MAX 8192 /* a raw ca_file, PEM, in PSRAM */

static char *s_ca_pem; /* PSRAM, loaded once at start when ca_file set */

/** mqtts:// verification, priority order: a cert_manager SET (CA +
 *  optional client cert/key = mutual TLS) > a raw ca_file > the built-in
 *  certificate bundle. */
void mm_configure_tls(esp_mqtt_client_config_t *cfg)
{
    const mm_config_t *mc = mm_settings_config();

    if (mc->cert_set[0] != '\0')
    {
        const char *pem;
        size_t len;

        if (cert_manager_get(mc->cert_set, CERT_MANAGER_CA, &pem, &len) ==
            ESP_OK)
        {
            cfg->broker.verification.certificate = pem;
            cfg->broker.verification.certificate_len = len;

            const char *ccert;
            const char *ckey;
            size_t ccert_len;
            size_t ckey_len;

            if (cert_manager_get(mc->cert_set, CERT_MANAGER_CLIENT_CERT,
                                 &ccert, &ccert_len) == ESP_OK &&
                cert_manager_get(mc->cert_set, CERT_MANAGER_CLIENT_KEY,
                                 &ckey, &ckey_len) == ESP_OK)
            {
                cfg->credentials.authentication.certificate = ccert;
                cfg->credentials.authentication.certificate_len =
                    ccert_len;
                cfg->credentials.authentication.key = ckey;
                cfg->credentials.authentication.key_len = ckey_len;
                ESP_LOGI(TAG, "mutual TLS via cert set '%s'",
                         mc->cert_set);
            }
            else
            {
                ESP_LOGI(TAG, "server-auth TLS via cert set '%s'",
                         mc->cert_set);
            }

            return;
        }

        ESP_LOGW(TAG, "cert_set '%s' unusable; falling back",
                 mc->cert_set);
    }

    if (mc->ca_file[0] != '\0')
    {
        size_t len = 0;

        if (s_ca_pem == NULL)
        {
            s_ca_pem = heap_caps_malloc(MM_CA_PEM_MAX, MALLOC_CAP_SPIRAM |
                                                       MALLOC_CAP_8BIT);
        }

        if (s_ca_pem != NULL &&
            filesystem_read(mc->ca_file, s_ca_pem, MM_CA_PEM_MAX - 1,
                            &len) == ESP_OK)
        {
            s_ca_pem[len] = '\0';
            cfg->broker.verification.certificate = s_ca_pem;
            cfg->broker.verification.certificate_len = len + 1;
            return;
        }

        ESP_LOGW(TAG, "ca_file '%s' unreadable; using the certificate "
                 "bundle", mc->ca_file);
    }

    cfg->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
}
