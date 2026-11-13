/**
 * @file test_lifecycle.c
 * @brief On-target test app for microlink's stop and destroy. The production
 *        init, stop, start and destroy against the real kernel, with one
 *        task held in the client's relay-task slot: a stop that times out
 *        keeps the task's handle, a start is refused, a destroy is deferred,
 *        and a destroy after the task has ended frees the client. Two real
 *        30-second deadlines, no network. See ../README.md.
 */
#include <stdio.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "unity.h"

#include "microlink_internal.h"

static microlink_t *s_client;
static TaskHandle_t s_held_task;
static StaticTask_t s_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_stack[4096];

static void held_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelete(NULL);
}

void setUp(void)
{
}

void tearDown(void)
{
    if (s_held_task != NULL)
    {
        if (eTaskGetState(s_held_task) != eDeleted)
        {
            vTaskDelete(s_held_task);
        }
        s_held_task = NULL;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_client != NULL)
    {
        s_client->derp_tx_task = NULL;
        TEST_ASSERT_EQUAL(ESP_OK, microlink_destroy(s_client));
        s_client = NULL;
    }
}

static void test_timed_out_task_stays_owned(void)
{
    /* init refuses a config without an auth key. This one is never sent:
     * the held task below makes start refuse before any network use. */
    const microlink_config_t config =
    {
        .auth_key = "lifecycle-test-not-a-key",
        .device_name = "lifecycle-test",
    };
    s_client = microlink_init(&config);
    TEST_ASSERT_NOT_NULL(s_client);
    s_held_task = xTaskCreateStatic(held_task, "ml_hold", sizeof(s_stack),
                                  NULL, 4, s_stack, &s_tcb);
    TEST_ASSERT_NOT_NULL(s_held_task);
    s_client->derp_tx_task = s_held_task;

    puts("MICROLINK TEST: stop must retain a live task at its 30-second deadline");
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, microlink_stop(s_client));
    TEST_ASSERT_EQUAL_PTR(s_held_task, s_client->derp_tx_task);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, microlink_start(s_client));

    puts("MICROLINK TEST: a second stop through destroy must not forget that task");
    esp_err_t err = microlink_destroy(s_client);
    if (err == ESP_OK)
    {
        s_client = NULL;
    }
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, err);
    TEST_ASSERT_EQUAL_PTR(s_held_task, s_client->derp_tx_task);

    xTaskNotifyGive(s_held_task);
    while (eTaskGetState(s_held_task) != eDeleted)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_held_task = NULL;
    vTaskDelay(pdMS_TO_TICKS(20));
    TEST_ASSERT_EQUAL(ESP_OK, microlink_destroy(s_client));
    s_client = NULL;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    UNITY_BEGIN();
    RUN_TEST(test_timed_out_task_stays_owned);
    if (UNITY_END() == 0)
    {
        puts("TEST DONE");
    }
    else
    {
        puts("TEST FAILED");
    }
}
