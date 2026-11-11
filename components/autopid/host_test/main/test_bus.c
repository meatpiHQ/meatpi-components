/**
 * @file test_bus.c
 * @brief The bus of a row (autopid_bus.h): the config words, the chain that
 *        takes the chip to MS, single-wire or CH CAN without a write of its
 *        memory, what a row of another bus may carry, the config parse, and
 *        the scheduler's grouping by bus.
 */
#include <string.h>

#include "unity.h"

#include "autopid_bus.h"
#include "autopid_j1939.h"
#include "autopid_private.h"

/* ~1 MB (the 2048-parameter pool): static, never on the task stack */
static ap_config_t s_cfg;
static ap_sched_t s_st;

void test_bus_words(void)
{
    ap_row_bus_t b = AP_BUS_CH;

    /* no word, the empty word and "obd" are the car's OBD protocol */
    TEST_ASSERT_TRUE(ap_bus_parse(NULL, &b));
    TEST_ASSERT_EQUAL(AP_BUS_OBD, b);
    b = AP_BUS_CH;
    TEST_ASSERT_TRUE(ap_bus_parse("", &b));
    TEST_ASSERT_EQUAL(AP_BUS_OBD, b);
    TEST_ASSERT_TRUE(ap_bus_parse("obd", &b));
    TEST_ASSERT_EQUAL(AP_BUS_OBD, b);

    TEST_ASSERT_TRUE(ap_bus_parse("ms", &b));
    TEST_ASSERT_EQUAL(AP_BUS_MS, b);
    TEST_ASSERT_TRUE(ap_bus_parse("sw", &b));
    TEST_ASSERT_EQUAL(AP_BUS_SW, b);
    TEST_ASSERT_TRUE(ap_bus_parse("ch", &b));
    TEST_ASSERT_EQUAL(AP_BUS_CH, b);

    /* a word this firmware does not know is refused, not guessed */
    TEST_ASSERT_FALSE(ap_bus_parse("hs", &b));
    TEST_ASSERT_FALSE(ap_bus_parse("MS", &b));
    TEST_ASSERT_FALSE(ap_bus_parse("ms ", &b));

    for (int i = 0; i < AP_BUS_COUNT; i++)
    {
        TEST_ASSERT_TRUE(ap_bus_parse(ap_bus_word((uint8_t)i), &b));
        TEST_ASSERT_EQUAL(i, b);
    }

    TEST_ASSERT_EQUAL_STRING("obd", ap_bus_word(200)); /* out of range */
    TEST_ASSERT_EQUAL_STRING("MS CAN", ap_bus_label(AP_BUS_MS));
}

void test_bus_chain_writes_nothing(void)
{
    char chain[AP_INIT_LEN];

    /* the car's OBD protocol has no chain here: the baseline prelude is it */
    TEST_ASSERT_EQUAL(0, ap_bus_chain(AP_BUS_KEY(AP_BUS_OBD, 0), chain,
                                      sizeof(chain)));
    TEST_ASSERT_EQUAL_STRING("", chain);
    TEST_ASSERT_EQUAL_CHAR('\0', ap_bus_slot(AP_BUS_OBD));

    TEST_ASSERT_TRUE(ap_bus_chain(AP_BUS_KEY(AP_BUS_MS, 0), chain,
                                  sizeof(chain)) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "ATS1;ATH1;ATST96;VTSET_CAN B,81,04,MS_CAN;ATTPB;ATSH7DF;ATCRA",
        chain);
    TEST_ASSERT_TRUE(ap_bus_chain(AP_BUS_KEY(AP_BUS_SW, 0), chain,
                                  sizeof(chain)) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "ATS1;ATH1;ATST96;VTSET_CAN F,81,0F,SW_CAN,3;ATTPF;ATSH7DF;ATCRA",
        chain);
    TEST_ASSERT_TRUE(ap_bus_chain(AP_BUS_KEY(AP_BUS_CH, 0), chain,
                                  sizeof(chain)) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "ATS1;ATH1;ATST96;VTSET_CAN C,81,01,CH_CAN;ATTPC;ATSH7DF;ATCRA",
        chain);

    /* a buffer it does not fit: nothing, not a cut chain */
    TEST_ASSERT_EQUAL(0, ap_bus_chain(AP_BUS_KEY(AP_BUS_MS, 0), chain, 20));
    TEST_ASSERT_EQUAL_STRING("", chain);

    for (int i = AP_BUS_MS; i < AP_BUS_COUNT; i++)
    {
        char slot = ap_bus_slot((uint8_t)i);

        (void)ap_bus_chain(AP_BUS_KEY(i, 0), chain, sizeof(chain));
        char def[] = { 'V', 'T', 'S', 'E', 'T', '_', 'C', 'A', 'N', ' ', slot,
                       ',', '\0' };
        char pick[] = { ';', 'A', 'T', 'T', 'P', slot, ';', '\0' };

        /* the chip STORES a preset and a link word (bench 2026-11-11): the
           chain has neither, nor the one stored protocol save */
        TEST_ASSERT_NULL(strstr(chain, "STP"));
        TEST_ASSERT_NULL(strstr(chain, "VTP"));
        TEST_ASSERT_NULL(strstr(chain, "ATLNK"));
        TEST_ASSERT_NULL(strstr(chain, "ATSP"));
        TEST_ASSERT_NULL(strstr(chain, "VTCFG"));

        /* the slot is defined BEFORE it is chosen (defined while chosen,
           the requests behind it read NO DATA) */
        const char *d = strstr(chain, def);
        const char *p = strstr(chain, pick);

        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_TRUE(d < p);

        /* every word of it passes the chip driver's memory guard as it is */
        TEST_ASSERT_TRUE(ap_bus_chain_allowed("ATSH7DF;ATCRA"));
    }

    /* single-wire CAN: the transceiver's normal mode (3), never the
       high-voltage wake */
    (void)ap_bus_chain(AP_BUS_KEY(AP_BUS_SW, 0), chain, sizeof(chain));
    TEST_ASSERT_NOT_NULL(strstr(chain, "SW_CAN,3;"));
}

void test_bus_speed(void)
{
    uint8_t code = 0xFF;
    char chain[AP_INIT_LEN];

    /* no speed named, and the bus's own: code 0, one place for both */
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 0, &code));
    TEST_ASSERT_EQUAL(0, code);
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 125, &code));
    TEST_ASSERT_EQUAL(0, code);
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_CH, 500, &code));
    TEST_ASSERT_EQUAL(0, code);
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_SW, 33.3, &code));
    TEST_ASSERT_EQUAL(0, code);
    TEST_ASSERT_EQUAL(125000, ap_bus_rate_bps(AP_BUS_MS, 0));
    TEST_ASSERT_EQUAL(33333, ap_bus_rate_bps(AP_BUS_SW, 0));
    TEST_ASSERT_EQUAL(500000, ap_bus_rate_bps(AP_BUS_CH, 0));
    TEST_ASSERT_EQUAL(0, ap_bus_rate_bps(AP_BUS_OBD, 0));

    /* the divisors of 500 kbit/s */
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 500, &code));
    TEST_ASSERT_EQUAL(1, code);
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 250, &code));
    TEST_ASSERT_EQUAL(2, code);
    TEST_ASSERT_EQUAL(250000, ap_bus_rate_bps(AP_BUS_MS, code));
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_CH, 125, &code));
    TEST_ASSERT_EQUAL(4, code);
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 100, &code));
    TEST_ASSERT_EQUAL(5, code);
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 83.3, &code));
    TEST_ASSERT_EQUAL(6, code); /* 500 / 6, not the 8/7 form of 7 */
    TEST_ASSERT_EQUAL(83333, ap_bus_rate_bps(AP_BUS_MS, code));
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 50, &code));
    TEST_ASSERT_EQUAL(10, code);
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 33.3, &code));
    TEST_ASSERT_EQUAL(15, code);

    /* 95.2 kbit/s takes the 8/7 multiplier: 500 * 8 / 7 / 6 */
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_MS, 95.2, &code));
    TEST_ASSERT_EQUAL(AP_BUS_RATE_X87 | 6, code);
    TEST_ASSERT_EQUAL(95238, ap_bus_rate_bps(AP_BUS_MS, code));

    /* a speed the chip cannot make, and a speed on the car's own protocol
       (that one has its speed in its number) */
    TEST_ASSERT_FALSE(ap_bus_rate_parse(AP_BUS_MS, 300, &code));
    TEST_ASSERT_FALSE(ap_bus_rate_parse(AP_BUS_MS, 5, &code));
    TEST_ASSERT_FALSE(ap_bus_rate_parse(AP_BUS_MS, 2000, &code));
    TEST_ASSERT_FALSE(ap_bus_rate_parse(AP_BUS_OBD, 250, &code));
    TEST_ASSERT_TRUE(ap_bus_rate_parse(AP_BUS_OBD, 0, &code));

    /* the chain carries it: the divisor, and option 91 with the multiplier */
    (void)ap_bus_rate_parse(AP_BUS_MS, 250, &code);
    (void)ap_bus_chain(AP_BUS_KEY(AP_BUS_MS, code), chain, sizeof(chain));
    TEST_ASSERT_EQUAL_STRING(
        "ATS1;ATH1;ATST96;VTSET_CAN B,81,02,MS_CAN;ATTPB;ATSH7DF;ATCRA",
        chain);
    (void)ap_bus_rate_parse(AP_BUS_CH, 95.2, &code);
    (void)ap_bus_chain(AP_BUS_KEY(AP_BUS_CH, code), chain, sizeof(chain));
    TEST_ASSERT_EQUAL_STRING(
        "ATS1;ATH1;ATST96;VTSET_CAN C,91,06,CH_CAN;ATTPC;ATSH7DF;ATCRA",
        chain);
    (void)ap_bus_rate_parse(AP_BUS_SW, 83.3, &code);
    (void)ap_bus_chain(AP_BUS_KEY(AP_BUS_SW, code), chain, sizeof(chain));
    TEST_ASSERT_EQUAL_STRING(
        "ATS1;ATH1;ATST96;VTSET_CAN F,81,06,SW_CAN,3;ATTPF;ATSH7DF;ATCRA",
        chain);

    /* the key keeps the two apart */
    TEST_ASSERT_EQUAL(AP_BUS_MS, AP_BUS_OF(AP_BUS_KEY(AP_BUS_MS, 2)));
    TEST_ASSERT_EQUAL(2, AP_BUS_RATE_OF(AP_BUS_KEY(AP_BUS_MS, 2)));
    TEST_ASSERT_EQUAL(0, AP_BUS_KEY(AP_BUS_OBD, 0));
}

void test_bus_chain_allowed(void)
{
    /* what a row of another bus usually carries */
    TEST_ASSERT_TRUE(ap_bus_chain_allowed(NULL));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed(""));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("ATSH726;ATCRA72E"));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("ATSH726;ATFCSH726;ATFCSD300000;ATFCSM1"));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("ATST40;ATAT2"));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("22F190"));
    /* a bit rate behind the slot is RAM and no choice of protocol */
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("STPBR 250000"));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("VTPBR83333"));
    TEST_ASSERT_TRUE(ap_bus_chain_allowed("STPBRR"));

    /* a protocol of its own, in every form the chip reads */
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATSP6"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("at tp 6"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATSH726;ATTPB"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATTPF"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATSPA6"));
    /* the presets and the link words: stored by the chip */
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("STP 53"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("stp53;ATSH726"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("VTP2C3"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("VTP 2C3"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATLNKMS PB"));
    /* the slot is ours to define */
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("VTSET_CAN B,01,04,MS_CAN"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATPB 81 0F"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("VTCFG_CAN 101,81,0F,SW_CAN,3"));
    /* a reset drops the slot's definition */
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATZ"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATSH726;ATD"));
    TEST_ASSERT_FALSE(ap_bus_chain_allowed("ATWS"));
}

void test_bus_config_parse(void)
{
    char err[128] = "";
    const char *json =
        "{\"pids\":["
        "{\"name\":\"rpm\",\"cmd\":\"010C\",\"type\":\"std\","
        "\"parameters\":[{\"name\":\"rpm\",\"expression\":\"[B3:B4]/4\"}]},"
        "{\"name\":\"door\",\"cmd\":\"224101\",\"bus\":\"ms\","
        "\"init\":\"ATSH726\",\"rxheader\":\"72E\","
        "\"parameters\":[{\"name\":\"door\",\"expression\":\"B4\"}]},"
        "{\"name\":\"seat\",\"cmd\":\"224102\",\"bus\":\"sw\","
        "\"parameters\":[{\"name\":\"seat\",\"expression\":\"B4\"}]},"
        "{\"name\":\"gw\",\"cmd\":\"22F190\",\"bus\":\"ch\","
        "\"bus_kbps\":250,"
        "\"parameters\":[{\"name\":\"gw\",\"expression\":\"B4\"}]},"
        "{\"name\":\"plain\",\"cmd\":\"22F191\",\"bus\":\"obd\","
        "\"parameters\":[{\"name\":\"plain\",\"expression\":\"B4\"}]}]}";

    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(json, &s_cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(5, s_cfg.n_pids);
    TEST_ASSERT_EQUAL(AP_BUS_OBD, s_cfg.pids[0].bus); /* no field */
    TEST_ASSERT_EQUAL(AP_BUS_MS, s_cfg.pids[1].bus);
    TEST_ASSERT_EQUAL(AP_BUS_SW, s_cfg.pids[2].bus);
    TEST_ASSERT_EQUAL(AP_BUS_CH, s_cfg.pids[3].bus);
    TEST_ASSERT_EQUAL(2, s_cfg.pids[3].bus_rate); /* 250 kbit/s = 500 / 2 */
    TEST_ASSERT_EQUAL(0, s_cfg.pids[1].bus_rate); /* no speed: its own */
    TEST_ASSERT_EQUAL(AP_BUS_OBD, s_cfg.pids[4].bus);

    /* a row of another bus is its own scheduling class: the HS bus guard
       has no say about it; it is a chip row for everything else */
    TEST_ASSERT_EQUAL(AP_CLASS_CHIP, ap_row_class(&s_cfg.pids[0]));
    TEST_ASSERT_EQUAL(AP_CLASS_CHIP_BUS, ap_row_class(&s_cfg.pids[1]));
    TEST_ASSERT_TRUE(AP_CLASS_IS_CHIP(ap_row_class(&s_cfg.pids[1])));
    TEST_ASSERT_EQUAL_STRING("chip", ap_row_class_name(AP_CLASS_CHIP_BUS));
    TEST_ASSERT_EQUAL(AP_BUS_KEY(AP_BUS_MS, 0), ap_sched_entry_bus(&s_cfg, 1));
    TEST_ASSERT_EQUAL(AP_BUS_KEY(AP_BUS_CH, 2), ap_sched_entry_bus(&s_cfg, 3));

    /* a speed the chip cannot make, and a speed without a bus */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"2201\",\"bus\":\"ms\","
        "\"bus_kbps\":300,"
        "\"parameters\":[{\"name\":\"x\",\"expression\":\"B4\"}]}]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "bus_kbps"));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"2201\","
        "\"bus_kbps\":250,"
        "\"parameters\":[{\"name\":\"x\",\"expression\":\"B4\"}]}]}",
        &s_cfg, err, sizeof(err)));

    /* an unknown bus: the config is refused, the row is not polled on a
       guess */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"2201\",\"bus\":\"hs\","
        "\"parameters\":[{\"name\":\"x\",\"expression\":\"B4\"}]}]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "unknown bus"));

    /* a row of another bus that picks a protocol itself, in its init or
       as its cmd */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"2201\",\"bus\":\"ms\","
        "\"init\":\"ATSP6;ATSH726\","
        "\"parameters\":[{\"name\":\"x\",\"expression\":\"B4\"}]}]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "another bus"));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"STP53\",\"bus\":\"ms\","
        "\"parameters\":[{\"name\":\"x\",\"expression\":\"B4\"}]}]}",
        &s_cfg, err, sizeof(err)));

    /* a J1939 group is read from the listener's store: it has no bus */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"PGN:FEF1\",\"bus\":\"ms\","
        "\"parameters\":[{\"name\":\"x\",\"expression\":\"B4\"}]}]}",
        &s_cfg, err, sizeof(err)));

    /* the same init on a row of the car's protocol is fine, as before */
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(
        "{\"pids\":[{\"name\":\"x\",\"cmd\":\"2201\","
        "\"init\":\"ATSP6;ATSH726\","
        "\"parameters\":[{\"name\":\"x\",\"expression\":\"B4\"}]}]}",
        &s_cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(AP_BUS_OBD, s_cfg.pids[0].bus);
}

/** Six rows in one group of 1 s: OBD, MS, OBD, single-wire, MS, OBD. */
static void six_rows(void)
{
    static const uint8_t BUSES[6] = { AP_BUS_OBD, AP_BUS_MS, AP_BUS_OBD,
                                      AP_BUS_SW, AP_BUS_MS, AP_BUS_OBD };

    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.n_groups = 1;
    strcpy(s_cfg.groups[0].name, "default");
    s_cfg.groups[0].enabled_default = true;
    s_cfg.groups[0].period_ms = 1000;
    s_cfg.n_pids = 6;

    for (int i = 0; i < 6; i++)
    {
        s_cfg.pids[i].enabled = true;
        s_cfg.pids[i].type = AP_PID_CUSTOM;
        s_cfg.pids[i].bus = BUSES[i];
    }

    ap_sched_reset(&s_st, &s_cfg, 0);
    s_st.type_enabled[AP_PID_CUSTOM] = true;
}

void test_bus_rows_are_asked_together(void)
{
    six_rows();

    /* a round as the poller runs it: every row due, the chip on the car's
       protocol. Each pick is run at once (10 ms a row); the bus the chip is
       on and since when follow the picked row, as ap_bus_row_setup and
       ap_bus_leave would set them */
    int64_t now = 1000000; /* all six are due (staggered from 0) */
    uint16_t bus = 0;
    int64_t since = 0;
    int order[6];
    int changes = 0;

    for (int n = 0; n < 6; n++)
    {
        int64_t due = 0;
        int i = ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, bus, since,
                                      now, &due);

        TEST_ASSERT_TRUE(i >= 0);
        TEST_ASSERT_TRUE(due <= now);

        if (ap_sched_entry_bus(&s_cfg, i) != bus)
        {
            /* the start the scheduler counts from: coming home, or leaving
               home; from one other bus to the next the excursion goes on */
            if (bus == 0 || ap_sched_entry_bus(&s_cfg, i) == 0)
            {
                since = now;
            }

            bus = ap_sched_entry_bus(&s_cfg, i);
            changes++;
        }

        order[n] = i;
        now += 10000;
        ap_sched_ran(&s_st, &s_cfg, i, now, true);
    }

    /* the three rows of the car's protocol, then the two of MS CAN, then
       single-wire: two changes of bus for three buses, where the plain
       order (0, 1, 2, 3, 4, 5) would change five times */
    const int want[6] = { 0, 2, 5, 1, 4, 3 };

    TEST_ASSERT_EQUAL_INT_ARRAY(want, order, 6);
    TEST_ASSERT_EQUAL(2, changes);

    /* nothing is due behind the round: the plain pick, for the caller to
       wait for (the chip goes home meanwhile) */
    int64_t due = 0;
    int i = ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, bus, since, now,
                                  &due);

    TEST_ASSERT_EQUAL(0, i);
    TEST_ASSERT_TRUE(due > now);
}

void test_bus_no_bus_holds_the_chip(void)
{
    six_rows();

    /* the rows of the car's protocol at max rate (period 0: due again the
       moment they ran), one MS row waiting. A bus gets one turn per row
       and no more: the MS row is asked after the three, not never */
    s_cfg.pids[0].period_ms = 0;
    s_cfg.pids[2].period_ms = 0;
    s_cfg.pids[5].period_ms = 0;
    s_cfg.groups[0].period_ms = 0;
    s_cfg.pids[1].period_ms = 1000;
    s_cfg.pids[3].enabled = false;
    s_cfg.pids[4].enabled = false;

    int64_t now = 1000000;
    uint16_t bus = 0;
    int64_t since = now;
    int asked_before_ms = 0;
    bool ms_asked = false;

    for (int n = 0; n < 12 && !ms_asked; n++)
    {
        int i = ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, bus, since,
                                      now, NULL);

        TEST_ASSERT_TRUE(i >= 0);

        if (ap_sched_entry_bus(&s_cfg, i) != bus)
        {
            if (bus == 0 || ap_sched_entry_bus(&s_cfg, i) == 0)
            {
                since = now;
            }

            bus = ap_sched_entry_bus(&s_cfg, i);
        }

        ms_asked = (i == 1);
        asked_before_ms += ms_asked ? 0 : 1;
        now += 10000;
        ap_sched_ran(&s_st, &s_cfg, i, now, true);
    }

    TEST_ASSERT_TRUE(ms_asked);
    TEST_ASSERT_TRUE(asked_before_ms <= 3);
}

void test_bus_an_excursion_asks_each_row_once(void)
{
    six_rows();

    /* the rows have drifted apart, as they do by what the rows before them
       took: MS row 1 is due now, MS row 4 in 300 ms, the single-wire row 3
       in 400 ms; the rows of the car's protocol in a second. The chip is
       home. The period is 2 s, so a row may be asked 500 ms early */
    int64_t now = 10000000;

    s_cfg.groups[0].period_ms = 2000;
    s_st.slots[0].due_us = now + 1000000;
    s_st.slots[2].due_us = now + 1000000;
    s_st.slots[5].due_us = now + 1000000;
    s_st.slots[1].due_us = now;
    s_st.slots[4].due_us = now + 300000;
    s_st.slots[3].due_us = now + 400000;

    for (int i = 0; i < 6; i++)
    {
        s_st.slots[i].last_run_us = now - 2000000;
    }

    int64_t due = 0;
    uint16_t ms = AP_BUS_KEY(AP_BUS_MS, 0);

    /* home: the MS row is due, no row of the car's protocol is near */
    TEST_ASSERT_EQUAL(1, ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, 0,
                                               now - 500000, now, &due));
    TEST_ASSERT_EQUAL(now, due);
    ap_sched_ran(&s_st, &s_cfg, 1, now + 10000, true);

    /* away since `now`: the other MS row is asked early (and comes back as
       due now), then the single-wire row on the next bus directly */
    TEST_ASSERT_EQUAL(4, ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, ms,
                                               now, now + 10000, &due));
    TEST_ASSERT_EQUAL(now + 10000, due);
    ap_sched_ran(&s_st, &s_cfg, 4, now + 20000, true);
    TEST_ASSERT_EQUAL(3, ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, ms,
                                               now, now + 20000, &due));
    ap_sched_ran(&s_st, &s_cfg, 3, now + 30000, true);

    /* every row of the other buses was asked in this excursion: the plain
       pick is left, a row of the car's protocol that is not due yet. The
       caller takes the chip home and waits */
    int i = ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL,
                                  AP_BUS_KEY(AP_BUS_SW, 0), now, now + 30000,
                                  &due);

    TEST_ASSERT_TRUE(i == 0 || i == 2 || i == 5);
    TEST_ASSERT_TRUE(due > now + 30000);

    /* asked early once, the rows of the other buses are due together from
       then on: within the 30 ms they took */
    TEST_ASSERT_TRUE(s_st.slots[3].due_us - s_st.slots[1].due_us <= 30000);

    /* a row that is further away than its quarter period is not pulled in */
    s_st.slots[4].due_us = now + 3000000;
    s_st.slots[4].last_run_us = now - 2000000;
    s_st.slots[3].last_run_us = now - 2000000;
    s_st.slots[3].due_us = now + 40000;
    TEST_ASSERT_EQUAL(3, ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, ms,
                                               now, now + 30000, &due));

    /* home, about to leave for a row of another bus: a row of the car's
       protocol that is nearly due goes first, one that is far does not */
    s_st.slots[0].due_us = now + 200000;
    s_st.slots[0].last_run_us = now - 2000000;
    s_st.slots[3].due_us = now;
    TEST_ASSERT_EQUAL(0, ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, 0,
                                               now - 100000, now + 30000,
                                               &due));
    TEST_ASSERT_EQUAL(now + 30000, due);
    s_st.slots[0].due_us = now + 900000;
    TEST_ASSERT_EQUAL(3, ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, 0,
                                               now - 100000, now + 30000,
                                               &due));

    /* the car's rows alone (no row of another bus in the pick): nothing is
       asked early, the scheduler is the one it always was */
    s_st.slots[1].due_us = now + 5000000;
    s_st.slots[3].due_us = now + 5000000;
    s_st.slots[4].due_us = now + 5000000;
    s_st.slots[0].due_us = now + 200000;
    i = ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, 0, now - 100000,
                              now + 30000, &due);
    TEST_ASSERT_EQUAL(0, i);
    TEST_ASSERT_EQUAL(now + 200000, due);
}

void test_bus_max_rate_on_two_buses_comes_home(void)
{
    six_rows();

    /* MS and single-wire rows at max rate (period 0: due the moment they
       ran), the car's rows at 1 s and due. Away, every row is asked once,
       then the car's rows get their turn: two buses at max rate cannot keep
       the chip between them */
    s_cfg.pids[1].period_ms = 0;
    s_cfg.pids[3].period_ms = 0;
    s_cfg.pids[4].period_ms = 0;
    s_cfg.groups[0].period_ms = 0;
    s_cfg.pids[0].period_ms = 1000;
    s_cfg.pids[2].period_ms = 1000;
    s_cfg.pids[5].period_ms = 1000;

    int64_t now = 5000000;
    uint16_t bus = AP_BUS_KEY(AP_BUS_MS, 0);
    int64_t since = now;
    int away = 0;
    int home_row = -1;

    for (int n = 0; n < 10 && home_row < 0; n++)
    {
        int i = ap_sched_next_grouped(&s_st, &s_cfg, AP_CLASS_ALL, bus, since,
                                      now, NULL);

        TEST_ASSERT_TRUE(i >= 0);

        if (ap_sched_entry_bus(&s_cfg, i) == 0)
        {
            home_row = i;
            break;
        }

        bus = ap_sched_entry_bus(&s_cfg, i);
        away++;
        now += 10000;
        ap_sched_ran(&s_st, &s_cfg, i, now, true);
    }

    TEST_ASSERT_EQUAL(3, away); /* rows 1 and 4 on MS CAN, row 3 on single-wire */
    TEST_ASSERT_TRUE(home_row == 0 || home_row == 2 || home_row == 5);
}

void test_bus_can_errors_rest_the_bus(void)
{
    ap_bus_err_t e = { 0 };
    int64_t now = 1000000;

    TEST_ASSERT_FALSE(ap_bus_err_parked(&e, now));

    /* two CAN ERRORs and an answer: nothing */
    TEST_ASSERT_FALSE(ap_bus_err_note(&e, true, now));
    TEST_ASSERT_FALSE(ap_bus_err_note(&e, true, now));
    TEST_ASSERT_FALSE(ap_bus_err_note(&e, false, now));
    TEST_ASSERT_FALSE(ap_bus_err_parked(&e, now));

    /* three in a row: the bus rests for 30 s */
    TEST_ASSERT_FALSE(ap_bus_err_note(&e, true, now));
    TEST_ASSERT_FALSE(ap_bus_err_note(&e, true, now));
    TEST_ASSERT_TRUE(ap_bus_err_note(&e, true, now));
    TEST_ASSERT_EQUAL(1, e.parks);
    TEST_ASSERT_TRUE(ap_bus_err_parked(&e, now));
    TEST_ASSERT_TRUE(ap_bus_err_parked(&e, now + AP_BUS_PARK_MIN_US - 1));
    TEST_ASSERT_FALSE(ap_bus_err_parked(&e, now + AP_BUS_PARK_MIN_US));

    /* still nobody there behind the rest: twice as long, up to ten minutes */
    now += AP_BUS_PARK_MIN_US;
    (void)ap_bus_err_note(&e, true, now);
    (void)ap_bus_err_note(&e, true, now);
    TEST_ASSERT_TRUE(ap_bus_err_note(&e, true, now));
    TEST_ASSERT_EQUAL(2 * AP_BUS_PARK_MIN_US, e.park_us);
    TEST_ASSERT_TRUE(ap_bus_err_parked(&e, now + 2 * AP_BUS_PARK_MIN_US - 1));

    for (int i = 0; i < 12; i++)
    {
        now = e.until_us;
        (void)ap_bus_err_note(&e, true, now);
        (void)ap_bus_err_note(&e, true, now);
        (void)ap_bus_err_note(&e, true, now);
    }

    TEST_ASSERT_EQUAL(AP_BUS_PARK_MAX_US, e.park_us);

    /* a frame gets through (an answer, or NO DATA): it starts from 30 s
       again the next time */
    now = e.until_us;
    TEST_ASSERT_FALSE(ap_bus_err_note(&e, false, now));
    TEST_ASSERT_FALSE(ap_bus_err_parked(&e, now));
    (void)ap_bus_err_note(&e, true, now);
    (void)ap_bus_err_note(&e, true, now);
    TEST_ASSERT_TRUE(ap_bus_err_note(&e, true, now));
    TEST_ASSERT_EQUAL(AP_BUS_PARK_MIN_US, e.park_us);
}

void test_bus_classes_gate_the_pick(void)
{
    six_rows();

    int64_t now = 1000000;

    /* the HS bus guard has no verdict: the rows of the car's protocol are
       not offered, the rows of the other buses are */
    unsigned classes = AP_CLASS_BIT(AP_CLASS_CHIP_BUS);
    int i = ap_sched_next_grouped(&s_st, &s_cfg, classes, 0, 0, now, NULL);

    TEST_ASSERT_EQUAL(1, i);

    /* and the other way round (a pause that is about the chip takes both,
       the caller passes neither) */
    classes = AP_CLASS_BIT(AP_CLASS_CHIP);
    i = ap_sched_next_grouped(&s_st, &s_cfg, classes,
                              AP_BUS_KEY(AP_BUS_MS, 0), now, now, NULL);
    TEST_ASSERT_EQUAL(0, i);

    /* one bus at two speeds is two places: its rows are not asked as one */
    s_cfg.pids[4].bus_rate = 2;
    TEST_ASSERT_NOT_EQUAL(ap_sched_entry_bus(&s_cfg, 1),
                          ap_sched_entry_bus(&s_cfg, 4));

    /* a row of another bus keeps the chip rows' period rules: period 0 is
       max rate, not the passive rows' floor */
    s_cfg.groups[0].period_ms = 0;
    TEST_ASSERT_EQUAL(0, ap_sched_period_us(&s_st, &s_cfg, 1));
}

void run_bus_tests(void)
{
    RUN_TEST(test_bus_words);
    RUN_TEST(test_bus_chain_writes_nothing);
    RUN_TEST(test_bus_speed);
    RUN_TEST(test_bus_chain_allowed);
    RUN_TEST(test_bus_config_parse);
    RUN_TEST(test_bus_rows_are_asked_together);
    RUN_TEST(test_bus_no_bus_holds_the_chip);
    RUN_TEST(test_bus_an_excursion_asks_each_row_once);
    RUN_TEST(test_bus_max_rate_on_two_buses_comes_home);
    RUN_TEST(test_bus_can_errors_rest_the_bus);
    RUN_TEST(test_bus_classes_gate_the_pick);
}
