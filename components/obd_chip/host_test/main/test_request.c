/**
 * @file test_request.c
 * @brief The request engine against a fake chip: the production
 *        obd_chip_cmd.c, obd_chip_claim.c and obd_chip_fanout.c with the UART
 *        replaced by a line-at-a-time chip model (this file). What is tested
 *        is the order and the presence of the words on the wire.
 *
 * The fake chip: bytes collect in its line buffer up to a CR, as the real
 * one's do; `ATM0`, the protocol words and the other AT words answer `OK`,
 * a bus request answers data, anything else `?`. Bytes left in the line
 * buffer without a CR (an app's half-typed word through a bridge) merge
 * with the next word: `XX` + `ATM0` is the line `XXATM0`, answered `?`.
 * That is how a real chip refuses an `ATM0` the driver sent.
 */
#include "obd_chip.h"

#include <string.h>
#include <time.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_gate.h"
#include "obd_chip_private.h"

#define PROMPT      "\r\r>"
#define REPLY_OK    "\rOK" PROMPT
#define REPLY_WHAT  "\r?" PROMPT
#define REPLY_DATA  "\r7E8 06 41 00 BE 3F A8 13" PROMPT

#define T_AT pdMS_TO_TICKS(500)
#define T_SHORT pdMS_TO_TICKS(60)

/* ---- the fake chip ------------------------------------------------------- */

const char obd_chip_gate_owner[1];

static char   s_wire[2048];   /* every byte the driver wrote            */
static size_t s_wire_len;
static char   s_line[128];    /* the chip's line buffer                 */
static size_t s_line_len;
static const char *s_m0_reply;  /* NULL = the chip says nothing to ATM0 */
static bool   s_echo;           /* ATE1: the line comes back first      */
static int    s_settings_changed;
static char   s_store;          /* the chip's EEPROM: its stored protocol */
static int    s_store_writes;   /* how often an `ATSP` wrote it          */
static bool   s_sp_silent;      /* the chip writes, its `OK` is lost     */

static void chip_reset(void)
{
    s_wire_len = 0;
    s_wire[0] = '\0';
    s_line_len = 0;
    s_m0_reply = REPLY_OK;
    s_echo = false;
    s_settings_changed = 0;
    s_store = '7';
    s_store_writes = 0;
    s_sp_silent = false;
}

/** Forget what went over the wire so far (the next assertion's window). */
static void wire_clear(void)
{
    s_wire_len = 0;
    s_wire[0] = '\0';
}

/** Bytes an app left in the chip's line buffer, no CR behind them. */
static void chip_pending(const char *junk)
{
    s_line_len = strlen(junk);
    memcpy(s_line, junk, s_line_len);
}

static const char *chip_answer(const char *line)
{
    if (strcmp(line, "ATM0") == 0)
    {
        return s_m0_reply;
    }

    if (strncmp(line, "ATSP", 4) == 0 && line[4] != '\0' && line[5] == '\0')
    {
        /* the real save: the chip acts on the word, then answers */
        s_store = line[4];
        s_store_writes++;
        return s_sp_silent ? NULL : REPLY_OK;
    }

    if (strncmp(line, "AT", 2) == 0 || strncmp(line, "STP", 3) == 0)
    {
        return REPLY_OK;
    }

    if (strcmp(line, "0100") == 0)
    {
        return REPLY_DATA;
    }

    return REPLY_WHAT;
}

esp_err_t obd_uart_write(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (s_wire_len < sizeof(s_wire) - 1)
        {
            s_wire[s_wire_len++] = (char)data[i];
            s_wire[s_wire_len] = '\0';
        }

        if (data[i] != '\r')
        {
            if (s_line_len < sizeof(s_line) - 1)
            {
                s_line[s_line_len++] = (char)data[i];
            }

            continue;
        }

        s_line[s_line_len] = '\0';
        s_line_len = 0;

        const char *reply = chip_answer(s_line);

        if (reply == NULL)
        {
            continue;
        }

        if (s_echo)
        {
            obd_core_fanout((const uint8_t *)s_line, strlen(s_line));
        }

        obd_core_fanout((const uint8_t *)reply, strlen(reply));
    }

    return ESP_OK;
}

/* ---- what the driver's other files provide on the target ----------------- */

int64_t esp_timer_get_time(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

bool obd_core_bringup_wait(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void obd_chip_settings_changed(void)
{
    s_settings_changed++;
}

bool obd_gate_enabled(void)
{
    return false;
}

esp_err_t obd_gate_acquire(const void *owner, uint32_t wait_ms)
{
    (void)owner;
    (void)wait_ms;
    return ESP_OK;
}

void obd_gate_release(const void *owner)
{
    (void)owner;
}

/* ---- helpers ------------------------------------------------------------- */

static obd_cmd_memory_t s_mem0;     /* the counters when the test began */
static uint32_t         s_m0_count0;

static uint32_t memory_off_count(void)
{
    obd_fanout_stats_t fs;

    obd_core_fanout_stats(&fs);
    return fs.memory_off;
}

static void engine_up(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, obd_cmd_engine_init());
    chip_reset();
    obd_cmd_memory_reset(); /* as a fresh bring-up: nothing owed */
    obd_cmd_memory_stats(&s_mem0);
    s_m0_count0 = memory_off_count();
}

static void assert_memory(uint32_t refused, uint32_t held, bool owed)
{
    obd_cmd_memory_t now;

    obd_cmd_memory_stats(&now);
    TEST_ASSERT_EQUAL_UINT32(refused, now.refused - s_mem0.refused);
    TEST_ASSERT_EQUAL_UINT32(held, now.held - s_mem0.held);
    TEST_ASSERT_EQUAL(owed, now.owed);
}

/* ---- the tests ----------------------------------------------------------- */

void test_request_switch_goes_out_behind_acknowledged_m0(void)
{
    char resp[64];

    engine_up();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATTP7", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATM0\rATTP7\r", s_wire);
    TEST_ASSERT_EQUAL_STRING("OK", resp);
    TEST_ASSERT_EQUAL_UINT32(1, memory_off_count() - s_m0_count0);
    assert_memory(0, 0, false);
    TEST_ASSERT_EQUAL(0, s_settings_changed);
}

/* The reproduction (2026-11-10): the chip answers `?` to the ATM0 (an app's
 * bytes sat in its line buffer). The switch must not reach the wire. */
void test_request_switch_not_sent_when_m0_answers_what(void)
{
    char resp[64] = "untouched";

    engine_up();
    chip_pending("XX");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      obd_chip_request("ATTP7", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATM0\r", s_wire);
    TEST_ASSERT_EQUAL_STRING("", resp);
    TEST_ASSERT_EQUAL_UINT32(0, memory_off_count() - s_m0_count0);
    assert_memory(1, 0, true);
    TEST_ASSERT_EQUAL(1, s_settings_changed); /* the holders' preludes are due */
}

/* The reproduction, the silent case: no prompt to the ATM0 inside the
 * caller's budget. The switch must not reach the wire. */
void test_request_switch_not_sent_when_m0_times_out(void)
{
    char resp[64] = "untouched";

    engine_up();
    s_m0_reply = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      obd_chip_request("STP33", resp, sizeof(resp), T_SHORT));
    TEST_ASSERT_EQUAL_STRING("ATM0\r", s_wire);
    TEST_ASSERT_EQUAL_STRING("", resp);
    assert_memory(1, 0, true);
}

/* `OK` and nothing else: a text that merely contains it, a stopped monitor
 * and a busy bus are refusals; the guard's ATSP -> ATTP form is covered. */
void test_request_m0_must_be_ok_and_nothing_else(void)
{
    static const char *const NOT_OK[] =
    {
        "\rNOT OK" PROMPT, "\rSTOPPED" PROMPT, "\rBUS BUSY" PROMPT,
        "\rOK\rELM327 v2.3" PROMPT, PROMPT,
    };
    char resp[64];

    engine_up();

    for (size_t i = 0; i < sizeof(NOT_OK) / sizeof(NOT_OK[0]); i++)
    {
        wire_clear();
        s_m0_reply = NOT_OK[i];
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_RESPONSE,
                                  obd_chip_request("ATSP6", resp, sizeof(resp), T_AT),
                                  NOT_OK[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ATM0\r", s_wire, NOT_OK[i]);
    }

    assert_memory(5, 0, true);

    /* the chip acknowledges again: the switch goes out, nothing is owed */
    wire_clear();
    s_m0_reply = "\r\nOK\r\n" PROMPT;
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATSP6", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATM0\rATTP6\r", s_wire);
    assert_memory(5, 0, false);
}

/* A refused switch is owed: what would transmit on the bus waits, what only
 * sets the chip passes, and the first switch that goes through ends it. */
void test_request_bus_requests_held_while_a_switch_is_owed(void)
{
    char resp[64] = "untouched";

    engine_up();
    chip_pending("XX");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, obd_chip_request("ATTP7", resp, sizeof(resp), T_AT));
    wire_clear();
    s_settings_changed = 0;

    /* the caller did not look at the switch's result and goes on */
    strcpy(resp, "untouched");
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED,
                      obd_chip_request("0100", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("", s_wire);
    TEST_ASSERT_EQUAL_STRING("", resp);
    TEST_ASSERT_EQUAL(1, s_settings_changed);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED,
                      obd_chip_request("22F190", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("", s_wire);
    assert_memory(1, 2, true);

    /* a header, a filter and a read change nothing on the bus */
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATSH7DF", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATCRA", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATSH7DF\rATCRA\r", s_wire);
    assert_memory(1, 2, true);

    /* the next switch finds the chip acknowledging: the bus is open again */
    wire_clear();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATTP6", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("0100", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATM0\rATTP6\r0100\r", s_wire);
    TEST_ASSERT_EQUAL_STRING("7E8 06 41 00 BE 3F A8 13", resp);
    assert_memory(1, 2, false);
}

/* An app's reset left the chip's echo on, and the guard rewrote the word:
 * the answer carries the echo of `ATTP6` ahead of the `OK`. Still taken. */
void test_request_owed_switch_ends_with_the_echo_on(void)
{
    char resp[64];

    engine_up();
    chip_pending("XX");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, obd_chip_request("ATSP7", resp, sizeof(resp), T_AT));
    assert_memory(1, 0, true);

    s_echo = true;
    wire_clear();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATSP6", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("ATM0\rATTP6\r", s_wire);
    assert_memory(1, 0, false);
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("0100", resp, sizeof(resp), T_AT));
}

/* A caller inside its own transaction (the UDS tool, the self-test): the
 * pair rides its hold, a refusal leaves the hold with the caller. */
void test_request_switch_inside_the_callers_transaction(void)
{
    char resp[64];

    engine_up();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_txn_begin(T_AT));
    TEST_ASSERT_TRUE(obd_core_txn_owned());

    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("ATTP6", resp, sizeof(resp), T_AT));
    TEST_ASSERT_TRUE(obd_core_txn_owned());

    chip_pending("XX");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      obd_chip_request("ATTP7", resp, sizeof(resp), T_AT));
    TEST_ASSERT_TRUE(obd_core_txn_owned());
    TEST_ASSERT_EQUAL_STRING("ATM0\rATTP6\rATM0\r", s_wire);

    obd_chip_txn_end();
    TEST_ASSERT_FALSE(obd_core_txn_owned());
    TEST_ASSERT_EQUAL(0, obd_core_claim_state());
}

/* No hold, no pair: with a monitor session on the chip neither the ATM0 nor
 * the switch goes out (before: the two took separate claims). */
void test_request_no_hold_no_switch(void)
{
    char resp[64] = "untouched";

    engine_up();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_claim(OBD_CHIP_CLAIM_MONITOR, T_SHORT));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      obd_chip_request("ATTP7", resp, sizeof(resp), T_SHORT));
    TEST_ASSERT_EQUAL_STRING("", s_wire);
    TEST_ASSERT_EQUAL_STRING("", resp);
    assert_memory(0, 0, false);
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_release());
}

/* The start-up check's order (2026-11-11): the bring-up task takes EXCLUSIVE
 * before it opens the bring-up gate, and the firmware check it then runs
 * claims again, as every caller of the update engine does. The holder is let
 * in; anybody else is refused while it holds; one release ends the hold. */
static volatile esp_err_t s_other_claim;

static void other_task_claims_exclusive(void *arg)
{
    (void)arg;
    s_other_claim = obd_chip_claim(OBD_CHIP_CLAIM_EXCLUSIVE, 0);
    vTaskDelete(NULL);
}

void test_claim_exclusive_holder_claims_again(void)
{
    engine_up();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_claim(OBD_CHIP_CLAIM_EXCLUSIVE, 0));
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_claim(OBD_CHIP_CLAIM_EXCLUSIVE, 0));
    TEST_ASSERT_EQUAL(3, obd_core_claim_state());

    /* another task is not the holder: refused, not let in */
    s_other_claim = ESP_FAIL;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(other_task_claims_exclusive, "other",
                                          4096, NULL, 5, NULL));

    for (int i = 0; i < 100 && s_other_claim == ESP_FAIL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_other_claim);

    /* a request meanwhile is refused too (the first requester of a boot
       waits its 2 s for the check; here the wait is the short one) */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      obd_chip_claim(OBD_CHIP_CLAIM_COMMAND, T_SHORT));
    TEST_ASSERT_EQUAL_STRING("", s_wire);

    /* claimed twice, released once: the chip is free and remembers no
       holder (the task that held it is not let in over another claim) */
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_release());
    TEST_ASSERT_EQUAL(0, obd_core_claim_state());
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_claim(OBD_CHIP_CLAIM_MONITOR, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      obd_chip_claim(OBD_CHIP_CLAIM_EXCLUSIVE, 0));
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_release());
}

/* A fresh bring-up: nothing is owed from the chip's life before it. */
void test_request_bringup_clears_what_is_owed(void)
{
    char resp[64];

    engine_up();
    chip_pending("XX");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, obd_chip_request("ATTP7", resp, sizeof(resp), T_AT));
    assert_memory(1, 0, true);
    obd_cmd_memory_reset();
    assert_memory(1, 0, false);
    wire_clear();
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("0100", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("0100\r", s_wire);
}

void test_bus_request_classification(void)
{
    static const char *const BUS[] =
    {
        "0100", "010C1", "22F190", "22 F1 90", " 03", "", "\r", "ATFI", "at si",
        "ATRTR", "STPX h:7DF, d:0100", "VTFullyRequestCk0008AABB0165",
        "0100;ATSH7E0", "XYZ",
    };
    static const char *const NOT_BUS[] =
    {
        "ATSH7DF", "ATCRA", "at h1", "ATST96", "ATE0", "ATI", "ATDPN", "ATRV",
        "ATTP6", "ATM0", "ATZ", "ATWS", "ATCP18", "ATFCSM1", "ATS1",
        "STP33", "STPBR 500000", "STSLEEP0", "STFCP", "STFAP 00F000,00FF00",
        "VTVERS", "VTP234", "VTPPSWS", "ATSH7DF;0100",
    };

    for (size_t i = 0; i < sizeof(BUS) / sizeof(BUS[0]); i++)
    {
        TEST_ASSERT_TRUE_MESSAGE(obd_parse_is_bus_request(BUS[i]), BUS[i]);
    }

    for (size_t i = 0; i < sizeof(NOT_BUS) / sizeof(NOT_BUS[0]); i++)
    {
        TEST_ASSERT_FALSE_MESSAGE(obd_parse_is_bus_request(NOT_BUS[i]), NOT_BUS[i]);
    }

    TEST_ASSERT_FALSE(obd_parse_is_bus_request(NULL));
}

/* ---- the deliberate save (obd_chip_protocol_save) ------------------------ */

static uint32_t s_saves0;
static uint32_t s_kept0;

/** A start as the bring-up leaves it: the per-boot state fresh, the chip's
 *  stored protocol read (NULL = the chip did not answer `ATDPN`). */
static void boot_reads(const char *dpn)
{
    obd_cmd_test_new_boot();
    obd_cmd_stored_note(dpn);
    s_saves0 = obd_cmd_protocol_saves();
    s_kept0 = obd_cmd_protocol_kept();
    wire_clear();
}

static void assert_save(uint32_t saves, uint32_t kept, const char *stored)
{
    char text[OBD_STORED_LEN];

    obd_cmd_stored_text(text, sizeof(text));
    TEST_ASSERT_EQUAL_UINT32(saves, obd_cmd_protocol_saves() - s_saves0);
    TEST_ASSERT_EQUAL_UINT32(kept, obd_cmd_protocol_kept() - s_kept0);
    TEST_ASSERT_EQUAL_STRING(stored, text);
}

/* The reproduction (2026-11-10): the chip takes the `ATSP`, its `OK` is
 * lost. A second call in the same boot must not write again, and the next
 * start, which reads the store, must not either. */
void test_save_lost_answer_is_not_written_twice(void)
{
    engine_up();
    boot_reads("A7");
    s_sp_silent = true;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL(1, s_store_writes);
    assert_save(0, 0, ""); /* what the chip holds is not known now */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL(1, s_store_writes);
    TEST_ASSERT_EQUAL_STRING("ATSP6\r", s_wire);

    /* the next start reads 6: the write was taken, nothing more to send */
    s_sp_silent = false;
    boot_reads("6");
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL_STRING("", s_wire);
    TEST_ASSERT_EQUAL(1, s_store_writes);
    assert_save(0, 1, "6");
}

/* A word the chip refused wrote nothing: still one attempt per boot, and
 * the next start, reading the old value, sends it once more. */
void test_save_refused_word_waits_for_the_next_start(void)
{
    engine_up();
    boot_reads("A7");
    chip_pending("XX");
    TEST_ASSERT_EQUAL(ESP_FAIL, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL(0, s_store_writes);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL_STRING("ATSP6\r", s_wire);

    boot_reads("A7");
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL_STRING("ATSP6\r", s_wire);
    TEST_ASSERT_EQUAL(1, s_store_writes);
    assert_save(1, 0, "6");
}

/* The chip holds the protocol already, with or without its automatic flag:
 * nothing is sent and the caller hears ESP_OK. */
void test_save_not_sent_when_the_chip_holds_it(void)
{
    static const struct
    {
        const char *dpn;
        char        proto;
    } HELD[] =
    {
        { "6", '6' }, { "A6", '6' }, { "a6\r", '6' }, { " A6 ", '6' },
        { "A", 'A' }, { "AA", 'a' }, { "9", '9' },
    };

    engine_up();

    for (size_t i = 0; i < sizeof(HELD) / sizeof(HELD[0]); i++)
    {
        boot_reads(HELD[i].dpn);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, obd_chip_protocol_save(HELD[i].proto),
                                  HELD[i].dpn);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", s_wire, HELD[i].dpn);
        TEST_ASSERT_EQUAL_UINT32(1, obd_cmd_protocol_kept() - s_kept0);
        TEST_ASSERT_EQUAL_UINT32(0, obd_cmd_protocol_saves() - s_saves0);
    }

    TEST_ASSERT_EQUAL(0, s_store_writes);
}

/* The chip holds another protocol, something the save does not take (an
 * STN preset), or did not answer the read: one `ATSP`, acknowledged, and
 * the driver knows the store from then on. One `ATSP` per boot. */
void test_save_writes_once_when_the_chip_holds_another(void)
{
    static const char *const OTHER[] = { "A7", "7", "A0", "34 [ST]", "?", "", NULL };

    engine_up();

    for (size_t i = 0; i < sizeof(OTHER) / sizeof(OTHER[0]); i++)
    {
        const char *label = (OTHER[i] != NULL) ? OTHER[i] : "(no answer)";

        s_store_writes = 0;
        boot_reads(OTHER[i]);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, obd_chip_protocol_save('6'), label);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ATSP6\r", s_wire, label);
        TEST_ASSERT_EQUAL_MESSAGE(1, s_store_writes, label);
        assert_save(1, 0, "6");

        /* asked again in the same boot: held, nothing sent */
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, obd_chip_protocol_save('6'), label);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ATSP6\r", s_wire, label);
        assert_save(1, 1, "6");

        /* another protocol in the same boot: the one `ATSP` is spent */
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_STATE,
                                  obd_chip_protocol_save('7'), label);
        TEST_ASSERT_EQUAL_MESSAGE(1, s_store_writes, label);
    }
}

/* The mark is set when the word goes out, not before: a save that could
 * not take the chip (a monitor session) spends nothing. */
void test_save_blocked_by_a_monitor_spends_nothing(void)
{
    engine_up();
    boot_reads("A7");
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_claim(OBD_CHIP_CLAIM_MONITOR, T_SHORT));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL_STRING("", s_wire);
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_release());
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_protocol_save('6'));
    TEST_ASSERT_EQUAL_STRING("ATSP6\r", s_wire);
    assert_save(1, 0, "6");
}

void test_save_refuses_what_is_not_a_protocol(void)
{
    static const char BAD[] = { '0', 'D', 'x', ' ', '\0' };

    engine_up();
    boot_reads("A7");

    for (size_t i = 0; i < sizeof(BAD); i++)
    {
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, obd_chip_protocol_save(BAD[i]));
    }

    TEST_ASSERT_EQUAL_STRING("", s_wire);
    assert_save(0, 0, "A7");
}

void test_dpn_parse(void)
{
    char p = '\0';
    bool a = true;

    TEST_ASSERT_TRUE(obd_parse_dpn("6", &p, &a));
    TEST_ASSERT_EQUAL_CHAR('6', p);
    TEST_ASSERT_FALSE(a);
    TEST_ASSERT_TRUE(obd_parse_dpn("A6", &p, &a));
    TEST_ASSERT_EQUAL_CHAR('6', p);
    TEST_ASSERT_TRUE(a);
    TEST_ASSERT_TRUE(obd_parse_dpn("\r\na7 \r", &p, &a));
    TEST_ASSERT_EQUAL_CHAR('7', p);
    TEST_ASSERT_TRUE(a);
    TEST_ASSERT_TRUE(obd_parse_dpn("A", &p, &a)); /* J1939, not "automatic" */
    TEST_ASSERT_EQUAL_CHAR('A', p);
    TEST_ASSERT_FALSE(a);
    TEST_ASSERT_TRUE(obd_parse_dpn("AC", &p, &a));
    TEST_ASSERT_EQUAL_CHAR('C', p);
    TEST_ASSERT_TRUE(a);
    TEST_ASSERT_TRUE(obd_parse_dpn("0", &p, NULL));
    TEST_ASSERT_EQUAL_CHAR('0', p);
    TEST_ASSERT_TRUE(obd_parse_dpn("3", NULL, NULL));

    TEST_ASSERT_FALSE(obd_parse_dpn("34 [ST]", &p, &a)); /* the STN preset */
    TEST_ASSERT_FALSE(obd_parse_dpn("11A [VT]", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn("34", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn("D", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn("AD", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn("?", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn("", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn("\r", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn("STOPPED", &p, &a));
    TEST_ASSERT_FALSE(obd_parse_dpn(NULL, &p, &a));
}
