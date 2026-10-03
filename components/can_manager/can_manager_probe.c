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
 * @file can_manager_probe.c
 * @brief What is on the bus, without transmitting: the probe (bitrate /
 *        silent / unreadable), the held listen-only watch, and the sample
 *        of identifiers heard (for whoever must tell a J1939 network from
 *        an OBD port before the native bus is enabled). Split out of
 *        can_manager.c 2026-10-03 (700-line rule); the probe and the watch
 *        are unchanged. The one bus handle and its life lock stay in
 *        can_manager.c (can_manager_private.h accessors).
 */
#include "can_manager.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_manager_private.h"

static const char *TAG = "can_manager";

static volatile bool s_probing;         /* a temporary probe node is up     */
static volatile bool s_watching;        /* a held listen-only node is up    */
static int64_t s_node_up_us;            /* when the node in use came up     */

static can_manager_probe_t s_last_probe;
static int64_t s_last_probe_us;

bool cm_probe_node_up(void)
{
    return s_probing || s_watching;
}

void cm_probe_node_dropped(void)
{
    s_watching = false;
}

void cm_node_came_up(void)
{
    s_node_up_us = esp_timer_get_time();
}

/* ---- what is on the bus? ------------------------------------------------------ */

const char *can_manager_probe_name(can_probe_result_t result)
{
    switch (result)
    {
    case CAN_PROBE_SILENT:     return "silent";
    case CAN_PROBE_LIVE:       return "live";
    case CAN_PROBE_UNREADABLE: return "unreadable";
    default:                   return "none";
    }
}

/** can_manager is disabled by settings: the listen-only node (no TX pin) a
 *  probe or a watch reads, on the two ISO 15765-4 / J1939 rates. */
static esp_err_t listener_up(void)
{
    can_core_config_t cfg =
    {
        .tx_gpio           = CONFIG_WICAN_CAN_TX_GPIO,
        .rx_gpio           = CONFIG_WICAN_CAN_RX_GPIO,
        .baud_kbps         = 500,
        .silent_mode       = true,
        .rx_queue_depth    = 32,
        .tx_queue_depth    = 4,
        .baud_candidates   = { 500, 250 },
        .n_baud_candidates = 2,
        .link_retry_ms     = 200,  /* find a bus that wakes up, soon */
        .quiet_link        = true,
    };

    cm_standby(false);

    if (can_core_init(cm_bus(), &cfg) != ELM327_OK)
    {
        cm_standby(true);
        return ESP_FAIL;
    }

    cm_node_came_up();
    return ESP_OK;
}

static void listener_down(void)
{
    can_core_deinit(cm_bus());
    cm_standby(true);
}

static uint32_t s_heard;    /* what the listener had counted at the last look */
static int64_t  s_heard_us; /* when a look last found that changed            */

/** The verdict of the node that is up. false = none yet: look again in a
 *  moment. The link policy does the judging: frames prove a bitrate at
 *  once, unreadable traffic is a mismatch within milliseconds. Silent is
 *  what is left when the node has been up, and has heard nothing at all
 *  (no frame, no receive error, no busy RX line, no candidate change), for
 *  CM_PROBE_SILENT_MS. */
static bool read_verdict(can_manager_probe_t *out, int64_t now_us)
{
    can_core_link_t link;
    can_core_stats_t st;

    can_core_get_link(cm_bus(), &link);
    can_core_get_stats(cm_bus(), &st);
    out->frames = st.rx_count;

    uint32_t heard = st.rx_count + st.rx_bad + st.rx_deaf + link.switches;

    if (heard != s_heard)
    {
        s_heard = heard;
        s_heard_us = now_us;
    }

    if (link.verified)
    {
        out->result = CAN_PROBE_LIVE;
        out->baud_kbps = link.baud_kbps;
        return true;
    }

    if (strcmp(link.state, "mismatch") == 0)
    {
        out->result = CAN_PROBE_UNREADABLE;
        return true;
    }

    int64_t quiet_from = (s_heard_us > s_node_up_us) ? s_heard_us
                                                     : s_node_up_us;

    if (now_us - quiet_from >= (int64_t)CM_PROBE_SILENT_MS * 1000)
    {
        out->result = CAN_PROBE_SILENT;
        return true;
    }

    return false;
}

esp_err_t can_manager_watch(bool on)
{
    if (!cm_started())
    {
        return ESP_ERR_INVALID_STATE;
    }

    cm_life_take();

    esp_err_t err = ESP_OK;

    if (!cm_started())
    {
        err = ESP_ERR_INVALID_STATE; /* stopped while we waited (sleep) */
    }
    else if (on && !cm_running() && !s_watching)
    {
        err = listener_up();
        s_watching = (err == ESP_OK);

        if (s_watching)
        {
            ESP_LOGD(TAG, "watch: listening to the bus (no TX pin)");
        }
    }
    else if (!on && s_watching)
    {
        s_watching = false;
        listener_down();
        ESP_LOGD(TAG, "watch: off");
    }

    cm_life_give();
    return err;
}

esp_err_t can_manager_probe(can_manager_probe_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (!cm_started())
    {
        return ESP_ERR_INVALID_STATE;
    }

    cm_life_take();

    esp_err_t err = ESP_OK;
    int64_t t0 = esp_timer_get_time();

    if (!cm_started())
    {
        err = ESP_ERR_INVALID_STATE; /* stopped while we waited (sleep) */
    }
    else
    {
        /* a running bus or a watch already listens; otherwise a node of
           our own for as long as the verdict takes */
        bool temporary = !cm_running() && !s_watching;

        if (temporary)
        {
            err = listener_up();
            s_probing = (err == ESP_OK);
        }

        if (err == ESP_OK)
        {
            while (!read_verdict(out, esp_timer_get_time()))
            {
                if (esp_timer_get_time() - t0 >=
                    (int64_t)CM_PROBE_MAX_MS * 1000)
                {
                    /* neither silent nor readable for this long (sparse
                       errors, no frame): something is there */
                    out->result = CAN_PROBE_UNREADABLE;
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(10));
            }

            out->took_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        }

        if (temporary && s_probing)
        {
            s_probing = false;
            listener_down();
        }
    }

    if (err == ESP_OK)
    {
        /* a bitrate read off the bus is worth a line, once; "silent" and
           "unreadable" come and go with the OBD chip's own unanswered
           requests and are the asking guard's to report */
        if (out->result == CAN_PROBE_LIVE &&
            (s_last_probe.result != CAN_PROBE_LIVE ||
             out->baud_kbps != s_last_probe.baud_kbps))
        {
            ESP_LOGI(TAG, "probe: bus live at %lu kbit/s (%lu frames, "
                          "%lu ms)",
                     (unsigned long)out->baud_kbps,
                     (unsigned long)out->frames,
                     (unsigned long)out->took_ms);
        }

        s_last_probe = *out;
        s_last_probe_us = esp_timer_get_time();
    }

    cm_life_give();
    return err;
}

void can_manager_last_probe(can_manager_probe_t *out, uint32_t *age_ms)
{
    if (out != NULL)
    {
        *out = s_last_probe;
    }

    if (age_ms != NULL)
    {
        *age_ms = (s_last_probe_us == 0)
                      ? 0
                      : (uint32_t)((esp_timer_get_time() - s_last_probe_us)
                                   / 1000);
    }
}

/* ---- which identifiers are on the bus? ----------------------------------------- */

#define CM_SAMPLE_Q_LEN  64
#define CM_SAMPLE_LINK_MS 4000u /* a node of its own: the bitrate found by
                                   listening within this, else no sample   */

static uint8_t s_sample_q_store[CM_SAMPLE_Q_LEN * sizeof(can_core_frame_t)]
    EXT_RAM_BSS_ATTR;                   /* PSRAM: the frames wait here      */
static StaticQueue_t s_sample_q_buf;    /* internal: FreeRTOS object        */
static QueueHandle_t s_sample_q;

/** One frame into the table: a known id counts up, a new one takes a free
 *  row; with none left the frame is counted and the id forgotten. */
static void sample_note(can_id_seen_t *out, size_t max, size_t *n,
                        const can_core_frame_t *f)
{
    for (size_t i = 0; i < *n; i++)
    {
        if (out[i].id == f->id && out[i].ext == f->ext)
        {
            if (out[i].count < UINT16_MAX)
            {
                out[i].count++;
            }

            out[i].dlc = f->dlc;
            return;
        }
    }

    if (*n < max)
    {
        out[*n].id = f->id;
        out[*n].ext = f->ext;
        out[*n].dlc = f->dlc;
        out[*n].count = 1;
        (*n)++;
    }
}

esp_err_t can_manager_sample_ids(uint32_t ms, can_id_seen_t *out, size_t max,
                                 size_t *n, uint32_t *frames)
{
    if (out == NULL || n == NULL || max == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *n = 0;

    if (frames != NULL)
    {
        *frames = 0;
    }

    if (!cm_started())
    {
        return ESP_ERR_INVALID_STATE;
    }

    cm_life_take();

    esp_err_t err = ESP_OK;
    bool temporary = false;

    if (!cm_started())
    {
        err = ESP_ERR_INVALID_STATE; /* stopped while we waited (sleep) */
    }
    else
    {
        temporary = !cm_running() && !s_watching;

        if (temporary)
        {
            err = listener_up();
            s_probing = (err == ESP_OK);

            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "sample: no node (%s)", esp_err_to_name(err));
            }
        }
    }

    if (err == ESP_OK)
    {
        if (s_sample_q == NULL)
        {
            s_sample_q = xQueueCreateStatic(CM_SAMPLE_Q_LEN,
                                            sizeof(can_core_frame_t),
                                            s_sample_q_store, &s_sample_q_buf);
        }

        can_core_queue_subscriber_t sub =
        {
            .queue       = s_sample_q,
            .monitor_all = true,
            .active      = true,
        };

        xQueueReset(s_sample_q);

        int idx = can_core_register_rx_queue(cm_bus(), &sub);

        if (idx < 0)
        {
            err = ESP_ERR_NO_MEM;
        }
        else
        {
            /* a node of our own first has to find the bitrate (the link
               policy reads the bus, 500 then 250 kbit/s): the window opens
               once the link is verified, or after CM_SAMPLE_LINK_MS without
               a verdict (a silent bus: nothing to sample). A running bus or
               a watch is verified already. */
            int64_t t0 = esp_timer_get_time();
            int64_t link_by = t0 + (int64_t)CM_SAMPLE_LINK_MS * 1000;
            can_core_link_t link;

            can_core_get_link(cm_bus(), &link);

            while (!link.verified && esp_timer_get_time() < link_by)
            {
                vTaskDelay(pdMS_TO_TICKS(20));
                can_core_get_link(cm_bus(), &link);
            }

            xQueueReset(s_sample_q);    /* the window starts now */

            int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
            uint32_t count = 0;
            can_core_frame_t f;

            while (esp_timer_get_time() < deadline)
            {
                if (xQueueReceive(s_sample_q, &f, pdMS_TO_TICKS(20)) != pdTRUE)
                {
                    continue;
                }

                count++;
                sample_note(out, max, n, &f);
            }

            can_core_unregister_rx_queue(cm_bus(), idx);

            if (frames != NULL)
            {
                *frames = count;
            }

            can_core_get_link(cm_bus(), &link);
            ESP_LOGI(TAG, "sample: %s node, %lu frames, %u ids in %lu ms, "
                          "link %s%s at %lu kbit/s",
                     temporary ? "own" : s_watching ? "watch" : "running",
                     (unsigned long)count, (unsigned)*n,
                     (unsigned long)((esp_timer_get_time() - t0) / 1000),
                     link.state, link.verified ? " (verified)" : "",
                     (unsigned long)link.baud_kbps);
        }
    }

    if (temporary && s_probing)
    {
        s_probing = false;
        listener_down();
    }

    cm_life_give();
    return err;
}
