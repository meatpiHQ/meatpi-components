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
 * @file log_sinks.c
 * @brief Engine: the four log_manager sink callbacks (log-task context,
 *        ring-copy only, never block — §9.5) and the network flusher task
 *        (PSRAM stack) serving the TCP tail server, the UDP collector
 *        push and the ws_log channel. The file half (internal-stack
 *        writer) lives in log_sinks_file.c.
 *
 * Recursion rule: NOTHING on the write-callback or per-batch flush path
 * may ESP_LOG — sink callbacks run inside the log task itself. Lifecycle
 * transitions (client attach/detach, resolve failures) log at DEBUG from
 * the flusher task only.
 */
#include "log_sinks.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "log_manager.h"
#include "websocket_manager.h"

#include "log_sinks_core.h"
#include "log_sinks_private.h"

static const char *TAG = "log_sinks";

#define LS_WS_CHANNEL     "ws_log"
#define LS_SEND_TIMEOUT_MS 200  /* stalled tail client = dead client      */
#define LS_RESOLVE_BACKOFF_MS 5000

typedef struct
{
    uint32_t in;
    uint32_t out;
    uint32_t dropped;
} ls_counters_t;

/* rings + batch buffer: PSRAM .bss (§2) */
static uint8_t s_tcp_buf[LS_TCP_RING_SZ] EXT_RAM_BSS_ATTR;
static uint8_t s_udp_buf[LS_UDP_RING_SZ] EXT_RAM_BSS_ATTR;
static uint8_t s_ws_buf[LS_WS_RING_SZ] EXT_RAM_BSS_ATTR;
static uint8_t s_file_buf[LS_FILE_RING_SZ] EXT_RAM_BSS_ATTR;
static uint8_t s_batch[2048] EXT_RAM_BSS_ATTR; /* net-task drain unit */

static ls_ring_t s_ring[LOG_SINKS_COUNT];
static ls_counters_t s_cnt[LOG_SINKS_COUNT];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; /* internal: spinlock */

static volatile bool s_net_up;   /* net engine running (tcp/udp/ws)    */
static volatile bool s_file_up;  /* file writer running                */
static volatile bool s_running;

static int s_listen_fd = -1;
static int s_client_fd[LS_TCP_MAX_CLIENTS];
static volatile int s_tcp_clients;

static int s_udp_fd = -1;
static struct sockaddr_storage s_udp_dest;
static socklen_t s_udp_dest_len;
static bool s_udp_resolved;
static TickType_t s_udp_next_resolve;
static uint32_t s_udp_fail;

static volatile uint16_t s_ws_clients;

static TaskHandle_t s_net_task;
static StaticTask_t s_net_tcb;                             /* internal: FreeRTOS object */
static StackType_t s_net_stack[6144] EXT_RAM_BSS_ATTR;     /* no flash writes here */

/* ---- sink callbacks (log-task context: copy + notify, nothing else) ---------- */

static void sink_push(log_sinks_id_t id, const char *line, size_t len)
{
    if (len > UINT16_MAX)
    {
        len = UINT16_MAX;
    }

    portENTER_CRITICAL(&s_lock);
    s_cnt[id].in++;
    s_cnt[id].dropped += ls_ring_push(&s_ring[id], line, (uint16_t)len);
    portEXIT_CRITICAL(&s_lock);
}

static void net_notify(void)
{
    TaskHandle_t t = s_net_task; /* snapshot: stop() nulls it async */

    if (t != NULL)
    {
        xTaskNotifyGive(t);
    }
}

static esp_err_t tcp_sink_write(const char *line, size_t len)
{
    if (s_net_up && s_tcp_clients > 0)
    {
        sink_push(LOG_SINKS_TCP, line, len);
        net_notify();
    }

    return ESP_OK;
}

static esp_err_t udp_sink_write(const char *line, size_t len)
{
    if (s_net_up)
    {
        sink_push(LOG_SINKS_UDP, line, len);
        net_notify();
    }

    return ESP_OK;
}

static esp_err_t ws_sink_write(const char *line, size_t len)
{
    if (s_net_up && s_ws_clients > 0)
    {
        sink_push(LOG_SINKS_WS, line, len);
        net_notify();
    }

    return ESP_OK;
}

static esp_err_t file_sink_write(const char *line, size_t len)
{
    if (s_file_up)
    {
        sink_push(LOG_SINKS_FILE, line, len);
        ls_file_notify();
    }

    return ESP_OK;
}

static const log_sink_t S_TCP_SINK = { "tcp", tcp_sink_write };
static const log_sink_t S_UDP_SINK = { "udp", udp_sink_write };
static const log_sink_t S_WS_SINK = { "ws", ws_sink_write };
static const log_sink_t S_FILE_SINK = { "file", file_sink_write };

/* locked pop shared by the drains (and the file writer via the wrappers) */
static uint32_t ring_pop_batch(log_sinks_id_t id, void *dst, uint32_t budget,
                               uint32_t *lines_out)
{
    portENTER_CRITICAL(&s_lock);

    uint32_t lines = 0;
    uint32_t n = ls_ring_pop_batch(&s_ring[id], dst, budget, &lines);

    s_cnt[id].out += lines;
    portEXIT_CRITICAL(&s_lock);

    if (lines_out != NULL)
    {
        *lines_out = lines;
    }

    return n;
}

uint32_t ls_file_ring_pop_batch(void *dst, uint32_t budget,
                                uint32_t *lines_out)
{
    return ring_pop_batch(LOG_SINKS_FILE, dst, budget, lines_out);
}

uint32_t ls_file_ring_used(void)
{
    portENTER_CRITICAL(&s_lock);

    uint32_t used = ls_ring_used(&s_ring[LOG_SINKS_FILE]);

    portEXIT_CRITICAL(&s_lock);
    return used;
}

/* ---- TCP tail server ----------------------------------------------------------- */

static void tcp_client_close(int slot)
{
    if (s_client_fd[slot] >= 0)
    {
        close(s_client_fd[slot]);
        s_client_fd[slot] = -1;
        s_tcp_clients--;
        ESP_LOGD(TAG, "tcp tail client gone (%d left)", s_tcp_clients);
    }
}

static void tcp_accept_poll(void)
{
    if (s_listen_fd < 0)
    {
        return;
    }

    for (;;)
    {
        int fd = accept(s_listen_fd, NULL, NULL);

        if (fd < 0)
        {
            return; /* EWOULDBLOCK: nobody waiting */
        }

        int slot = -1;

        for (int i = 0; i < LS_TCP_MAX_CLIENTS; i++)
        {
            if (s_client_fd[i] < 0)
            {
                slot = i;
                break;
            }
        }

        if (slot < 0)
        {
            close(fd); /* table full: refuse quietly */
            continue;
        }

        struct timeval tv = { .tv_usec = LS_SEND_TIMEOUT_MS * 1000 };
        int one = 1;

        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        s_client_fd[slot] = fd;
        s_tcp_clients++;
        ESP_LOGD(TAG, "tcp tail client attached (%d)", s_tcp_clients);
    }
}

/** Whole-buffer send under SO_SNDTIMEO; false = dead/stalled client. */
static bool send_all(int fd, const uint8_t *p, uint32_t n)
{
    while (n > 0)
    {
        int w = send(fd, p, n, 0);

        if (w <= 0)
        {
            return false;
        }

        p += w;
        n -= (uint32_t)w;
    }

    return true;
}

static void drain_tcp(void)
{
    while (s_tcp_clients > 0)
    {
        uint32_t n = ring_pop_batch(LOG_SINKS_TCP, s_batch, sizeof(s_batch),
                                    NULL);

        if (n == 0)
        {
            return;
        }

        for (int i = 0; i < LS_TCP_MAX_CLIENTS; i++)
        {
            if (s_client_fd[i] >= 0 && !send_all(s_client_fd[i], s_batch, n))
            {
                tcp_client_close(i);
            }
        }
    }
}

/* ---- UDP collector push ---------------------------------------------------------- */

static bool udp_ready(void)
{
    const log_sinks_config_t *cfg = ls_settings_config();

    if (s_udp_resolved)
    {
        return true;
    }

    TickType_t now = xTaskGetTickCount();

    if (s_udp_next_resolve != 0 &&
        (int32_t)(now - s_udp_next_resolve) < 0)
    {
        return false;
    }

    s_udp_next_resolve = now + pdMS_TO_TICKS(LS_RESOLVE_BACKOFF_MS);

    char port[8];
    struct addrinfo hints = { .ai_family = AF_INET,
                              .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;

    snprintf(port, sizeof(port), "%u", (unsigned)cfg->udp_port);

    if (getaddrinfo(cfg->udp_host, port, &hints, &res) != 0 || res == NULL)
    {
        ESP_LOGD(TAG, "udp collector '%s' not resolvable yet",
                 cfg->udp_host);
        return false;
    }

    memcpy(&s_udp_dest, res->ai_addr, res->ai_addrlen);
    s_udp_dest_len = res->ai_addrlen;
    freeaddrinfo(res);

    if (s_udp_fd < 0)
    {
        s_udp_fd = socket(s_udp_dest.ss_family, SOCK_DGRAM, IPPROTO_IP);
    }

    s_udp_resolved = (s_udp_fd >= 0);

    if (s_udp_resolved)
    {
        ESP_LOGI(TAG, "udp sink -> %s:%u", cfg->udp_host,
                 (unsigned)cfg->udp_port);
    }

    return s_udp_resolved;
}

static void drain_udp(void)
{
    portENTER_CRITICAL(&s_lock);

    uint32_t queued = s_ring[LOG_SINKS_UDP].count;

    portEXIT_CRITICAL(&s_lock);

    if (queued == 0 || !udp_ready())
    {
        return;
    }

    for (;;)
    {
        uint32_t n = ring_pop_batch(LOG_SINKS_UDP, s_batch,
                                    LS_UDP_BATCH_MAX, NULL);

        if (n == 0)
        {
            return;
        }

        if (sendto(s_udp_fd, s_batch, n, 0,
                   (struct sockaddr *)&s_udp_dest, s_udp_dest_len) < 0)
        {
            s_udp_fail++;
            return; /* no route yet: the ring keeps absorbing */
        }
    }
}

/* ---- ws_log channel ---------------------------------------------------------------- */

static void drain_ws(void)
{
    websocket_stats_t st;

    if (websocket_manager_stats(LS_WS_CHANNEL, &st) != ESP_OK)
    {
        s_ws_clients = 0; /* channel removed from settings: sink idles */
        return;
    }

    s_ws_clients = st.clients;

    while (s_ws_clients > 0)
    {
        uint32_t n = ring_pop_batch(LOG_SINKS_WS, s_batch, LS_WS_FRAME_MAX,
                                    NULL);

        if (n == 0)
        {
            return;
        }

        if (websocket_manager_send(LS_WS_CHANNEL, s_batch, n) != ESP_OK)
        {
            return;
        }
    }
}

/* ---- the network flusher task ------------------------------------------------------- */

static void net_task(void *arg)
{
    const log_sinks_config_t *cfg = ls_settings_config();

    (void)arg;

    while (s_running)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (cfg->tcp_enabled)
        {
            tcp_accept_poll();
            drain_tcp();
        }

        if (cfg->udp_enabled)
        {
            drain_udp();
        }

        if (cfg->ws_enabled)
        {
            drain_ws();
        }
    }

    if (s_listen_fd >= 0)
    {
        close(s_listen_fd);
        s_listen_fd = -1;
    }

    for (int i = 0; i < LS_TCP_MAX_CLIENTS; i++)
    {
        tcp_client_close(i);
    }

    if (s_udp_fd >= 0)
    {
        close(s_udp_fd);
        s_udp_fd = -1;
    }

    s_net_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t tcp_listener_open(uint16_t port)
{
    struct sockaddr_in addr =
    {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    int one = 1;

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    if (s_listen_fd < 0)
    {
        return ESP_FAIL;
    }

    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s_listen_fd, LS_TCP_MAX_CLIENTS) != 0)
    {
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    /* non-blocking accepts: polled from the flusher lap */
    fcntl(s_listen_fd, F_SETFL,
          fcntl(s_listen_fd, F_GETFL, 0) | O_NONBLOCK);
    return ESP_OK;
}

/* ---- bench/CLI line generator --------------------------------------------------------- */

void ls_emit_lines(uint32_t n, uint32_t gap_ms)
{
    for (uint32_t i = 1; i <= n; i++)
    {
        ESP_LOGI(TAG, "LSBENCH %lu/%lu", (unsigned long)i,
                 (unsigned long)n);

        if (gap_ms > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
    }
}

/* ---- lifecycle ------------------------------------------------------------------------- */

esp_err_t log_sinks_init(void)
{
    static const log_descriptor_t LOG_DESC = { "log_sinks", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return ls_settings_register();
}

esp_err_t log_sinks_start(void)
{
    const log_sinks_config_t *cfg = ls_settings_config();

    if (!ls_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running)
    {
        return ESP_OK;
    }

    ls_ring_init(&s_ring[LOG_SINKS_TCP], s_tcp_buf, sizeof(s_tcp_buf));
    ls_ring_init(&s_ring[LOG_SINKS_UDP], s_udp_buf, sizeof(s_udp_buf));
    ls_ring_init(&s_ring[LOG_SINKS_WS], s_ws_buf, sizeof(s_ws_buf));
    ls_ring_init(&s_ring[LOG_SINKS_FILE], s_file_buf, sizeof(s_file_buf));

    for (int i = 0; i < LS_TCP_MAX_CLIENTS; i++)
    {
        s_client_fd[i] = -1;
    }

    s_running = true;

    /* All four sinks register unconditionally (they enumerate in
     * /api/logs/status and the web UI chip row); the settings gates
     * decide which ENGINES run — a gate-off sink's callback is a no-op.
     * PUT /api/logs/sink can pause/resume a running sink (§9.4); it
     * cannot bring up an engine whose gate was off at boot. */
    log_manager_add_sink(&S_TCP_SINK);
    log_manager_add_sink(&S_UDP_SINK);
    log_manager_add_sink(&S_WS_SINK);
    log_manager_add_sink(&S_FILE_SINK);
    log_manager_sink_set_enabled("tcp", cfg->tcp_enabled);
    log_manager_sink_set_enabled("udp", cfg->udp_enabled);
    log_manager_sink_set_enabled("ws", cfg->ws_enabled);
    log_manager_sink_set_enabled("file", cfg->file_enabled);

    bool net_wanted = cfg->tcp_enabled || cfg->udp_enabled ||
                      cfg->ws_enabled;

    if (cfg->tcp_enabled && tcp_listener_open(cfg->tcp_port) != ESP_OK)
    {
        ESP_LOGE(TAG, "tcp tail listener failed on port %u",
                 (unsigned)cfg->tcp_port);
        /* keep going: udp/ws/file may still come up */
    }

    if (net_wanted)
    {
        s_net_task = xTaskCreateStatic(
            net_task, "log_sinks",
            sizeof(s_net_stack) / sizeof(s_net_stack[0]), NULL, 3,
            s_net_stack, &s_net_tcb);
        s_net_up = (s_net_task != NULL);
    }

    if (cfg->file_enabled && ls_file_start() == ESP_OK)
    {
        s_file_up = true;
    }

    if (net_wanted || cfg->file_enabled)
    {
        ESP_LOGI(TAG, "up (tcp=%d port=%u, udp=%d %s:%u, ws=%d, file=%d)",
                 cfg->tcp_enabled, (unsigned)cfg->tcp_port,
                 cfg->udp_enabled, cfg->udp_host,
                 (unsigned)cfg->udp_port, cfg->ws_enabled,
                 cfg->file_enabled);
    }

    return ESP_OK;
}

esp_err_t log_sinks_stop(void)
{
    s_net_up = false;
    s_file_up = false;
    s_running = false;

    if (s_net_task != NULL)
    {
        xTaskNotifyGive(s_net_task);
    }

    for (int i = 0; i < 20 && s_net_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}

/* ---- stats -------------------------------------------------------------------------------- */

esp_err_t log_sinks_stats(log_sinks_id_t id, log_sinks_stats_t *out)
{
    const log_sinks_config_t *cfg = ls_settings_config();

    if (id >= LOG_SINKS_COUNT || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    out->in = s_cnt[id].in;
    out->out = s_cnt[id].out;
    out->dropped = s_cnt[id].dropped;
    out->buffered = ls_ring_count(&s_ring[id]);
    portEXIT_CRITICAL(&s_lock);

    switch (id)
    {
    case LOG_SINKS_TCP:
        out->enabled = cfg->tcp_enabled;
        out->detail = (uint32_t)s_tcp_clients;
        break;
    case LOG_SINKS_UDP:
        out->enabled = cfg->udp_enabled;
        out->detail = s_udp_fail;
        break;
    case LOG_SINKS_WS:
        out->enabled = cfg->ws_enabled;
        out->detail = s_ws_clients;
        break;
    default:
        out->enabled = cfg->file_enabled;
        out->detail = ls_file_rotations();
        break;
    }

    return ESP_OK;
}
