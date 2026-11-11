/**
 * @file test_kline.c
 * @brief The request engine on a K-line (ISO 9141-2, ISO 14230-4): the
 *        production obd_chip_cmd.c and obd_chip_kline.c against the fake
 *        chip of test_request.c with a K-line ECU behind it.
 *
 * The ECU and the chip as the bench showed them (2026-11-12, the MIC3624
 * V2.3.24 against ECU sim v2): the first request behind a closed protocol
 * starts the bus (`BUS INIT: ` at once, `...OK` and the reply 2.6 s later,
 * `...ERROR` when nobody is there); the ECU ends the session after five
 * seconds without a request and the chip, which does not notice, answers
 * `NO DATA` from then on; `ATPC` closes the protocol, so the next request
 * starts the bus again. Reproduced first (GitHub issue #1045): a request
 * with a 1.5 s timeout was cut in the middle of the start, 3 of 3.
 */
#include "obd_chip.h"

#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

extern bool (*test_chip_hook)(const char *line, const char **reply);
void test_fake_chip_reset(void);

#define PROMPT   "\r\r>"
#define RPM      "48 6B 10 41 0C 0C 80 9C "
#define T_AT     pdMS_TO_TICKS(500)
#define T_SHORT  pdMS_TO_TICKS(60)
#define LATE_MS  180

/* ---- the K-line ECU behind the fake chip --------------------------------- */

static bool s_ecu;          /* an ECU is on the bus                          */
static bool s_chip_open;    /* the chip has the protocol open                */
static bool s_session;      /* the ECU still has its session                 */
static bool s_slow;         /* the start as the chip prints it: late          */
static int  s_inits;        /* bus starts tried                              */
static char s_seen[256];    /* the words the chip got, blank between them    */
static char s_late[96];     /* what the chip prints LATE_MS later            */

static void late_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(LATE_MS));
    obd_core_fanout((const uint8_t *)s_late, strlen(s_late));
    vTaskDelete(NULL);
}

static void later(const char *text)
{
    snprintf(s_late, sizeof(s_late), "%s", text);
    xTaskCreate(late_task, "late", 4096, NULL, 5, NULL);
}

static bool kline_chip(const char *line, const char **reply)
{
    size_t n = strlen(s_seen);

    snprintf(s_seen + n, sizeof(s_seen) - n, "%s%s", (n > 0) ? " " : "", line);
    *reply = "\rOK" PROMPT;

    if (strcmp(line, "ATPC") == 0)
    {
        s_chip_open = false;
        return true;
    }

    if (strncmp(line, "ATTP", 4) == 0)
    {
        if (line[4] != '3')
        {
            s_chip_open = false;    /* another protocol: the K-line is left */
        }

        return true;
    }

    if (strncmp(line, "AT", 2) == 0)
    {
        return true;
    }

    if (strcmp(line, "0105") == 0)
    {
        /* an answer that comes late with nothing said meanwhile */
        *reply = NULL;
        later("\r48 6B 10 41 05 7E 88 " PROMPT);
        return true;
    }

    if (s_chip_open)
    {
        /* the chip sends the request whatever the ECU thinks of the session;
           a request the ECU does not know is answered with silence too */
        *reply = (s_session && strcmp(line, "0199") != 0)
                     ? "\r" RPM PROMPT : "\rNO DATA" PROMPT;
        return true;
    }

    s_inits++;

    if (!s_ecu)
    {
        *reply = "\rBUS INIT: ...ERROR" PROMPT;
        return true;
    }

    s_chip_open = true;
    s_session = true;

    if (s_slow)
    {
        *reply = "\rBUS INIT: ";
        later("...OK\r" RPM PROMPT);
    }
    else
    {
        *reply = "\rBUS INIT: ...OK\r" RPM PROMPT;
    }

    return true;
}

/** A fresh chip on protocol 3 with a K-line car behind it. */
static void car(void)
{
    char resp[64];

    TEST_ASSERT_EQUAL(ESP_OK, obd_cmd_engine_init());
    test_fake_chip_reset();
    obd_cmd_memory_reset();         /* as a bring-up leaves the engine */
    s_ecu = true;
    s_chip_open = false;
    s_session = false;
    s_slow = false;
    s_inits = 0;
    test_chip_hook = kline_chip;
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATTP3", resp, sizeof(resp), T_AT));
    s_seen[0] = '\0';
}

/** The ECU's five seconds pass: @p ms later, nothing asked meanwhile. */
static void wait_ms(uint32_t ms)
{
    obd_cmd_test_kline_age(ms);

    obd_kline_t k;

    obd_cmd_kline_stats(&k);

    if (obd_chip_kline_idle_ms() >= 5000 || !k.up)
    {
        s_session = false;
    }
}

static obd_kline_t stats(void)
{
    obd_kline_t k;

    obd_cmd_kline_stats(&k);
    return k;
}

/* ---- the tests ----------------------------------------------------------- */

/* The reproduction: a request with a short timeout meets a bus that is
 * being started. The chip has said `BUS INIT: ` and nothing else when the
 * caller's time is up. */
void test_kline_request_that_starts_the_bus_gets_the_start_time(void)
{
    char resp[96] = "";

    car();
    s_slow = true;

    uint32_t starts0 = stats().starts;
    TickType_t t0 = xTaskGetTickCount();

    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_SHORT));
    TEST_ASSERT_TRUE(xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(LATE_MS - 20));
    TEST_ASSERT_NOT_NULL(strstr(resp, "BUS INIT: ...OK"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "48 6B 10 41 0C 0C 80 9C"));
    TEST_ASSERT_TRUE(stats().up);
    TEST_ASSERT_EQUAL_UINT32(1, stats().starts - starts0);

    /* a later request keeps the caller's own time: an answer that is late
       with nothing said meanwhile is a timeout, as it always was */
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, obd_chip_request("0105", resp, sizeof(resp), T_SHORT));
    vTaskDelay(pdMS_TO_TICKS(LATE_MS + 60)); /* the late answer is out */
    test_chip_hook = NULL;
}

void test_kline_text_that_means_a_bus_start(void)
{
    TEST_ASSERT_TRUE(obd_parse_bus_starting("\rBUS INIT: ", 11));
    TEST_ASSERT_TRUE(obd_parse_bus_starting("BUS INIT: ...", 13));
    TEST_ASSERT_TRUE(obd_parse_bus_starting("010C1\rBUS INIT: .", 17)); /* echo on */
    TEST_ASSERT_FALSE(obd_parse_bus_starting("", 0));
    TEST_ASSERT_FALSE(obd_parse_bus_starting(NULL, 0));
    TEST_ASSERT_FALSE(obd_parse_bus_starting("SEARCHING...", 12));
    TEST_ASSERT_FALSE(obd_parse_bus_starting("BUS BUSY", 8));
    TEST_ASSERT_FALSE(obd_parse_bus_starting("BUS INI", 7)); /* not all of it yet */
    TEST_ASSERT_FALSE(obd_parse_bus_starting("48 6B 10 41 0C", 14));
}

/* A session the ECU ended: the chip prints NO DATA for every request from
 * then on. The engine closes the protocol first, the request starts the bus. */
void test_kline_idle_session_is_closed_ahead_of_the_request(void)
{
    char resp[96] = "";

    car();

    obd_kline_t k0 = stats();

    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("010C1", s_seen);
    TEST_ASSERT_EQUAL(1, s_inits);

    /* asked again in time: the session is used as it is */
    wait_ms(4000);
    s_seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("010C1", s_seen);
    TEST_ASSERT_NOT_NULL(strstr(resp, "41 0C"));
    TEST_ASSERT_EQUAL(1, s_inits);

    /* five seconds without a request */
    wait_ms(5000);
    s_seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATPC 010C1", s_seen);
    TEST_ASSERT_NOT_NULL(strstr(resp, "BUS INIT: ...OK"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "41 0C"));
    TEST_ASSERT_EQUAL(2, s_inits);
    TEST_ASSERT_EQUAL_UINT32(1, stats().closed - k0.closed);
    TEST_ASSERT_EQUAL_UINT32(2, stats().starts - k0.starts);
    TEST_ASSERT_TRUE(obd_chip_kline_idle_ms() < 1000);

    /* a word that only sets the chip never closes anything */
    wait_ms(6000);
    s_seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATH1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATH1", s_seen);
    test_chip_hook = NULL;
}

/* A request the ECU does not know is answered with silence on a K-line: it
 * ends no session, and it is no sign of life either. */
void test_kline_no_data_in_a_live_session_is_left_alone(void)
{
    char resp[96] = "";

    car();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    wait_ms(3000);
    s_seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("0199", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("NO DATA", resp);
    TEST_ASSERT_EQUAL_STRING("0199", s_seen);       /* no close, no restart */
    TEST_ASSERT_TRUE(stats().up);
    TEST_ASSERT_TRUE(obd_chip_kline_idle_ms() >= 3000); /* the clock stood */

    /* two more seconds: five since the bus last answered */
    wait_ms(2000);
    s_seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATPC 010C1", s_seen);
    TEST_ASSERT_NOT_NULL(strstr(resp, "41 0C"));
    test_chip_hook = NULL;
}

void test_kline_another_protocol_ends_the_session_the_same_one_does_not(void)
{
    char resp[96] = "";

    car();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_TRUE(stats().up);

    /* the prelude's own word, before every row type: the session stays */
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATTP3", resp, sizeof(resp), T_AT));
    TEST_ASSERT_TRUE(stats().up);
    wait_ms(5000);
    s_seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATPC 010C1", s_seen);

    /* a CAN protocol: no K-line session, and no `ATPC` ahead of its requests
       however long they wait */
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATTP6", resp, sizeof(resp), T_AT));
    TEST_ASSERT_FALSE(stats().up);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, obd_chip_kline_idle_ms());
    s_seen[0] = '\0';
    (void)obd_chip_request("0100", resp, sizeof(resp), T_AT);
    TEST_ASSERT_NULL(strstr(s_seen, "ATPC"));

    /* the reset words and the close itself end it too */
    static const char *const ENDS[] = { "ATPC", "ATWS", "ATD", "at z" };

    for (size_t i = 0; i < sizeof(ENDS) / sizeof(ENDS[0]); i++)
    {
        car();
        TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
        TEST_ASSERT_TRUE_MESSAGE(stats().up, ENDS[i]);
        (void)obd_chip_request(ENDS[i], resp, sizeof(resp), T_AT);
        TEST_ASSERT_FALSE_MESSAGE(stats().up, ENDS[i]);
    }

    test_chip_hook = NULL;
}

void test_kline_nobody_there_is_no_session(void)
{
    char resp[96] = "";

    car();
    s_ecu = false;

    uint32_t failed0 = stats().failed;

    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("BUS INIT: ...ERROR", resp);
    TEST_ASSERT_FALSE(stats().up);
    TEST_ASSERT_EQUAL_UINT32(1, stats().failed - failed0);

    /* the chip tries again by itself at the next request: nothing to close */
    obd_cmd_test_kline_age(20000);
    s_seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("010C1", s_seen);
    TEST_ASSERT_EQUAL(2, s_inits);

    /* a fresh chip (the bring-up) knows no session */
    s_ecu = true;
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("010C1", resp, sizeof(resp), T_AT));
    TEST_ASSERT_TRUE(stats().up);
    obd_cmd_memory_reset();
    TEST_ASSERT_FALSE(stats().up);
    test_chip_hook = NULL;
}

/* The rule alone, on the texts the chip prints. */
void test_kline_rule_on_the_chips_texts(void)
{
    obd_kline_t k;

    memset(&k, 0, sizeof(k));
    obd_kline_word(&k, "ATTP4");
    obd_kline_reply(&k, "0100", "BUS INIT: ...OK\r86 F1 10 41 00 18 3F 80 03 A2 ", 1000000);
    TEST_ASSERT_TRUE(k.up);
    TEST_ASSERT_EQUAL('4', k.proto);

    /* what is no answer of the bus: the clock stands */
    static const char *const SILENT[] =
    {
        "NO DATA", "FB ERROR", "FC RX TIMEOUT", "?", "OK", "STOPPED",
        "BUS BUSY", "DATA ERROR", "BUFFER FULL", "",
    };

    for (size_t i = 0; i < sizeof(SILENT) / sizeof(SILENT[0]); i++)
    {
        obd_kline_reply(&k, "010C1", SILENT[i], 3000000);
        TEST_ASSERT_EQUAL_MESSAGE(1000000, (int)k.last_us, SILENT[i]);
    }

    /* what is: headers on and off, and the rows of a long reply */
    static const char *const DATA[] =
    {
        "84 F1 10 41 0C 0C 80 5E ", "41 0C 0C 80 ", "48 6B 10 41 0C 0C 80 9C \r",
        "014\r0: 49 02 01 31 57 43\r1: 41 4E 30 46 57 30 50",
    };

    for (size_t i = 0; i < sizeof(DATA) / sizeof(DATA[0]); i++)
    {
        k.last_us = 1000000;
        obd_kline_reply(&k, "010C1", DATA[i], 2000000);
        TEST_ASSERT_EQUAL_MESSAGE(2000000, (int)k.last_us, DATA[i]);
    }

    /* an AT word's answer is no sign of the bus */
    k.last_us = 1000000;
    obd_kline_reply(&k, "ATDPN", "4", 2000000);
    TEST_ASSERT_EQUAL(1000000, (int)k.last_us);

    /* stale by the clock, for a bus request only, and never for the two
       words that start the bus themselves */
    TEST_ASSERT_FALSE(obd_kline_stale(&k, "010C1", 1000000 + 4400000));
    TEST_ASSERT_TRUE(obd_kline_stale(&k, "010C1", 1000000 + 4600000));
    TEST_ASSERT_TRUE(obd_kline_stale(&k, "03", 1000000 + 60000000));
    TEST_ASSERT_FALSE(obd_kline_stale(&k, "ATRV", 1000000 + 60000000));
    TEST_ASSERT_FALSE(obd_kline_stale(&k, "ATSI", 1000000 + 60000000));
    TEST_ASSERT_FALSE(obd_kline_stale(&k, "at fi", 1000000 + 60000000));
    TEST_ASSERT_EQUAL_UINT32(4600, obd_kline_idle_ms(&k, 1000000 + 4600000));

    /* `ATSI` alone starts the bus: `BUS INIT: ...OK` and no data */
    obd_kline_reset(&k);
    obd_kline_word(&k, "ATTP3");
    obd_kline_reply(&k, "ATSI", "BUS INIT: ...OK", 5000000);
    TEST_ASSERT_TRUE(k.up);
    TEST_ASSERT_EQUAL(5000000, (int)k.last_us);

    /* a session that came up before any word named a protocol (the chip on
       its stored one) is taken for the K-line protocol named first, and
       ended by any other */
    obd_kline_reset(&k);
    obd_kline_reply(&k, "0100", "BUS INIT: OK\r86 F1 10 41 00 18 3F 80 03 A2 ", 1);
    TEST_ASSERT_TRUE(k.up);
    obd_kline_word(&k, "ATTP5");
    TEST_ASSERT_TRUE(k.up);
    obd_kline_word(&k, "ATSP3");
    TEST_ASSERT_FALSE(k.up);

    /* the STN and vendor presets are another bus, whichever */
    obd_kline_reset(&k);
    obd_kline_word(&k, "ATTP3");
    obd_kline_reply(&k, "0100", "BUS INIT: ...OK\r48 6B 10 41 00 18 3F 80 03 DE ", 1);
    TEST_ASSERT_TRUE(k.up);
    obd_kline_word(&k, "STP 53");
    TEST_ASSERT_FALSE(k.up);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, obd_kline_idle_ms(&k, 2));
}
