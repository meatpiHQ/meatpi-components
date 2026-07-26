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
 * @file bench_main.c
 * @brief The flagship data-path bench: the FULL production composition —
 *        wifi_manager (STA to the rpi001 bench AP) + socket_manager +
 *        bridge_manager + obd_chip — with two configured bridges:
 *
 *          br_obd:  obd  <-raw-> obd0  (TCP:35000)  — the real OBD chip,
 *                   reachable from the LAN exactly like production
 *          br_echo: echo <-raw-> echo0 (TCP:3334)   — RF benchmark loop
 *                   (latency / throughput measured by socket_bench.py)
 *
 * Endpoint glue lives HERE (composition root), per the ownership rule:
 * obd_chip and socket_manager never depend on bridge_manager.
 *
 * PC side: tools/testbench/bridge_wifi_bench.py (AP up via rpi001, waits
 * for "BENCH READY ip=", drives ATI/0100 through TCP:35000 against the
 * live ECU simulator, runs the socket_bench scenarios against :3334).
 */
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bridge_manager.h"
#include "dev_status_manager.h"
#include "log_manager.h"
#include "obd_chip.h"
#include "settings_manager.h"
#include "socket_manager.h"
#include "wifi_manager.h"

static const char *TAG = "bridge_bench";

/* Bench AP credentials come from the build environment (see
 * main/CMakeLists.txt: WICAN_BENCH_SSID / WICAN_BENCH_PSK). The
 * placeholder default will NOT associate with the real bench AP. */
#ifndef BENCH_STA_SSID
#define BENCH_STA_SSID "WICAN_TEST_AP"
#endif
#ifndef BENCH_STA_PASS
#define BENCH_STA_PASS "changeme"
#endif

_Static_assert(sizeof(bridge_chunk_t) == sizeof(obd_chunk_t),
               "chunk conventions diverged");
_Static_assert(sizeof(bridge_chunk_t) == sizeof(socket_chunk_t),
               "chunk conventions diverged");

/* ---- endpoint glue (three-liners, as promised) ------------------------------ */

static esp_err_t obd_ep_send(const uint8_t *d, size_t l)
{
    return obd_chip_send(d, l);
}

static esp_err_t obd_ep_subscribe(QueueHandle_t q)
{
    return obd_chip_subscribe(q, "bridge");
}

static esp_err_t obd_ep_unsubscribe(QueueHandle_t q)
{
    return obd_chip_unsubscribe(q);
}

static esp_err_t obd0_send(const uint8_t *d, size_t l)
{
    return socket_manager_send("obd0", d, l);
}

static esp_err_t obd0_subscribe(QueueHandle_t q)
{
    return socket_manager_subscribe("obd0", q);
}

static esp_err_t obd0_unsubscribe(QueueHandle_t q)
{
    return socket_manager_unsubscribe("obd0", q);
}

static esp_err_t echo0_send(const uint8_t *d, size_t l)
{
    return socket_manager_send("echo0", d, l);
}

static esp_err_t echo0_subscribe(QueueHandle_t q)
{
    return socket_manager_subscribe("echo0", q);
}

static esp_err_t echo0_unsubscribe(QueueHandle_t q)
{
    return socket_manager_unsubscribe("echo0", q);
}

/* echo endpoint: send() loops data straight back via its subscriber queue */
static QueueHandle_t s_echo_sub;

static esp_err_t echo_send(const uint8_t *d, size_t l)
{
    if (s_echo_sub == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bridge_chunk_t chunk;

    while (l > 0)
    {
        size_t n = (l > BRIDGE_MANAGER_CHUNK_SIZE) ? BRIDGE_MANAGER_CHUNK_SIZE
                                                   : l;

        chunk.len = (uint16_t)n;
        memcpy(chunk.data, d, n);

        if (xQueueSend(s_echo_sub, &chunk, 0) != pdTRUE)
        {
            return ESP_FAIL; /* bounded: drop under overload */
        }

        d += n;
        l -= n;
    }

    return ESP_OK;
}

static esp_err_t echo_subscribe(QueueHandle_t q)
{
    s_echo_sub = q;
    return ESP_OK;
}

static esp_err_t echo_unsubscribe(QueueHandle_t q)
{
    (void)q;
    s_echo_sub = NULL;
    return ESP_OK;
}

/* ---- app --------------------------------------------------------------------- */

static void persist(const char *comp, const char *json)
{
    cJSON *obj = cJSON_Parse(json);
    char errbuf[96] = "";
    esp_err_t err = settings_manager_set(comp, obj, errbuf, sizeof(errbuf),
                                         NULL);

    cJSON_Delete(obj);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s config rejected: %s", comp, errbuf);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    /* ---- compose like main --------------------------------------------- */
    ESP_ERROR_CHECK(log_manager_init());
    ESP_ERROR_CHECK(dev_status_manager_init());
    ESP_ERROR_CHECK(settings_manager_init());
    ESP_ERROR_CHECK(log_manager_register_settings());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(socket_manager_init());
    ESP_ERROR_CHECK(bridge_manager_init());
    ESP_ERROR_CHECK(obd_chip_init());

    static const bridge_endpoint_t OBD_EP =
        { "obd", obd_ep_send, obd_ep_subscribe, obd_ep_unsubscribe };
    static const bridge_endpoint_t OBD0_EP =
        { "obd0", obd0_send, obd0_subscribe, obd0_unsubscribe };
    static const bridge_endpoint_t ECHO_EP =
        { "echo", echo_send, echo_subscribe, echo_unsubscribe };
    static const bridge_endpoint_t ECHO0_EP =
        { "echo0", echo0_send, echo0_subscribe, echo0_unsubscribe };

    ESP_ERROR_CHECK(bridge_manager_register_endpoint(&OBD_EP));
    ESP_ERROR_CHECK(bridge_manager_register_endpoint(&OBD0_EP));
    ESP_ERROR_CHECK(bridge_manager_register_endpoint(&ECHO_EP));
    ESP_ERROR_CHECK(bridge_manager_register_endpoint(&ECHO0_EP));

    /* bench configuration, persisted before the boot pass */
    persist("wifi_manager",
            "{\"mode\":\"sta\",\"sta_ssid\":\"" BENCH_STA_SSID "\","
            "\"sta_password\":\"" BENCH_STA_PASS "\"}");
    persist("socket_manager",
            "{\"servers\":["
            "{\"name\":\"obd0\",\"proto\":\"tcp\",\"port\":35000,"
             "\"max_clients\":2,\"enabled\":true},"
            "{\"name\":\"echo0\",\"proto\":\"tcp\",\"port\":3334,"
             "\"max_clients\":4,\"enabled\":true}]}");
    persist("bridge_manager",
            "{\"bridges\":["
            "{\"name\":\"br_obd\",\"a\":\"obd\",\"b\":\"obd0\","
             "\"enabled\":true},"
            "{\"name\":\"br_echo\",\"a\":\"echo\",\"b\":\"echo0\","
             "\"enabled\":true}]}");

    ESP_ERROR_CHECK(settings_manager_start()); /* the one on_apply pass */
    ESP_ERROR_CHECK(log_manager_start());
    printf("INIT ok=1\n");

    esp_err_t wifi_err = wifi_manager_start();
    esp_err_t obd_err = obd_chip_start();
    esp_err_t sock_err = socket_manager_start();
    esp_err_t br_err = bridge_manager_start();

    printf("START wifi=%d obd=%d sock=%d bridge=%d\n", wifi_err == ESP_OK,
           obd_err == ESP_OK, sock_err == ESP_OK, br_err == ESP_OK);

    /* wait for the bench AP */
    EventBits_t bits = xEventGroupWaitBits(wifi_manager_get_event_group(),
                                           WIFI_MANAGER_BIT_STA_CONNECTED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(60000));

    if ((bits & WIFI_MANAGER_BIT_STA_CONNECTED) == 0)
    {
        printf("BENCH FAILED no_wifi\n");
        return;
    }

    char ip[16] = "";

    wifi_manager_get_sta_ip(ip, sizeof(ip));
    printf("BENCH READY ip=%s\n", ip);

    /* stay up for the PC-driven scenarios; report stats each 30 s at DEBUG */
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));

        bridge_stats_t s;

        if (bridge_manager_stats("br_obd", &s) == ESP_OK)
        {
            ESP_LOGD(TAG, "br_obd a2b=%lu b2a=%lu err=%lu",
                     (unsigned long)s.a2b_chunks, (unsigned long)s.b2a_chunks,
                     (unsigned long)s.send_errors);
        }
    }
}
