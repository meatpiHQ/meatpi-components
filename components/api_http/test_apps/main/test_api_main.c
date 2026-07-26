/**
 * @file test_api_main.c
 * @brief On-target test app for api_http. Composes the full core stack the
 *        way main will (log/restart/dev_status/filesystem/settings/http
 *        server + the api_http glue), serves on the lwIP loopback, and
 *        asserts every /api area with esp_http_client against 127.0.0.1.
 *
 * TWO-PHASE (like restart_tracker's suite): phase 1 exercises the read/write
 * endpoints, then PUTs a real settings change and POSTs /api/settings/submit
 * — which REBOOTS the device via restart_tracker_restart(CONFIG_APPLY,
 * CONFIG_SERVER). Phase 2 (marker file present) verifies the change
 * persisted across the reboot and that the restart history records the
 * planned config_apply/config_server reboot, then prints TEST DONE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "api_http.h"
#include "dev_status_manager.h"
#include "filesystem.h"
#include "http_server_manager.h"
#include "log_manager.h"
#include "restart_tracker.h"
#include "settings_manager.h"

#define PHASE_MARKER "/data/api_test_phase"

/* ---- a test settings component: proves redaction + persistence ------------- */

static const settings_field_t API_TEST_FIELDS[] =
{
    SETTINGS_INT("value", 0, 100, 1),
    SETTINGS_STR("api_password", 32, "topsecret"),
};

static esp_err_t api_test_apply(const cJSON *settings)
{
    (void)settings;
    return ESP_OK;
}

static const settings_descriptor_t API_TEST_DESC =
{
    .name = "api_test",
    .version = 1,
    .fields = API_TEST_FIELDS,
    .field_count = sizeof(API_TEST_FIELDS) / sizeof(API_TEST_FIELDS[0]),
    .on_apply = api_test_apply,
};

/* ---- tiny client helper ------------------------------------------------------ */

static char s_body[4096];

static int http_req(esp_http_client_method_t method, const char *path,
                    const char *req_body)
{
    char url[96];

    snprintf(url, sizeof(url), "http://127.0.0.1%s", path);

    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);

    if (c == NULL)
    {
        return -1;
    }

    esp_http_client_set_method(c, method);

    size_t req_len = (req_body != NULL) ? strlen(req_body) : 0;
    int status = -1;

    if (req_len > 0)
    {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }

    if (esp_http_client_open(c, (int)req_len) == ESP_OK)
    {
        if (req_len > 0)
        {
            esp_http_client_write(c, req_body, (int)req_len);
        }

        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);

        int n = esp_http_client_read_response(c, s_body,
                                              (int)sizeof(s_body) - 1);

        s_body[(n > 0) ? n : 0] = '\0';
    }

    esp_http_client_cleanup(c);
    return status;
}

/* ---- phases ------------------------------------------------------------------- */

static void phase1(void)
{
    int st;

    /* settings surface */
    st = http_req(HTTP_METHOD_GET, "/api/settings", NULL);
    printf("SETTINGS-LIST ok=%d has_api_test=%d\n", st == 200,
           strstr(s_body, "\"api_test\"") != NULL);

    st = http_req(HTTP_METHOD_GET, "/api/settings/api_test", NULL);
    printf("SETTINGS-GET ok=%d degraded0=%d redacted=%d\n", st == 200,
           strstr(s_body, "\"degraded\":false") != NULL,
           strstr(s_body, "\"api_password\":\"\"") != NULL &&
               strstr(s_body, "topsecret") == NULL);

    st = http_req(HTTP_METHOD_GET, "/api/settings/api_test/schema", NULL);
    printf("SCHEMA ok=%d has_props=%d\n", st == 200,
           strstr(s_body, "\"properties\"") != NULL);

    st = http_req(HTTP_METHOD_GET, "/api/settings/nope", NULL);
    printf("SETTINGS-404 ok=%d\n", st == 404);

    st = http_req(HTTP_METHOD_PUT, "/api/settings/api_test",
                  "{\"value\":999,\"api_password\":\"\"}");
    printf("PUT-BAD rejected=%d has_err=%d\n", st == 400,
           strstr(s_body, "\"error\"") != NULL);

    /* status + history */
    dev_status_manager_set(DEV_STATUS_BIT_AWAKE);
    st = http_req(HTTP_METHOD_GET, "/api/status", NULL);
    printf("STATUS ok=%d awake=%d uptime=%d boots=%d\n", st == 200,
           strstr(s_body, "\"awake\":true") != NULL,
           strstr(s_body, "\"uptime\":\"") != NULL,
           strstr(s_body, "\"boot_count\":") != NULL);

    st = http_req(HTTP_METHOD_GET, "/api/restart/history", NULL);
    printf("HISTORY ok=%d has_records=%d\n", st == 200,
           strstr(s_body, "\"records\":[{") != NULL);

    /* logs runtime knobs */
    st = http_req(HTTP_METHOD_GET, "/api/logs/status", NULL);
    printf("LOGS-STATUS ok=%d has_console=%d has_ring=%d\n", st == 200,
           strstr(s_body, "\"console\"") != NULL,
           strstr(s_body, "\"ring\"") != NULL);

    st = http_req(HTTP_METHOD_PUT, "/api/logs/level",
                  "{\"tag\":\"api_http\",\"level\":\"debug\"}");
    printf("LOGS-LEVEL ok=%d\n", st == 200);
    log_manager_set_level("api_http", ESP_LOG_INFO); /* restore */

    st = http_req(HTTP_METHOD_PUT, "/api/logs/level",
                  "{\"tag\":\"api_http\",\"level\":\"loud\"}");
    printf("LOGS-LEVEL-BAD rejected=%d\n", st == 400);

    st = http_req(HTTP_METHOD_PUT, "/api/logs/sink",
                  "{\"name\":\"nope\",\"enabled\":false}");
    printf("LOGS-SINK-404 ok=%d\n", st == 404);

    st = http_req(HTTP_METHOD_GET, "/api/logs/ring", NULL);
    printf("LOGS-RING ok=%d bytes_gt0=%d\n", st == 200, s_body[0] != '\0');

    /* filesystem browse */
    filesystem_write("/data/web/probe.txt", "x", 1);
    st = http_req(HTTP_METHOD_GET, "/api/fs/list?path=/data/web", NULL);
    printf("FS-LIST ok=%d has_probe=%d\n", st == 200,
           strstr(s_body, "\"probe.txt\"") != NULL);

    st = http_req(HTTP_METHOD_GET, "/api/fs/list?path=/data/../etc", NULL);
    printf("FS-LIST-BAD rejected=%d\n", st != 200);

    st = http_req(HTTP_METHOD_GET, "/api/fs/info?path=/data", NULL);
    printf("FS-INFO ok=%d has_total=%d\n", st == 200,
           strstr(s_body, "\"total\":") != NULL);

    /* the real settings change: value 1 -> 7, password "" = keep stored */
    st = http_req(HTTP_METHOD_PUT, "/api/settings/api_test",
                  "{\"value\":7,\"api_password\":\"\"}");
    printf("PUT-OK ok=%d changed=%d\n", st == 200,
           strstr(s_body, "\"changed\":true") != NULL);

    /* identical PUT: unredact must map "" back to the stored secret, so
       the write dedups — changed=false proves the keep-stored semantics */
    st = http_req(HTTP_METHOD_PUT, "/api/settings/api_test",
                  "{\"value\":7,\"api_password\":\"\"}");
    printf("PUT-KEEP noop=%d\n", st == 200 &&
           strstr(s_body, "\"changed\":false") != NULL);

    /* mark phase 2, then submit-with-changes => {"reboot":true} + reboot */
    ESP_ERROR_CHECK(filesystem_write(PHASE_MARKER, "1", 1));
    st = http_req(HTTP_METHOD_POST, "/api/settings/submit", NULL);
    printf("SUBMIT ok=%d reboot=%d\n", st == 200,
           strstr(s_body, "\"reboot\":true") != NULL);
    printf("REBOOTING (config_apply via restart_tracker)\n");
    /* the api_http reboot task fires in ~1 s */
}

static void phase2(void)
{
    int st;

    /* the PUT persisted across the reboot and the boot pass applied it */
    st = http_req(HTTP_METHOD_GET, "/api/settings/api_test", NULL);
    printf("PHASE2-PERSISTED ok=%d value7=%d\n", st == 200,
           strstr(s_body, "\"value\":7") != NULL);

    /* the reboot is in the history as planned config_apply/config_server */
    st = http_req(HTTP_METHOD_GET, "/api/restart/history", NULL);
    printf("PHASE2-HISTORY ok=%d planned=%d reason=%d source=%d\n",
           st == 200,
           strstr(s_body, "\"planned\":true") != NULL,
           strstr(s_body, "\"config_apply\"") != NULL,
           strstr(s_body, "\"config_server\"") != NULL);

    /* a no-op submit never reboots */
    st = http_req(HTTP_METHOD_POST, "/api/settings/submit", NULL);
    printf("PHASE2-NOOP-SUBMIT ok=%d noreboot=%d\n", st == 200,
           strstr(s_body, "\"reboot\":false") != NULL);

    /* clean re-runs: restore defaults + drop the marker */
    cJSON *reset = cJSON_CreateObject();

    cJSON_AddNumberToObject(reset, "value", 1);
    cJSON_AddStringToObject(reset, "api_password", "topsecret");
    settings_manager_set("api_test", reset, NULL, 0, NULL);
    cJSON_Delete(reset);
    filesystem_delete(PHASE_MARKER);

    printf("TEST DONE\n");
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

    /* ---- compose like main (ARCHITECTURE §11 boot order) ------------------ */
    ESP_ERROR_CHECK(log_manager_init());
    ESP_ERROR_CHECK(restart_tracker_init());
    ESP_ERROR_CHECK(dev_status_manager_init());
    ESP_ERROR_CHECK(filesystem_init());
    ESP_ERROR_CHECK(filesystem_start());
    ESP_ERROR_CHECK(settings_manager_init());
    ESP_ERROR_CHECK(log_manager_register_settings());
    ESP_ERROR_CHECK(settings_manager_register(&API_TEST_DESC));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(http_server_manager_init());

    err = api_http_init();
    printf("INIT ok=%d\n", err == ESP_OK);

    ESP_ERROR_CHECK(settings_manager_start()); /* boot pass: on_apply */
    ESP_ERROR_CHECK(log_manager_start());
    ESP_ERROR_CHECK(http_server_manager_start());
    ESP_ERROR_CHECK(api_http_start());

    if (filesystem_exists(PHASE_MARKER))
    {
        printf("PHASE 2 (after submit reboot)\n");
        phase2();
    }
    else
    {
        printf("PHASE 1\n");
        phase1();
    }
}
