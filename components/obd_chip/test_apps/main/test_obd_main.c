/**
 * @file test_obd_main.c
 * @brief On-target test app for obd_chip against the LIVE bench (chip +
 *        ECU simulator at 500k/11-bit — components/obd_chip_manager/
 *        test_apps/README.md documents the bench).
 *
 * Self-driving markers up to TEST DONE, then:
 *   - a USB bridge (UART2 <-> chip via subscribe/send — the production
 *     passthrough shape) keeps running for pytest/PCAN scenarios
 *   - the console accepts "FWUPDATE" (chip firmware update from the
 *     embedded vendor file — EXCLUSIVE, minutes) and "VERSION"
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "filesystem.h"
#include "log_manager.h"
#include "obd_chip.h"
#include "settings_manager.h"

static const char *TAG = "obd_test";

#define FW_FS_PATH "/data/obd_fw/latest.txt"

/* embedded vendor firmware (V2.3.22) for the update test */
extern const uint8_t fw_txt_start[] asm("_binary_V2_3_22_txt_start");
extern const uint8_t fw_txt_end[] asm("_binary_V2_3_22_txt_end");

/* ---- USB bridge channel B: plain fan-out subscriber + raw send ------------- */

#define BRIDGE_UART UART_NUM_2

static QueueHandle_t s_bridge_q;
static StaticQueue_t s_bridge_q_buf;
static uint8_t s_bridge_q_storage[32 * sizeof(obd_chunk_t)];

static void bridge_chip_to_usb(void *arg)
{
    obd_chunk_t chunk;

    (void)arg;

    while (true)
    {
        if (xQueueReceive(s_bridge_q, &chunk, portMAX_DELAY) == pdTRUE)
        {
            uart_write_bytes(BRIDGE_UART, chunk.data, chunk.len);
        }
    }
}

static void bridge_usb_to_chip(void *arg)
{
    static uint8_t buf[256];

    (void)arg;

    while (true)
    {
        int got = uart_read_bytes(BRIDGE_UART, buf, sizeof(buf),
                                  pdMS_TO_TICKS(20));

        if (got > 0)
        {
            obd_chip_send(buf, (size_t)got);
        }
    }
}

static void bridge_start(void)
{
    uart_config_t cfg =
    {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART, 4096, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART, GPIO_NUM_17, GPIO_NUM_18,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_bridge_q = xQueueCreateStatic(32, sizeof(obd_chunk_t),
                                    s_bridge_q_storage, &s_bridge_q_buf);
    ESP_ERROR_CHECK(obd_chip_subscribe(s_bridge_q, "usb_bridge"));
    xTaskCreate(bridge_chip_to_usb, "br_c2u", 3072, NULL, 10, NULL);
    xTaskCreate(bridge_usb_to_chip, "br_u2c", 3072, NULL, 10, NULL);
}

/* ---- helpers ----------------------------------------------------------------- */

static int count_lines(const char *s)
{
    int lines = (*s != '\0') ? 1 : 0;

    for (; *s != '\0'; s++)
    {
        if (*s == '\r')
        {
            lines++;
        }
    }

    return lines;
}

void app_main(void)
{
    static char resp[1024];
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    /* compose like main */
    ESP_ERROR_CHECK(log_manager_init());
    ESP_ERROR_CHECK(filesystem_init());
    ESP_ERROR_CHECK(settings_manager_init());
    ESP_ERROR_CHECK(log_manager_register_settings());
    err = obd_chip_init();
    printf("INIT ok=%d\n", err == ESP_OK);
    ESP_ERROR_CHECK(settings_manager_start()); /* boot apply pass */
    ESP_ERROR_CHECK(log_manager_start());
    ESP_ERROR_CHECK(filesystem_start());

    err = obd_chip_start(); /* async: launches wake/negotiate/RX bring-up */

    /* bring-up runs on its own task since 2026-07-26 — wait for the
       outcome before asserting (worst case: hard reset + full baud walk
       + first-boot provisioning) */
    for (int i = 0; i < 100 && err == ESP_OK && !obd_chip_ready(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    printf("START ok=%d ready=%d ready_pin=%d\n",
           err == ESP_OK && obd_chip_ready(), obd_chip_ready(),
           obd_chip_status_ok());

    if (err != ESP_OK || !obd_chip_ready())
    {
        printf("TEST DONE\n"); /* fail fast: markers below would all hang */
        return;
    }

    /* ---- request -> response against the real chip ----------------------- */
    err = obd_chip_request("ATI", resp, sizeof(resp), pdMS_TO_TICKS(2000));
    printf("REQ ati_ok=%d elm=%d\n", err == ESP_OK,
           strstr(resp, "ELM327") != NULL);

    err = obd_chip_get_version(resp, sizeof(resp), pdMS_TO_TICKS(3000));
    printf("VERSION ok=%d mic=%d resp=%s\n", err == ESP_OK,
           strstr(resp, "MIC3624") != NULL, resp);

    /* ---- ECU simulator: standard PID ---------------------------------------- */
    err = obd_chip_request("0100", resp, sizeof(resp), pdMS_TO_TICKS(3000));
    printf("PID0100 ok=%d has41=%d\n", err == ESP_OK,
           strstr(resp, "41 00") != NULL || strstr(resp, "4100") != NULL);

    /* ---- canonical multi-line: VIN (mode 09 PID 02, ISO-TP multi-frame) ---- */
    err = obd_chip_request("0902", resp, sizeof(resp), pdMS_TO_TICKS(5000));

    int vin_lines = count_lines(resp);

    printf("VIN ok=%d lines=%d has4902=%d\n", err == ESP_OK, vin_lines,
           strstr(resp, "49 02") != NULL || strstr(resp, "4902") != NULL);

    /* ---- fan-out: a subscriber sees the engine's traffic too ---------------- */
    static StaticQueue_t subq_buf;
    static uint8_t subq_storage[16 * sizeof(obd_chunk_t)];
    QueueHandle_t subq = xQueueCreateStatic(16, sizeof(obd_chunk_t),
                                            subq_storage, &subq_buf);

    ESP_ERROR_CHECK(obd_chip_subscribe(subq, "probe"));
    obd_chip_request("ATI", resp, sizeof(resp), pdMS_TO_TICKS(2000));

    int chunks = 0;
    obd_chunk_t chunk;

    while (xQueueReceive(subq, &chunk, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        chunks++;
    }

    printf("FANOUT chunks=%d gt0=%d\n", chunks, chunks > 0);

    /* ---- slow-consumer drop policy: depth-1 queue + a long response --------- */
    static StaticQueue_t tinyq_buf;
    static uint8_t tinyq_storage[1 * sizeof(obd_chunk_t)];
    QueueHandle_t tinyq = xQueueCreateStatic(1, sizeof(obd_chunk_t),
                                             tinyq_storage, &tinyq_buf);

    ESP_ERROR_CHECK(obd_chip_subscribe(tinyq, "tiny"));
    obd_chip_request("0902", resp, sizeof(resp), pdMS_TO_TICKS(5000));
    printf("DROPS tiny=%lu gt0=%d probe_intact=%d\n",
           (unsigned long)obd_chip_dropped(tinyq),
           obd_chip_dropped(tinyq) > 0,
           obd_chip_dropped(subq) == 0 || chunks > 0);
    ESP_ERROR_CHECK(obd_chip_unsubscribe(tinyq));

    /* ---- claims: monitor blocks commands; release restores ------------------ */
    ESP_ERROR_CHECK(obd_chip_claim(OBD_CHIP_CLAIM_MONITOR, pdMS_TO_TICKS(500)));
    err = obd_chip_request("ATI", resp, sizeof(resp), pdMS_TO_TICKS(500));
    printf("CLAIM blocked=%d\n", err == ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(obd_chip_release());
    err = obd_chip_request("ATI", resp, sizeof(resp), pdMS_TO_TICKS(2000));
    printf("CLAIM released_ok=%d\n", err == ESP_OK);

    /* monitor-class classification + request() refusing them */
    err = obd_chip_request("ATMA", resp, sizeof(resp), pdMS_TO_TICKS(100));
    printf("MONITOR classified=%d refused=%d\n",
           obd_chip_is_monitor_cmd("ATMA"), err == ESP_ERR_NOT_SUPPORTED);

    /* ---- real monitor session: ATMA, stop byte, then commands work ---------- */
    ESP_ERROR_CHECK(obd_chip_claim(OBD_CHIP_CLAIM_MONITOR, pdMS_TO_TICKS(500)));
    obd_chip_send((const uint8_t *)"ATMA\r", 5);
    vTaskDelay(pdMS_TO_TICKS(500)); /* stream (bus may be quiet — fine) */
    obd_chip_monitor_stop();        /* SPACE, never CR */
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(obd_chip_release());
    err = obd_chip_request("ATI", resp, sizeof(resp), pdMS_TO_TICKS(2000));
    printf("ATMA stopped_ok=%d\n", err == ESP_OK);

    /* ---- stage the embedded vendor fw file for the (manual) update test ----- */
    size_t fw_len = (size_t)(fw_txt_end - fw_txt_start);

    if (!filesystem_exists(FW_FS_PATH))
    {
        err = filesystem_write(FW_FS_PATH, fw_txt_start, fw_len);
        printf("FWSTAGE ok=%d bytes=%u\n", err == ESP_OK, (unsigned)fw_len);
    }
    else
    {
        printf("FWSTAGE ok=1 bytes=%u\n", (unsigned)fw_len);
    }

    bridge_start();
    printf("BRIDGE READY\n");
    printf("TEST DONE\n");

    /* ---- console commands (UART0): FWUPDATE / VERSION ----------------------- */
    char line[64];
    size_t n = 0;

    while (true)
    {
        int c = fgetc(stdin);

        if (c == EOF)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c != '\n' && c != '\r')
        {
            if (n < sizeof(line) - 1)
            {
                line[n++] = (char)c;
            }

            continue;
        }

        line[n] = '\0';
        n = 0;

        if (strcmp(line, "FWUPDATE") == 0 || strcmp(line, "FWUPDATE FORCE") == 0)
        {
            bool force = (strstr(line, "FORCE") != NULL);

            printf("FWUPDATE starting (force=%d) — several minutes\n", force);
            err = obd_chip_firmware_update(FW_FS_PATH, force);
            printf("FWUPDATE done err=%s\n", esp_err_to_name(err));

            /* chip rebooted: re-probe through the normal engine */
            vTaskDelay(pdMS_TO_TICKS(1000));

            if (obd_chip_get_version(resp, sizeof(resp),
                                     pdMS_TO_TICKS(5000)) == ESP_OK)
            {
                printf("FWVERSION %s\n", resp);
            }
            else
            {
                printf("FWVERSION unavailable\n");
            }
        }
        else if (strcmp(line, "VERSION") == 0)
        {
            if (obd_chip_get_version(resp, sizeof(resp),
                                     pdMS_TO_TICKS(5000)) == ESP_OK)
            {
                printf("FWVERSION %s\n", resp);
            }
        }
        else if (n == 0 && line[0] != '\0')
        {
            ESP_LOGW(TAG, "unknown console command '%s'", line);
        }
    }
}
