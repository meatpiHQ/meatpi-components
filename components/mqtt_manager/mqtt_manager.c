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
 * @file mqtt_manager.c
 * @brief Lifecycle, the esp-mqtt client glue (events, LWT, TLS), the
 *        handler registry, and the network-gated starter task (see
 *        include/mqtt_manager.h for the model). Settings live in
 *        mqtt_manager_settings.c (standard §4.1).
 */
#include "mqtt_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "cert_manager.h"
#include "dev_status_manager.h"
#include "filesystem.h"
#include "log_manager.h"

#include "mqtt_manager_private.h"

static const char *TAG = "mqtt_manager";

/* the settings buffer must fit any cert_manager set name */
_Static_assert(sizeof(((mm_config_t *)0)->cert_set) ==
               CERT_MANAGER_NAME_MAX + 1, "mm_config_t.cert_set size");

/* legacy on-wire contract — byte-identical payloads */
#define MM_STATUS_ONLINE  "{\"status\": \"online\"}"
#define MM_STATUS_OFFLINE "{\"status\": \"offline\"}"

#define MM_RX_BUF      4096
#define MM_OUT_BUF     4096
#define MM_CA_PEM_MAX  8192
#define MM_RING_BYTES  (32 * 1024) /* async publish elasticity (PSRAM)  */

typedef struct
{
    const char           *filter;
    mqtt_manager_msg_cb_t cb;
    void                 *arg;
} mm_handler_t;

static esp_mqtt_client_handle_t s_client;
static mm_handler_t s_handlers[MQTT_MANAGER_MAX_HANDLERS] EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */
static volatile bool s_connected;
static uint32_t s_frag_drops;
static bool s_started;

/* starter task (PSRAM stack: network only, no flash writes) */
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                            /* internal: FreeRTOS */
static StackType_t s_stack[3072] EXT_RAM_BSS_ATTR;

/* async publish path: bounded PSRAM ring drained by the publisher task —
 * ALL socket blocking lives here, never in a producer */
static RingbufHandle_t s_ring;
static TaskHandle_t s_pub_task;
static StaticTask_t s_pub_tcb;                        /* internal: FreeRTOS */
static StackType_t s_pub_stack[4096] EXT_RAM_BSS_ATTR;
static mqtt_manager_stats_t s_stats;

/* runtime identity: on_apply seeds these (mm_core_set_identity); start()
 * resolves the device-id defaults in place. The rest of the boot-applied
 * config lives in mqtt_manager_settings.c (mm_settings_config()). */
static char s_client_id[64];
static char s_prefix[64];
static char s_status_topic[80];
static char *s_ca_pem; /* PSRAM, loaded once at start when ca_file set */

/* ---- settings glue (descriptor in mqtt_manager_settings.c) ----------------- */

void mm_core_set_identity(const char *client_id, const char *prefix)
{
    snprintf(s_client_id, sizeof(s_client_id), "%s", client_id);
    snprintf(s_prefix, sizeof(s_prefix), "%s", prefix);
}

/* ---- event handling ----------------------------------------------------------- */

static void dispatch(const char *topic, size_t topic_len,
                     const uint8_t *data, size_t len)
{
    /* NUL-bounded copy so handlers get a normal C-string topic */
    char tbuf[128];

    if (topic_len >= sizeof(tbuf))
    {
        return;
    }

    memcpy(tbuf, topic, topic_len);
    tbuf[topic_len] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < MQTT_MANAGER_MAX_HANDLERS; i++)
    {
        if (s_handlers[i].cb != NULL &&
            mm_topic_matches(s_handlers[i].filter, tbuf))
        {
            s_handlers[i].cb(tbuf, data, len, s_handlers[i].arg);
        }
    }

    xSemaphoreGive(s_lock);
}

static void subscribe_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < MQTT_MANAGER_MAX_HANDLERS; i++)
    {
        if (s_handlers[i].cb != NULL)
        {
            esp_mqtt_client_subscribe(s_client, s_handlers[i].filter, 0);
        }
    }

    xSemaphoreGive(s_lock);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id,
                       void *event_data)
{
    esp_mqtt_event_handle_t evt = event_data;

    (void)arg;
    (void)base;

    switch ((esp_mqtt_event_id_t)id)
    {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            dev_status_manager_set(DEV_STATUS_BIT_MQTT_CONNECTED);
            esp_mqtt_client_publish(s_client, s_status_topic,
                                    MM_STATUS_ONLINE, 0, 0, 1);
            subscribe_all();
            ESP_LOGI(TAG, "connected (%s)", mm_settings_config()->url);
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            dev_status_manager_clear(DEV_STATUS_BIT_MQTT_CONNECTED);
            ESP_LOGI(TAG, "disconnected");
            break;

        case MQTT_EVENT_DATA:
            if (evt->current_data_offset == 0 &&
                evt->data_len == evt->total_data_len)
            {
                dispatch(evt->topic, (size_t)evt->topic_len,
                         (const uint8_t *)evt->data,
                         (size_t)evt->data_len);
            }
            else
            {
                s_frag_drops++; /* > RX buffer: v1 drops fragments */

                if ((s_frag_drops % 100) == 1)
                {
                    ESP_LOGW(TAG, "oversized payload dropped (%lu total)",
                             (unsigned long)s_frag_drops);
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            /* retry chatter stays at DEBUG (standard §10) */
            ESP_LOGD(TAG, "transport error (reconnect pending)");
            break;

        default:
            break;
    }
}

/* ---- network-gated starter ----------------------------------------------------- */

static void starter_task(void *arg)
{
    (void)arg;
    dev_status_manager_wait_any(DEV_STATUS_NETWORK_CONNECTED_MASK,
                                portMAX_DELAY);
    esp_mqtt_client_start(s_client); /* reconnects handled by esp-mqtt */
    vTaskDelete(NULL);
}

/* ---- async publisher ----------------------------------------------------------- */

static void publisher_task(void *arg)
{
    (void)arg;

    while (true)
    {
        size_t item_len = 0;
        uint8_t *item = xRingbufferReceive(s_ring, &item_len,
                                           portMAX_DELAY);

        if (item == NULL)
        {
            continue;
        }

        mm_item_hdr_t hdr;
        const char *topic;
        const uint8_t *data;

        if (mm_item_unpack(item, item_len, &hdr, &topic, &data))
        {
            if (s_connected &&
                esp_mqtt_client_publish(s_client, topic,
                                        (const char *)data, hdr.data_len,
                                        hdr.qos, hdr.retain) >= 0)
            {
                s_stats.published++;
            }
            else
            {
                s_stats.dropped_offline++; /* keep draining during
                                              outages: newest data wins */
            }
        }

        vRingbufferReturnItem(s_ring, item);
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t mqtt_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "mqtt_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    mm_events_register(); /* mqtt.rx source + mqtt.publish action */
    return mm_settings_register();
}

/** mqtts:// verification, priority order: a cert_manager SET (CA +
 *  optional client cert/key = mutual TLS) > a raw ca_file > the built-in
 *  certificate bundle. */
static void configure_tls(esp_mqtt_client_config_t *cfg)
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

esp_err_t mqtt_manager_start(void)
{
    if (!mm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    const mm_config_t *mc = mm_settings_config();

    /* resolve the derived defaults now (device id is available) */
    if (s_client_id[0] == '\0')
    {
        snprintf(s_client_id, sizeof(s_client_id), "wican_%s",
                 dev_status_manager_device_id());
    }

    if (s_prefix[0] == '\0')
    {
        snprintf(s_prefix, sizeof(s_prefix), "wican/%s",
                 dev_status_manager_device_id());
    }

    snprintf(s_status_topic, sizeof(s_status_topic), "%s/status",
             s_prefix);

    if (!mc->enabled)
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    esp_mqtt_client_config_t cfg =
    {
        .broker.address.uri = mc->url,
        .credentials.client_id = s_client_id,
        .network.reconnect_timeout_ms = 5000,
        .session.keepalive = (int)mc->keepalive_s,
        .session.last_will =
        {
            .topic = s_status_topic,
            .msg = MM_STATUS_OFFLINE,
            .retain = 1,
        },
        .buffer.size = MM_RX_BUF,
        .buffer.out_size = MM_OUT_BUF,
    };

    if (mc->username[0] != '\0')
    {
        cfg.credentials.username = mc->username;
        cfg.credentials.authentication.password = mc->password;
    }

    if (strncmp(mc->url, "mqtts://", 8) == 0)
    {
        configure_tls(&cfg);
    }

    s_client = esp_mqtt_client_init(&cfg);

    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "client init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event,
                                   NULL);

    /* the async path: PSRAM ring + the one publisher task */
    if (s_ring == NULL)
    {
        s_ring = xRingbufferCreateWithCaps(MM_RING_BYTES,
                                           RINGBUF_TYPE_NOSPLIT,
                                           MALLOC_CAP_SPIRAM);
    }

    if (s_ring == NULL)
    {
        ESP_LOGE(TAG, "publish ring alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_pub_task = xTaskCreateStatic(publisher_task, "mqtt_pub",
                                   sizeof(s_pub_stack) /
                                       sizeof(s_pub_stack[0]),
                                   NULL, 5, s_pub_stack, &s_pub_tcb);
    s_task = xTaskCreateStatic(starter_task, "mqtt_start",
                               sizeof(s_stack) / sizeof(s_stack[0]),
                               NULL, 4, s_stack, &s_tcb);

    if (s_task == NULL || s_pub_task == NULL)
    {
        return ESP_FAIL;
    }

    s_started = true;
    mm_events_start(); /* subscribe the rules' mqtt.rx topics */
    ESP_LOGI(TAG, "started (%s, id %s, prefix %s, keepalive %lus)",
             mc->url, s_client_id, s_prefix,
             (unsigned long)mc->keepalive_s);
    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (s_started && s_client != NULL)
    {
        esp_mqtt_client_stop(s_client);
        s_connected = false;
        dev_status_manager_clear(DEV_STATUS_BIT_MQTT_CONNECTED);
        s_started = false;
    }

    return ESP_OK;
}

/* ---- public API --------------------------------------------------------------- */

bool mqtt_manager_connected(void)
{
    return s_connected;
}

const char *mqtt_manager_topic_prefix(void)
{
    return s_prefix;
}

esp_err_t mqtt_manager_publish(const char *topic, const void *data,
                               size_t len, int qos, bool retain)
{
    if (topic == NULL || (data == NULL && len > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_connected)
    {
        return ESP_ERR_INVALID_STATE;
    }

    int id = esp_mqtt_client_publish(s_client, topic, data, (int)len, qos,
                                     retain ? 1 : 0);

    if (id >= 0)
    {
        s_stats.published++;
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t mqtt_manager_publish_async(const char *topic, const void *data,
                                     size_t len, int qos, bool retain)
{
    if (topic == NULL || (data == NULL && len > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ring == NULL || !s_connected)
    {
        s_stats.dropped_offline++;
        return ESP_ERR_INVALID_STATE;
    }

    size_t need = mm_item_size(strlen(topic), len);

    if (need == 0)
    {
        return ESP_ERR_INVALID_ARG; /* topic/payload out of bounds */
    }

    /* acquire-pack-complete: one copy straight into the ring */
    void *slot = NULL;

    if (xRingbufferSendAcquire(s_ring, &slot, need, 0) != pdTRUE)
    {
        s_stats.dropped_full++;
        return ESP_ERR_NO_MEM; /* never block the producer */
    }

    mm_item_pack(slot, need, topic, data, len, qos, retain);
    xRingbufferSendComplete(s_ring, slot);
    return ESP_OK;
}

esp_err_t mqtt_manager_stats(mqtt_manager_stats_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_stats;
    return ESP_OK;
}

esp_err_t mqtt_manager_register_handler(const char *filter,
                                        mqtt_manager_msg_cb_t cb,
                                        void *arg)
{
    if (filter == NULL || filter[0] == '\0' || cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < MQTT_MANAGER_MAX_HANDLERS; i++)
    {
        if (s_handlers[i].cb == NULL)
        {
            s_handlers[i] = (mm_handler_t)
                { .filter = filter, .cb = cb, .arg = arg };
            err = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_lock);

    if (err == ESP_OK && s_connected)
    {
        esp_mqtt_client_subscribe(s_client, filter, 0);
    }

    return err;
}
