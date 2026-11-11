/**
 * @file test_store_words.c
 * @brief The words the OBD chip stores every time it takes them (a protocol
 *        preset, a link word; autopid_bus.h): which words they are, that no
 *        chain of the tables keeps one, what the config parser refuses and
 *        what it lets a switched-off row keep, the repair of a file an
 *        earlier build stored, and the rows that wait while their type's
 *        init carries one.
 */
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "cJSON.h"

#include "autopid_bus.h"
#include "autopid_private.h"

/* ~1 MB (the 2048-parameter pool): static, never on the task stack */
static ap_config_t s_cfg;

#define ROW(name, init, extra) \
    "{\"name\":\"" name "\",\"cmd\":\"22F190\",\"init\":\"" init "\"," extra \
    "\"parameters\":[{\"name\":\"" name "\",\"expression\":\"B4\"}]}"

void test_store_which_words(void)
{
    char w[16] = "";

    /* the presets, as the chip reads them: spaces ignored, any case */
    TEST_ASSERT_TRUE(ap_chain_store_word("STP53", w, sizeof(w)));
    TEST_ASSERT_EQUAL_STRING("STP53", w);
    TEST_ASSERT_TRUE(ap_chain_store_word("stp 53", w, sizeof(w)));
    TEST_ASSERT_EQUAL_STRING("STP53", w);
    TEST_ASSERT_TRUE(ap_chain_store_word("VTP 2C3", w, sizeof(w)));
    TEST_ASSERT_EQUAL_STRING("VTP2C3", w);
    TEST_ASSERT_TRUE(ap_chain_store_word("STP 6", NULL, 0));
    /* the link words */
    TEST_ASSERT_TRUE(ap_chain_store_word("ATLNKMS PB", w, sizeof(w)));
    TEST_ASSERT_EQUAL_STRING("ATLNKMSPB", w);
    TEST_ASSERT_TRUE(ap_chain_store_word("at lnkhs pb", NULL, 0));
    /* anywhere in a chain: the first one is named */
    TEST_ASSERT_TRUE(ap_chain_store_word("ATH1;STP 53;ATSH726;ATLNKMS PB",
                                         w, sizeof(w)));
    TEST_ASSERT_EQUAL_STRING("STP53", w);
    TEST_ASSERT_TRUE(ap_chain_store_word("ATSH726;;vtp2c1", NULL, 0));

    /* not stored: no chain, the usual words of an init, a request */
    TEST_ASSERT_FALSE(ap_chain_store_word(NULL, w, sizeof(w)));
    TEST_ASSERT_FALSE(ap_chain_store_word("", w, sizeof(w)));
    TEST_ASSERT_FALSE(ap_chain_store_word("ATSP6;ATSH7E4;ATST96", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("ATTPB", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("22F190", NULL, 0));
    /* RAM words that begin like a preset: a bit rate behind the slot, a
       timeout, the open and close words (the chip reads `STP C` as STPC) */
    TEST_ASSERT_FALSE(ap_chain_store_word("STPBR 250000", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("VTPBR83333", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("STPBRR", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("STPTO 100", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("STPO", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("STPC", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("STP C", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("STP", NULL, 0));
    TEST_ASSERT_FALSE(ap_chain_store_word("STP5300", NULL, 0));
    /* a slot defined in RAM is the way to another bus, not a stored word */
    TEST_ASSERT_FALSE(ap_chain_store_word("VTSET_CAN B,81,04,MS_CAN", NULL,
                                          0));

    /* a row of another bus still may carry none of them, nor close the
       protocol under the poller */
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("STP53"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATLNKMS PB"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("STPC"));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("STPBR 250000"));
}

void test_store_no_chain_keeps_one(void)
{
    char c[AP_INIT_LEN];

    snprintf(c, sizeof(c), "STP53;ATSH726");
    TEST_ASSERT_EQUAL(1, ap_chain_strip_store(c));
    TEST_ASSERT_EQUAL_STRING("ATSH726", c);

    snprintf(c, sizeof(c), "ATSH726;stp 53");
    TEST_ASSERT_EQUAL(1, ap_chain_strip_store(c));
    TEST_ASSERT_EQUAL_STRING("ATSH726", c);

    snprintf(c, sizeof(c), "ATH1;VTP 2C3;ATSH7DF;ATLNKMS PB;ATCRA7E8");
    TEST_ASSERT_EQUAL(2, ap_chain_strip_store(c));
    TEST_ASSERT_EQUAL_STRING("ATH1;ATSH7DF;ATCRA7E8", c);

    snprintf(c, sizeof(c), "STP53");
    TEST_ASSERT_EQUAL(1, ap_chain_strip_store(c));
    TEST_ASSERT_EQUAL_STRING("", c);

    /* nothing to drop: the chain is not touched */
    snprintf(c, sizeof(c), "ATSP6;STPBR 250000;ATSH7E4;");
    TEST_ASSERT_EQUAL(0, ap_chain_strip_store(c));
    TEST_ASSERT_EQUAL_STRING("ATSP6;STPBR 250000;ATSH7E4;", c);
    TEST_ASSERT_EQUAL(0, ap_chain_strip_store(NULL));

    /* what every chain of the tables goes through before the wire drops
       them with the rest (ATSP becomes ATTP, ATH0 goes) */
    snprintf(c, sizeof(c), "STP53;ATSP6;ATH0;ATLNKMS PB");
    ap_init_sanitize(c);
    TEST_ASSERT_EQUAL_STRING("ATTP6", c);
}

void test_store_parser_refuses_a_row_that_is_on(void)
{
    char err[96] = "";

    /* a row that is switched on with a preset in its init: refused, and the
       message names the row, the word and the way out */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[" ROW("ms_door", "STP53;ATSH726", "") "]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "ms_door"));
    TEST_ASSERT_NOT_NULL(strstr(err, "STP53"));
    TEST_ASSERT_NOT_NULL(strstr(err, "Bus"));

    /* `enabled` present and true, a link word, a vendor preset */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[" ROW("a", "ATLNKMS PB", "\"enabled\":true,") "]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[" ROW("a", "ATSH726;vtp 2c3", "") "]}",
        &s_cfg, err, sizeof(err)));

    /* the word as the row's request */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"a\",\"cmd\":\"STP53\",\"parameters\":[]}]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "STP53"));

    /* on a row of another bus as well */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[" ROW("a", "STP53", "\"bus\":\"ms\",") "]}",
        &s_cfg, err, sizeof(err)));

    /* a row that is switched off may keep it (the file an earlier build
       stored loads that way), and nothing of the word is left in what the
       poller would send */
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[" ROW("ms_door", "STP53;ATSH726", "\"enabled\":false,")
        "," ROW("rpm", "ATSH7E0", "") "]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(2, s_cfg.n_pids);
    TEST_ASSERT_FALSE(s_cfg.pids[0].enabled);
    TEST_ASSERT_EQUAL_STRING("ATSH726", s_cfg.pids[0].init);
    TEST_ASSERT_TRUE(s_cfg.pids[1].enabled);

    /* also when the word is the whole request of the switched-off row */
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"a\",\"cmd\":\"STP53\",\"enabled\":false,"
        "\"parameters\":[]}]}",
        &s_cfg, err, sizeof(err)));

    /* a bit rate word behind a slot is RAM: no refusal */
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[" ROW("a", "STPBR 250000", "\"bus\":\"ms\",") "]}",
        &s_cfg, err, sizeof(err)));
}

void test_store_a_stored_file_is_repaired(void)
{
    const char *stored =
        "{\"groups\":[{\"name\":\"default\",\"period_ms\":1000}],\"pids\":["
        ROW("ms_door", "STP53;ATSH726", "") ","
        ROW("ms_seat", "ATLNKMS PB", "\"enabled\":true,") ","
        ROW("was_off", "STP53", "\"enabled\":false,") ","
        ROW("rpm", "ATSH7E0", "") "]}";
    char err[96] = "";
    int off = -1;

    /* as an earlier build stored it, the parser refuses the file whole */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(stored, &s_cfg, err,
                                                  sizeof(err)));

    /* the loader's repair: the two rows that were on are switched off */
    char *fixed = ap_config_repair_store(stored, &off);

    TEST_ASSERT_NOT_NULL(fixed);
    TEST_ASSERT_EQUAL(2, off);
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(fixed, &s_cfg, err,
                                              sizeof(err)));
    TEST_ASSERT_EQUAL(4, s_cfg.n_pids);
    TEST_ASSERT_FALSE(s_cfg.pids[0].enabled);
    TEST_ASSERT_FALSE(s_cfg.pids[1].enabled);
    TEST_ASSERT_FALSE(s_cfg.pids[2].enabled);
    TEST_ASSERT_TRUE(s_cfg.pids[3].enabled);   /* the clean row stays on */
    TEST_ASSERT_EQUAL_STRING("rpm", s_cfg.pids[3].name);

    /* the word stays in the file for the user to see and to fix */
    TEST_ASSERT_NOT_NULL(strstr(fixed, "STP53;ATSH726"));

    /* once: the repaired text needs no second repair */
    int again = -1;

    TEST_ASSERT_NULL(ap_config_repair_store(fixed, &again));
    TEST_ASSERT_EQUAL(0, again);
    free(fixed);

    /* nothing to repair, and a text that is no JSON: left to the parser */
    TEST_ASSERT_NULL(ap_config_repair_store(
        "{\"pids\":[" ROW("rpm", "ATSH7E0", "") "]}", &off));
    TEST_ASSERT_EQUAL(0, off);
    TEST_ASSERT_NULL(ap_config_repair_store("{}", &off));
    TEST_ASSERT_NULL(ap_config_repair_store("nonsense", &off));
    TEST_ASSERT_EQUAL(0, off);
}

void test_store_a_type_init_with_one_holds_its_rows(void)
{
    static ap_sched_t st; /* the slots of every row: not on the task stack */
    char err[96] = "";

    /* a custom row on the car's protocol, one on MS CAN, a J1939 group, and
       a standard row */
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(
        "{\"groups\":[{\"name\":\"default\",\"period_ms\":1000}],\"pids\":["
        ROW("obd_row", "ATSH726", "\"type\":\"custom\",") ","
        ROW("ms_row", "ATSH726", "\"type\":\"custom\",\"bus\":\"ms\",") ","
        "{\"name\":\"heard\",\"type\":\"custom\",\"cmd\":\"PGN:FEEE\","
        "\"parameters\":[{\"name\":\"heard\",\"expression\":\"B0\"}]},"
        "{\"name\":\"rpm\",\"type\":\"std\",\"cmd\":\"010C1\","
        "\"parameters\":[{\"name\":\"rpm\",\"expression\":\"[B3:B4]/4\"}]}"
        "]}", &s_cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(4, s_cfg.n_pids);
    TEST_ASSERT_EQUAL(AP_PID_CUSTOM, s_cfg.pids[0].type);
    TEST_ASSERT_EQUAL(AP_PID_STD, s_cfg.pids[3].type);

    memset(&st, 0, sizeof(st));
    ap_sched_reset(&st, &s_cfg, 0);
    st.type_enabled[0] = st.type_enabled[1] = st.type_enabled[2] = true;

    /* no init holds a word: every row is asked */
    for (int i = 0; i < 4; i++)
    {
        TEST_ASSERT_TRUE(ap_sched_entry_enabled(&st, &s_cfg, i));
    }

    /* `custom_init` = "STP53;ATSH726" (ap_runner_set_type_init sets the
       flag): the word is not sent, so the custom row of the car's protocol
       would go out with a header meant for MS CAN. It waits */
    st.type_held[AP_PID_CUSTOM] = true;
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&st, &s_cfg, 0));
    /* the row whose Bus is chosen never sees the type's init: it goes on,
       which is the way out the page names */
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&st, &s_cfg, 1));
    /* a J1939 group is heard, not asked; another type is not concerned */
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&st, &s_cfg, 2));
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&st, &s_cfg, 3));

    /* the scheduler never picks the held row, and still picks the others */
    int64_t now = 2000000;
    bool seen[4] = { false, false, false, false };

    for (int n = 0; n < 12; n++)
    {
        int64_t due = 0;
        int i = ap_sched_next(&st, &s_cfg, &due);

        TEST_ASSERT_TRUE(i >= 1 && i < 4);
        seen[i] = true;
        now = (due > now) ? due : now + 10000;
        ap_sched_ran(&st, &s_cfg, i, now, true);
    }

    TEST_ASSERT_FALSE(seen[0]);
    TEST_ASSERT_TRUE(seen[1] && seen[2] && seen[3]);

    /* `std_init` with one: the standard rows wait, the custom ones do not */
    st.type_held[AP_PID_CUSTOM] = false;
    st.type_held[AP_PID_STD] = true;
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&st, &s_cfg, 0));
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&st, &s_cfg, 3));

    /* the word taken out of the init: asked again */
    st.type_held[AP_PID_STD] = false;
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&st, &s_cfg, 3));

    /* a hold is about the type's init alone: a row that is switched off
       stays off whatever the flag says */
    s_cfg.pids[1].enabled = false;
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&st, &s_cfg, 1));
}

void run_store_words_tests(void)
{
    RUN_TEST(test_store_which_words);
    RUN_TEST(test_store_no_chain_keeps_one);
    RUN_TEST(test_store_parser_refuses_a_row_that_is_on);
    RUN_TEST(test_store_a_stored_file_is_repaired);
    RUN_TEST(test_store_a_type_init_with_one_holds_its_rows);
}
