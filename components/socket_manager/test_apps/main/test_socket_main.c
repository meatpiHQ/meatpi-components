/**
 * @file test_socket_main.c
 * @brief On-target test app for socket_manager. Self-contained on the lwIP
 *        loopback: persists a test server set (TCP tcp0:3333 max 2 clients +
 *        UDP udp0:17) BEFORE the settings boot pass (reboot-to-apply without
 *        a reboot: set() then settings_manager_start()), then drives real
 *        BSD-socket clients on 127.0.0.1 and asserts via serial markers.
 *
 * Covers: TCP RX into the subscriber queue, TX to a client, multi-client
 * aggregate semantics (fan-out TX to 2 clients, merged RX), max_clients
 * accept-then-close + refused counter, abrupt-close reaping, UDP last-peer
 * RX/TX, and stats consistency.
 */
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "settings_manager.h"
#include "socket_manager.h"

static const char *TAG = "sock_test";

static StaticQueue_t s_q_buf; /* internal: FreeRTOS object */
static uint8_t s_q_store[16 * sizeof(socket_chunk_t)];
static QueueHandle_t s_q;

static int connect_tcp(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a =
    {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };

    a.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        close(fd);
        return -1;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/** Drain the subscriber queue into buf; returns bytes gathered. */
static int gather(char *buf, size_t buf_len, uint32_t wait_ms)
{
    socket_chunk_t chunk;
    int total = 0;

    while (xQueueReceive(s_q, &chunk, pdMS_TO_TICKS(wait_ms)) == pdTRUE)
    {
        if (total + chunk.len < (int)buf_len)
        {
            memcpy(buf + total, chunk.data, chunk.len);
            total += chunk.len;
        }

        wait_ms = 100; /* keep draining briefly after the first chunk */
    }

    buf[total] = '\0';
    return total;
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
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ---- compose like main -------------------------------------------- */
    ESP_ERROR_CHECK(settings_manager_init());
    err = socket_manager_init();
    printf("INIT ok=%d\n", err == ESP_OK);

    /* persist the TEST server set BEFORE the boot pass applies it */
    cJSON *cfg = cJSON_Parse(
        "{\"servers\":["
        "{\"name\":\"tcp0\",\"proto\":\"tcp\",\"port\":3333,"
         "\"max_clients\":2,\"enabled\":true},"
        "{\"name\":\"udp0\",\"proto\":\"udp\",\"port\":17,"
         "\"enabled\":true}]}");
    char errbuf[96] = "";

    err = settings_manager_set("socket_manager", cfg, errbuf, sizeof(errbuf),
                               NULL);
    cJSON_Delete(cfg);
    printf("CFG ok=%d err='%s'\n", err == ESP_OK, errbuf);

    ESP_ERROR_CHECK(settings_manager_start()); /* boot pass -> on_apply */

    err = socket_manager_start();
    printf("START ok=%d\n", err == ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(500)); /* listeners come up in the net task */

    s_q = xQueueCreateStatic(16, sizeof(socket_chunk_t), s_q_store, &s_q_buf);
    err = socket_manager_subscribe("tcp0", s_q);
    printf("SUB ok=%d dup_rejected=%d\n", err == ESP_OK,
           socket_manager_subscribe("tcp0", s_q) == ESP_ERR_INVALID_STATE);

    char buf[256];

    /* ---- TCP single client: RX + TX ----------------------------------- */
    int a = connect_tcp(3333);

    vTaskDelay(pdMS_TO_TICKS(300));
    send(a, "hello-up", 8, 0);

    int got = gather(buf, sizeof(buf), 1000);

    printf("TCP-RX ok=%d match=%d\n", got == 8, strcmp(buf, "hello-up") == 0);

    socket_manager_send("tcp0", (const uint8_t *)"hello-down", 10);

    int n = recv(a, buf, sizeof(buf) - 1, 0);

    buf[(n > 0) ? n : 0] = '\0';
    printf("TCP-TX ok=%d match=%d\n", n == 10,
           strcmp(buf, "hello-down") == 0);

    /* ---- second client: fan-out TX + merged RX ------------------------- */
    int b = connect_tcp(3333);

    vTaskDelay(pdMS_TO_TICKS(300));
    socket_manager_send("tcp0", (const uint8_t *)"both", 4);

    char bufb[64];
    int na = recv(a, buf, sizeof(buf) - 1, 0);
    int nb = recv(b, bufb, sizeof(bufb) - 1, 0);

    printf("FANOUT ok=%d\n", na == 4 && nb == 4);

    send(b, "from-b", 6, 0);
    got = gather(buf, sizeof(buf), 1000);
    printf("MERGE ok=%d\n", got == 6 && strcmp(buf, "from-b") == 0);

    /* ---- max_clients: third connection is accept-then-closed ----------- */
    int c = connect_tcp(3333);
    int nc = (c >= 0) ? recv(c, buf, sizeof(buf), 0) : -1;

    socket_stats_t stats;

    socket_manager_stats("tcp0", &stats);
    printf("MAXCLIENTS closed=%d refused=%u\n", nc == 0,
           (unsigned)stats.refused);

    if (c >= 0)
    {
        close(c);
    }

    /* ---- abrupt close: B disappears, A keeps working ------------------- */
    close(b);
    vTaskDelay(pdMS_TO_TICKS(300));
    socket_manager_send("tcp0", (const uint8_t *)"still-there", 11);
    n = recv(a, buf, sizeof(buf) - 1, 0);
    socket_manager_stats("tcp0", &stats);
    printf("REAP ok=%d clients=%u\n", n == 11, (unsigned)stats.clients);

    /* ---- UDP: last-peer semantics --------------------------------------- */
    socket_manager_unsubscribe("tcp0", s_q);
    ESP_ERROR_CHECK(socket_manager_subscribe("udp0", s_q));

    /* TX before any peer is known must refuse */
    err = socket_manager_send("udp0", (const uint8_t *)"x", 1);
    printf("UDP-NOPEER refused=%d\n", err == ESP_ERR_INVALID_STATE);

    int u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in ua =
    {
        .sin_family = AF_INET,
        .sin_port = htons(17),
    };

    ua.sin_addr.s_addr = inet_addr("127.0.0.1");

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sendto(u, "ping", 4, 0, (struct sockaddr *)&ua, sizeof(ua));
    got = gather(buf, sizeof(buf), 1000);
    printf("UDP-RX ok=%d\n", got == 4 && strcmp(buf, "ping") == 0);

    socket_manager_send("udp0", (const uint8_t *)"pong", 4);
    n = recv(u, buf, sizeof(buf) - 1, 0);
    printf("UDP-LASTPEER ok=%d\n", n == 4);

    close(u);
    close(a);

    socket_manager_stats("tcp0", &stats);
    printf("STATS in=%u out=%u rx_drops=%u\n", (unsigned)stats.bytes_in,
           (unsigned)stats.bytes_out, (unsigned)stats.rx_drops);

    ESP_LOGI(TAG, "suite complete");
    printf("TEST DONE\n");
}
