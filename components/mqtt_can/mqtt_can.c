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
 * @file mqtt_can.c
 * @brief Target glue (TASK_mqtt_can.md): settings, the `mqtt0` bridge
 *        endpoint, the `canmqtt` translator registration, CLI.
 *
 * Gate placement (meatpi 2026-07-22: "both tx and Rx safety gate"):
 * BOTH gates live in the ENDPOINT — allow_rx guards the outbound
 * publish (bus traffic leaving the device), allow_tx guards the
 * inbound handler (broker messages reaching the bus side). The
 * translator stays pure. Everything gate-blocked is COUNTED.
 */
#include "mqtt_can.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "cJSON.h"

#include "bridge_endpoints.h"
#include "bridge_manager.h"
#include "can_frame_wire.h"
#include "cmdline_manager.h"
#include "mqtt_manager.h"
#include "settings_manager.h"

#include "mqtt_can_codec.h"

static const char *TAG = "mqtt_can";

/* ---- settings --------------------------------------------------------------- */

#define MC_TOPIC_LEN 96

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_STR("pub_topic", MC_TOPIC_LEN, "~/can/rx"),
    SETTINGS_STR("sub_topic", MC_TOPIC_LEN, "~/can/tx"),
    SETTINGS_INT("qos", 0, 1, 0),
    SETTINGS_BOOL("allow_rx", false),   /* GATE: publish bus traffic    */
    SETTINGS_BOOL("allow_tx", false),   /* GATE: inject broker frames   */
    SETTINGS_INT("batch_ms", 10, 1000, 50),
    SETTINGS_INT("batch_frames", 1, MC_BATCH_MAX, MC_BATCH_MAX),
    SETTINGS_STR("filter", 8, ""),      /* hex id filter (empty = all)  */
    SETTINGS_STR("mask", 8, ""),        /* hex mask                     */
    SETTINGS_BOOL("filter_ext", false),
    SETTINGS_BOOL("cli", true),
};

static bool s_enabled;
static char s_pub_topic[MC_TOPIC_LEN];
static char s_sub_topic[MC_TOPIC_LEN];
static int s_qos;
static bool s_allow_rx;
static bool s_allow_tx;
static uint32_t s_batch_ms;
static uint16_t s_batch_frames;
static uint32_t s_filter;
static uint32_t s_mask;
static bool s_filter_ext;

static mqtt_can_stats_t s_stats;

static void copy_str(char *dst, size_t cap, const cJSON *obj,
                     const char *key, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    snprintf(dst, cap, "%s",
             (cJSON_IsString(v) && v->valuestring[0] != '\0')
                 ? v->valuestring : dflt);
}

static uint32_t hex_or_zero(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    return (cJSON_IsString(v) && v->valuestring[0] != '\0')
               ? (uint32_t)strtoul(v->valuestring, NULL, 16) : 0;
}

static int cmd_mqtt_can(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cmdline_printf("mqtt_can: %s  gates rx=%d tx=%d\n",
                   s_enabled ? "enabled" : "disabled", s_allow_rx,
                   s_allow_tx);
    cmdline_printf("frames rx %lu (batches %lu)  tx %lu\n",
                   (unsigned long)s_stats.frames_rx,
                   (unsigned long)s_stats.batches,
                   (unsigned long)s_stats.frames_tx);
    cmdline_printf("dropped: rx-gate %lu  tx-gate %lu  q-full %lu  "
                   "parse-err %lu\n",
                   (unsigned long)s_stats.dropped_rx_gate,
                   (unsigned long)s_stats.dropped_tx_gate,
                   (unsigned long)s_stats.dropped_q_full,
                   (unsigned long)s_stats.parse_errors);
    return 0;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    copy_str(s_pub_topic, sizeof(s_pub_topic), settings, "pub_topic",
             "~/can/rx");
    copy_str(s_sub_topic, sizeof(s_sub_topic), settings, "sub_topic",
             "~/can/tx");

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, "qos");

    s_qos = cJSON_IsNumber(v) ? v->valueint : 0;
    s_allow_rx = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "allow_rx"));
    s_allow_tx = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "allow_tx"));

    v = cJSON_GetObjectItemCaseSensitive(settings, "batch_ms");
    s_batch_ms = cJSON_IsNumber(v) ? (uint32_t)v->valueint : 50;
    v = cJSON_GetObjectItemCaseSensitive(settings, "batch_frames");
    s_batch_frames = cJSON_IsNumber(v) ? (uint16_t)v->valueint
                                       : MC_BATCH_MAX;

    s_filter = hex_or_zero(settings, "filter");
    s_mask = hex_or_zero(settings, "mask");
    s_filter_ext = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "filter_ext"));

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;
        static const esp_console_cmd_t CMD =
        {
            .command = "mqtt_can",
            .help = "CAN<->MQTT bridge stats",
            .func = cmd_mqtt_can,
        };

        if (!s_cli_registered && cmdline_manager_register(&CMD) == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    return ESP_OK;
}

esp_err_t mqtt_can_init(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "mqtt_can",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}

void mqtt_can_stats(mqtt_can_stats_t *out)
{
    *out = s_stats;
}

/* ---- topic expansion: "~" = mqtt_manager_topic_prefix() --------------------- */

static void expand_topic(const char *in, char *out, size_t cap)
{
    if (in[0] == '~')
    {
        snprintf(out, cap, "%s%s", mqtt_manager_topic_prefix(), in + 1);
    }
    else
    {
        snprintf(out, cap, "%s", in);
    }
}

/* ---- the mqtt0 endpoint ------------------------------------------------------ */

static QueueHandle_t s_bridge_q;        /* owned by the attached bridge  */
static char s_pub_full[160];
static char s_sub_full[160];            /* static: handler filters must
                                           outlive the firmware          */
static bool s_handler_registered;

static esp_err_t ep_send(const uint8_t *d, size_t l)
{
    if (!s_allow_rx)
    {
        s_stats.dropped_rx_gate++;
        return ESP_OK;          /* gate is policy, not an error storm    */
    }

    esp_err_t err = mqtt_manager_publish_async(s_pub_full, d, l, s_qos,
                                               false);

    if (err == ESP_OK)
    {
        s_stats.batches++;
    }

    return err;
}

/** esp-mqtt event task: keep short — gate, split into bridge chunks,
 *  queue. The canmqtt encode ctx reassembles fragments (its normal
 *  stream model), so chunk boundaries are free. */
static void sub_handler(const char *topic, const uint8_t *data,
                        size_t len, void *arg)
{
    (void)topic;
    (void)arg;

    if (s_bridge_q == NULL)
    {
        return;                 /* endpoint not in an active bridge      */
    }

    if (!s_allow_tx)
    {
        s_stats.dropped_tx_gate++;
        return;
    }

    for (size_t off = 0; off < len; off += BRIDGE_MANAGER_CHUNK_SIZE)
    {
        bridge_chunk_t c;
        size_t n = len - off;

        if (n > BRIDGE_MANAGER_CHUNK_SIZE)
        {
            n = BRIDGE_MANAGER_CHUNK_SIZE;
        }

        memcpy(c.data, data + off, n);
        c.len = (uint16_t)n;

        if (xQueueSend(s_bridge_q, &c, 0) != pdTRUE)
        {
            s_stats.dropped_q_full++;
            return;             /* partial object: reassembler resyncs  */
        }
    }
}

static esp_err_t ep_subscribe(QueueHandle_t q)
{
    if (!s_enabled)
    {
        /* registration is unconditional (bridge rows must VALIDATE
           regardless of this component's enabled state); attaching a
           bridge to a disabled mqtt0 fails loudly instead */
        ESP_LOGW(TAG, "bridge attach refused: mqtt_can disabled");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_bridge_q != NULL)
    {
        return ESP_ERR_INVALID_STATE; /* one bridge owns mqtt0          */
    }

    if (!s_handler_registered)
    {
        esp_err_t err = mqtt_manager_register_handler(s_sub_full,
                                                      sub_handler, NULL);

        if (err != ESP_OK)
        {
            return err;
        }

        s_handler_registered = true; /* registrations are for life      */
    }

    s_bridge_q = q;
    return ESP_OK;
}

static esp_err_t ep_unsubscribe(QueueHandle_t q)
{
    (void)q;
    s_bridge_q = NULL;          /* handler stays; it checks s_bridge_q   */
    return ESP_OK;
}

/* ---- the canmqtt translator -------------------------------------------------- */

typedef struct
{
    mc_batch_t batch;           /* decode dir (can -> mqtt)              */
    mc_txasm_t txasm;           /* encode dir (mqtt -> can)              */
    char json[MC_JSON_MAX];
} canmqtt_ctx_t;

_Static_assert(sizeof(canmqtt_ctx_t) <= 4096,
               "canmqtt ctx must fit the BM_CTX_MAX pool slot");

static esp_err_t tr_ctx_init(void *vctx)
{
    canmqtt_ctx_t *ctx = vctx;

    mc_batch_init(&ctx->batch, s_batch_frames);
    mc_txasm_init(&ctx->txasm);
    return ESP_OK;
}

static esp_err_t emit_batch(canmqtt_ctx_t *ctx, bridge_sink_fn_t sink,
                            void *sink_arg)
{
    size_t n = mc_batch_json(&ctx->batch, esp_timer_get_time() / 1000,
                             ctx->json, sizeof(ctx->json));

    if (n == 0)
    {
        return ESP_OK;
    }

    return sink(sink_arg, (const uint8_t *)ctx->json, n);
}

static esp_err_t tr_decode(void *vctx, const uint8_t *in, size_t len,
                           bridge_sink_fn_t sink, void *sink_arg)
{
    canmqtt_ctx_t *ctx = vctx;

    s_stats.frames_rx += (uint32_t)mc_batch_add_chunk(&ctx->batch, in,
                                                      len);

    if (mc_batch_ready(&ctx->batch))
    {
        return emit_batch(ctx, sink, sink_arg);
    }

    return ESP_OK;
}

typedef struct
{
    bridge_sink_fn_t sink;
    void *sink_arg;
} tx_emit_t;

static void tx_obj_cb(void *arg, const char *json, size_t len)
{
    tx_emit_t *e = arg;
    can_core_frame_t frames[8];
    int n = mc_tx_parse(json, len, frames, 8);

    if (n < 0)
    {
        s_stats.parse_errors++;
        return;
    }

    for (int i = 0; i < n; i++)
    {
        uint8_t wire[CAN_WIRE_MAX];
        size_t w = can_wire_encode(&frames[i], wire);

        if (e->sink(e->sink_arg, wire, w) == ESP_OK)
        {
            s_stats.frames_tx++;
        }
    }
}

static esp_err_t tr_encode(void *vctx, const uint8_t *in, size_t len,
                           bridge_sink_fn_t sink, void *sink_arg)
{
    canmqtt_ctx_t *ctx = vctx;
    tx_emit_t e = { .sink = sink, .sink_arg = sink_arg };

    (void)mc_txasm_feed(&ctx->txasm, in, len, tx_obj_cb, &e);
    return ESP_OK;
}

static esp_err_t tr_flush(void *vctx, bridge_sink_fn_t sink,
                          void *sink_arg)
{
    canmqtt_ctx_t *ctx = vctx;

    /* the encode-direction ctx never accumulates a batch — this is a
       no-op there by construction */
    return emit_batch(ctx, sink, sink_arg);
}

/* ---- registration ------------------------------------------------------------ */

esp_err_t mqtt_can_start(void)
{
    /* jack + codec register UNCONDITIONALLY: bridge settings rows
       naming mqtt0/canmqtt must validate whether or not this component
       is enabled (found by the first bench run — the bridge PUT was
       rejected with "unknown endpoint 'mqtt0'" on a disabled build).
       `enabled` gates the ATTACH (ep_subscribe) instead. */
    expand_topic(s_pub_topic, s_pub_full, sizeof(s_pub_full));
    expand_topic(s_sub_topic, s_sub_full, sizeof(s_sub_full));

    if (s_enabled && s_mask != 0)
    {
        /* meatpi 2026-07-22: filters map 1:1 onto the can endpoint */
        bep_can_set_filter(s_filter, s_mask, s_filter_ext);
    }

    static const bridge_endpoint_t EP =
    {
        .name = "mqtt0",
        .send = ep_send,
        .subscribe = ep_subscribe,
        .unsubscribe = ep_unsubscribe,
        .multi_consumer = false,
    };

    static bridge_translator_t TR =
    {
        .name = "canmqtt",
        .ctx_size = sizeof(canmqtt_ctx_t),
        .wants_reply = false,
        .ctx_init = tr_ctx_init,
        .decode = tr_decode,
        .encode = tr_encode,
        .flush = tr_flush,
    };

    TR.flush_ms = s_batch_ms;

    esp_err_t err = bridge_manager_register_endpoint(&EP);

    if (err == ESP_OK)
    {
        err = bridge_manager_register_translator(&TR);
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "mqtt0 + canmqtt registered (%s; pub %s%s, "
                      "sub %s%s, batch %u fr / %lu ms)",
                 s_enabled ? "enabled" : "disabled",
                 s_pub_full, s_allow_rx ? "" : " [rx GATED]",
                 s_sub_full, s_allow_tx ? "" : " [tx GATED]",
                 s_batch_frames, (unsigned long)s_batch_ms);
    }

    return err;
}
