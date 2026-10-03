/**
 * @file test_j1939_rows.c
 * @brief Host suite for the J1939 rows of autopid (TASK_j1939_wwh.md
 *        phase 5; autopid_j1939_core.c + the config parser + the
 *        scheduler's classes): the `PGN:` grammar, the class of a row, the
 *        class-masked pick and the passive floor, and the expressions that
 *        decode the j1939 component's built-in table (cross-checked
 *        against j1939_spn_decode over the bench truck's frames). Run from
 *        test_main.c's app_main.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "expression_parser.h"
#include "autopid_private.h"

/* ---- the grammar ---------------------------------------------------------------- */

void test_pgn_cmd_parse_shapes(void)
{
    uint32_t pgn = 0;
    int16_t sa = 0;
    bool req = true;

    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse("PGN:F004", &pgn, &sa, &req));
    TEST_ASSERT_EQUAL_HEX32(0xF004, pgn);
    TEST_ASSERT_EQUAL(J1939_ADDR_ANY, sa);
    TEST_ASSERT_FALSE(req);

    /* case, spaces around, a pinned source in decimal and in hex */
    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse(" pgn:feee@0 ", &pgn, &sa,
                                               &req));
    TEST_ASSERT_EQUAL_HEX32(0xFEEE, pgn);
    TEST_ASSERT_EQUAL(0, sa);
    TEST_ASSERT_FALSE(req);

    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse("PGN:FEF1@0x17", &pgn, &sa,
                                               &req));
    TEST_ASSERT_EQUAL(23, sa);

    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse("PGN:FEE5@253?", &pgn, &sa,
                                               &req));
    TEST_ASSERT_EQUAL_HEX32(0xFEE5, pgn);
    TEST_ASSERT_EQUAL(253, sa);
    TEST_ASSERT_TRUE(req);

    /* on request, no source; the 18-bit top; a PDU1 group (EA00 = request) */
    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse("PGN:FEEC?", &pgn, &sa, &req));
    TEST_ASSERT_TRUE(req);
    TEST_ASSERT_EQUAL(J1939_ADDR_ANY, sa);
    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse("PGN:3FFFF", &pgn, &sa, &req));
    TEST_ASSERT_EQUAL_HEX32(0x3FFFF, pgn);
    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse("PGN:EA00", &pgn, &sa, &req));
    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse("PGN:0", &pgn, &sa, &req));
    TEST_ASSERT_EQUAL_HEX32(0, pgn);
}

void test_pgn_cmd_parse_is_not_a_chip_request(void)
{
    uint32_t pgn = 0;
    int16_t sa = 0;
    bool req = false;

    /* every chip request of the tables is "not a PGN command", not an error */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ap_pgn_cmd_parse("010C", &pgn, &sa,
                                                          &req));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ap_pgn_cmd_parse("22F40C", &pgn,
                                                          &sa, &req));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ap_pgn_cmd_parse("ATRV", &pgn, &sa,
                                                          &req));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ap_pgn_cmd_parse("", &pgn, &sa,
                                                          &req));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ap_pgn_cmd_parse("PGNF004", &pgn,
                                                          &sa, &req));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ap_pgn_cmd_parse(NULL, &pgn, &sa,
                                                            &req));
}

void test_pgn_cmd_parse_rejects_malformed(void)
{
    uint32_t pgn = 0;
    int16_t sa = 0;
    bool req = false;
    static const char *const BAD[] =
    {
        "PGN:",            /* no number                                   */
        "PGN:G004",        /* not hex                                     */
        "PGN:40000",       /* above 18 bits                               */
        "PGN:F004F",       /* too many digits (5 digits + room, > max)    */
        "PGN:EA01",        /* a PDU1 group with a destination in it       */
        "PGN:F004@",       /* source without a number                     */
        "PGN:F004@254",    /* the null address                            */
        "PGN:F004@255",    /* the global address                         */
        "PGN:F004@-1",
        "PGN:F004@0x100",
        "PGN:F004??",
        "PGN:F004?@0",     /* the mark goes last                          */
        "PGN:F004 x",
        "PGN:F004;ATSH",
    };

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++)
    {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG,
                                  ap_pgn_cmd_parse(BAD[i], &pgn, &sa, &req),
                                  BAD[i]);
    }
}

void test_pgn_cmd_format_roundtrip(void)
{
    char out[AP_CMD_LEN];
    uint32_t pgn = 0;
    int16_t sa = 0;
    bool req = false;

    TEST_ASSERT_EQUAL_size_t(8, ap_pgn_cmd_format(0xF004, -1, false, out,
                                                 sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("PGN:F004", out);
    TEST_ASSERT_EQUAL_size_t(10, ap_pgn_cmd_format(0xFEEE, 0, false, out,
                                                  sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("PGN:FEEE@0", out);
    TEST_ASSERT_EQUAL_size_t(13, ap_pgn_cmd_format(0xFEE5, 253, true, out,
                                                  sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("PGN:FEE5@253?", out);
    TEST_ASSERT_EQUAL(ESP_OK, ap_pgn_cmd_parse(out, &pgn, &sa, &req));
    TEST_ASSERT_EQUAL_HEX32(0xFEE5, pgn);
    TEST_ASSERT_EQUAL(253, sa);
    TEST_ASSERT_TRUE(req);

    /* the longest spelling fits a cmd field; a short buffer is refused */
    TEST_ASSERT_TRUE(ap_pgn_cmd_format(0x3FFFF, 253, true, out,
                                       sizeof(out)) < AP_CMD_LEN);
    TEST_ASSERT_EQUAL_size_t(0, ap_pgn_cmd_format(0xF004, -1, false, out,
                                                 8));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- the config parser ----------------------------------------------------------- */

void test_config_parse_pgn_rows(void)
{
    const char *json =
        "{\"pids\":["
        "{\"name\":\"eec1\",\"type\":\"std\",\"cmd\":\"PGN:F004\","
        "\"parameters\":[{\"name\":\"EngineSpeed\","
        "\"expression\":\"(B3+B4*256)*0.125\",\"unit\":\"rpm\",\"min\":0,"
        "\"max\":8031.875}]},"
        "{\"name\":\"et1\",\"type\":\"custom\",\"cmd\":\"PGN:FEEE@0\","
        "\"period_ms\":1000,\"parameters\":[{\"name\":\"Coolant\","
        "\"expression\":\"B0-40\"}]},"
        "{\"name\":\"hours\",\"type\":\"specific\",\"cmd\":\"PGN:FEE5?\","
        "\"parameters\":[{\"name\":\"EngineHours\","
        "\"expression\":\"(B0+B1*256+B2*65536+B3*16777216)*0.05\"}]},"
        "{\"name\":\"rpm\",\"type\":\"std\",\"cmd\":\"010C\","
        "\"parameters\":[{\"name\":\"rpm\",\"expression\":\"[B2:B3]/4\"}]}"
        "]}";
    /* ~1 MB struct (2048-param pool): STATIC, never on the task stack */
    static ap_config_t cfg;
    char err[96] = "";

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                              ap_config_parse(json, &cfg, err, sizeof(err)),
                              err);
    TEST_ASSERT_EQUAL(4, cfg.n_pids);

    TEST_ASSERT_TRUE(cfg.pids[0].j1939);
    TEST_ASSERT_EQUAL_HEX32(0xF004, cfg.pids[0].pgn);
    TEST_ASSERT_EQUAL(J1939_ADDR_ANY, cfg.pids[0].j1939_sa);
    TEST_ASSERT_FALSE(cfg.pids[0].j1939_request);
    TEST_ASSERT_EQUAL(AP_PID_STD, cfg.pids[0].type);
    TEST_ASSERT_EQUAL(AP_CLASS_PASSIVE, ap_row_class(&cfg.pids[0]));

    TEST_ASSERT_TRUE(cfg.pids[1].j1939);
    TEST_ASSERT_EQUAL_HEX32(0xFEEE, cfg.pids[1].pgn);
    TEST_ASSERT_EQUAL(0, cfg.pids[1].j1939_sa);
    TEST_ASSERT_EQUAL(AP_PID_CUSTOM, cfg.pids[1].type);
    TEST_ASSERT_EQUAL(AP_CLASS_PASSIVE, ap_row_class(&cfg.pids[1]));

    TEST_ASSERT_TRUE(cfg.pids[2].j1939_request);
    TEST_ASSERT_EQUAL(AP_CLASS_BUS_TX, ap_row_class(&cfg.pids[2]));

    /* the chip row is untouched */
    TEST_ASSERT_FALSE(cfg.pids[3].j1939);
    TEST_ASSERT_EQUAL(0, cfg.pids[3].pgn);
    TEST_ASSERT_EQUAL(AP_CLASS_CHIP, ap_row_class(&cfg.pids[3]));
    TEST_ASSERT_EQUAL_STRING("chip", ap_row_class_name(AP_CLASS_CHIP));
    TEST_ASSERT_EQUAL_STRING("passive", ap_row_class_name(AP_CLASS_PASSIVE));
    TEST_ASSERT_EQUAL_STRING("bus_tx", ap_row_class_name(AP_CLASS_BUS_TX));
    TEST_ASSERT_EQUAL(AP_CLASS_CHIP, ap_row_class(NULL));

    /* the expressions of the rows evaluate over the truck's frames */
    const uint8_t eec1[8] = { 0xF0, 0xAA, 0xA5, 0xE0, 0x2E, 0xFF, 0xFF, 0xFF };
    double v = 0;

    TEST_ASSERT_EQUAL(ESP_OK, expression_parser_eval(
                                  cfg.params[0].expression, eec1, 8, 12.0,
                                  &v));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 1500.0, v); /* 0x2EE0 * 0.125 */
}

void test_config_parse_pgn_rows_refused(void)
{
    static ap_config_t cfg;
    char err[96] = "";

    /* init and rxheader mean nothing to a row served from the store */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse("{\"pids\":[{\"name\":\"a\","
                                      "\"cmd\":\"PGN:F004\","
                                      "\"init\":\"ATSH7E0\",\"parameters\":"
                                      "[{\"name\":\"x\",\"expression\":"
                                      "\"B0\"}]}]}",
                                      &cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "init and rxheader"));
    TEST_ASSERT_EQUAL(0, cfg.n_pids);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse("{\"pids\":[{\"name\":\"a\","
                                      "\"cmd\":\"PGN:F004\","
                                      "\"rxheader\":\"7E8\",\"parameters\":"
                                      "[{\"name\":\"x\",\"expression\":"
                                      "\"B0\"}]}]}",
                                      &cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "init and rxheader"));

    /* a malformed PGN command is a config error, not a chip request */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse("{\"pids\":[{\"name\":\"a\","
                                      "\"cmd\":\"PGN:F004@255\","
                                      "\"parameters\":[{\"name\":\"x\","
                                      "\"expression\":\"B0\"}]}]}",
                                      &cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "bad PGN command"));
}

/* ---- the scheduler's classes -------------------------------------------------------- */

static void two_class_tables(ap_config_t *cfg)
{
    const char *json =
        "{\"groups\":[{\"name\":\"default\",\"period_ms\":1000},"
        "{\"name\":\"max\",\"period_ms\":0}],"
        "\"pids\":["
        "{\"name\":\"chip\",\"cmd\":\"010C\",\"parameters\":"
        "[{\"name\":\"rpm\",\"expression\":\"[B2:B3]/4\"}]},"
        "{\"name\":\"pass\",\"cmd\":\"PGN:F004\",\"parameters\":"
        "[{\"name\":\"EngineSpeed\",\"expression\":\"(B3+B4*256)*0.125\"}]},"
        "{\"name\":\"asked\",\"cmd\":\"PGN:FEE5?\",\"parameters\":"
        "[{\"name\":\"EngineHours\",\"expression\":\"B0\"}]},"
        "{\"name\":\"fast\",\"cmd\":\"PGN:FEF1\",\"period_ms\":0,"
        "\"group\":\"max\",\"parameters\":"
        "[{\"name\":\"WheelBasedVehicleSpeed\",\"expression\":\"B1\"}]}"
        "],\"filters\":[{\"frame_id\":291,\"monitor_ms\":500,"
        "\"parameters\":[{\"name\":\"f\",\"expression\":\"B0\"}]}]}";

    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(json, cfg, NULL, 0));
    TEST_ASSERT_EQUAL(4, cfg->n_pids);
    TEST_ASSERT_EQUAL(1, cfg->n_filters);
}

void test_sched_classes_masked_pick(void)
{
    static ap_config_t cfg;
    static ap_sched_t st;

    two_class_tables(&cfg);
    memset(&st, 0, sizeof(st));
    st.type_enabled[0] = st.type_enabled[1] = st.type_enabled[2] = true;
    ap_sched_reset(&st, &cfg, 0);

    /* entry classes: pid 0 chip, 1 passive, 2 bus_tx, 3 passive, filter chip */
    TEST_ASSERT_EQUAL(AP_CLASS_CHIP, ap_sched_entry_class(&cfg, 0));
    TEST_ASSERT_EQUAL(AP_CLASS_PASSIVE, ap_sched_entry_class(&cfg, 1));
    TEST_ASSERT_EQUAL(AP_CLASS_BUS_TX, ap_sched_entry_class(&cfg, 2));
    TEST_ASSERT_EQUAL(AP_CLASS_PASSIVE, ap_sched_entry_class(&cfg, 3));
    TEST_ASSERT_EQUAL(AP_CLASS_CHIP, ap_sched_entry_class(&cfg, 4));

    /* every class: the stagger makes entry 0 first */
    int64_t due = -1;

    TEST_ASSERT_EQUAL(0, ap_sched_next(&st, &cfg, &due));
    TEST_ASSERT_EQUAL(0, ap_sched_next_of(&st, &cfg, AP_CLASS_ALL, &due));

    /* the chip is held: only the passive rows are offered, earliest first */
    unsigned passive = AP_CLASS_BIT(AP_CLASS_PASSIVE) |
                       AP_CLASS_BIT(AP_CLASS_BUS_TX);

    TEST_ASSERT_EQUAL(1, ap_sched_next_of(&st, &cfg, passive, &due));
    TEST_ASSERT_EQUAL(1 * AP_SCHED_STAGGER_US, due);

    /* only the chip's: pid 0, then (after it ran) the filter */
    TEST_ASSERT_EQUAL(0, ap_sched_next_of(&st, &cfg,
                                          AP_CLASS_BIT(AP_CLASS_CHIP), &due));
    ap_sched_ran(&st, &cfg, 0, 5 * AP_SCHED_STAGGER_US, true);
    TEST_ASSERT_EQUAL(4, ap_sched_next_of(&st, &cfg,
                                          AP_CLASS_BIT(AP_CLASS_CHIP), &due));

    /* no class at all: nothing */
    TEST_ASSERT_EQUAL(-1, ap_sched_next_of(&st, &cfg, 0, &due));

    /* a disabled passive row leaves the mask's pick to the next one */
    cfg.pids[1].enabled = false;
    TEST_ASSERT_EQUAL(2, ap_sched_next_of(&st, &cfg, passive, &due));
    cfg.pids[1].enabled = true;

    /* the type gate still applies to a PGN row */
    st.type_enabled[AP_PID_CUSTOM] = false;
    TEST_ASSERT_EQUAL(-1, ap_sched_next_of(&st, &cfg, passive, &due));
    st.type_enabled[AP_PID_CUSTOM] = true;
}

void test_sched_passive_floor(void)
{
    static ap_config_t cfg;
    static ap_sched_t st;

    two_class_tables(&cfg);
    memset(&st, 0, sizeof(st));
    st.type_enabled[0] = st.type_enabled[1] = st.type_enabled[2] = true;
    ap_sched_reset(&st, &cfg, 0);

    /* the max-rate group: a chip row would be due again at once (0); the
       passive row in it looks every AP_PASSIVE_FLOOR_MS */
    TEST_ASSERT_EQUAL_INT64(1000 * 1000, ap_sched_period_us(&st, &cfg, 1));
    TEST_ASSERT_EQUAL_INT64((int64_t)AP_PASSIVE_FLOOR_MS * 1000,
                            ap_sched_period_us(&st, &cfg, 3));

    /* a runtime override to max rate (period 0) on the default group: the
       chip row goes to 0, the passive one to the floor */
    ap_sched_group_set(&st, 0, true, 0);
    TEST_ASSERT_EQUAL_INT64(0, ap_sched_period_us(&st, &cfg, 0));
    TEST_ASSERT_EQUAL_INT64((int64_t)AP_PASSIVE_FLOOR_MS * 1000,
                            ap_sched_period_us(&st, &cfg, 1));
    ap_sched_group_restore(&st, &cfg, 0);

    /* after a run the passive row is due one floor later, not now */
    ap_sched_ran(&st, &cfg, 3, 1000000, true);
    TEST_ASSERT_EQUAL_INT64(1000000 + (int64_t)AP_PASSIVE_FLOOR_MS * 1000,
                            st.slots[3].due_us);

    /* a group nobody sends backs off like a silent PID: x4 of 1 s */
    for (int i = 0; i < AP_SCHED_BACKOFF_STREAK; i++)
    {
        ap_sched_ran(&st, &cfg, 1, 2000000, false);
    }

    TEST_ASSERT_EQUAL_INT64(4 * 1000 * 1000, ap_sched_period_us(&st, &cfg, 1));
}

/* ---- the built-in table's expressions ---------------------------------------------- */

void test_j1939_std_expression_shapes(void)
{
    char expr[AP_EXPR_LEN];

    const j1939_spn_t *spd = j1939_spn_find("EngineSpeed");
    const j1939_spn_t *ect = j1939_spn_find("EngineCoolantTemperature");
    const j1939_spn_t *pedal = j1939_spn_find("AccelPedalPosition1");
    const j1939_spn_t *gear = j1939_spn_find("TransCurrentGear");

    TEST_ASSERT_NOT_NULL(spd);
    TEST_ASSERT_NOT_NULL(ect);
    TEST_ASSERT_NOT_NULL(pedal);
    TEST_ASSERT_NOT_NULL(gear);

    TEST_ASSERT_TRUE(ap_j1939_std_expression(spd, expr, sizeof(expr)) > 0);
    TEST_ASSERT_EQUAL_STRING("(B3+B4*256)*0.125", expr);
    TEST_ASSERT_TRUE(ap_j1939_std_expression(ect, expr, sizeof(expr)) > 0);
    TEST_ASSERT_EQUAL_STRING("B0-40", expr);
    TEST_ASSERT_TRUE(ap_j1939_std_expression(pedal, expr, sizeof(expr)) > 0);
    TEST_ASSERT_EQUAL_STRING("B1*0.4", expr);
    TEST_ASSERT_TRUE(ap_j1939_std_expression(gear, expr, sizeof(expr)) > 0);
    TEST_ASSERT_EQUAL_STRING("B3-125", expr);

    /* a field inside a byte, and a 32-bit word */
    j1939_spn_t bits = *spd;

    bits.byte = 4;
    bits.bit = 2;
    bits.bits = 2;
    bits.scale = 1.0;
    bits.offset = 0.0;
    TEST_ASSERT_TRUE(ap_j1939_std_expression(&bits, expr, sizeof(expr)) > 0);
    TEST_ASSERT_EQUAL_STRING("(B4>>2)&3", expr);
    bits.scale = 0.4;
    TEST_ASSERT_TRUE(ap_j1939_std_expression(&bits, expr, sizeof(expr)) > 0);
    TEST_ASSERT_EQUAL_STRING("((B4>>2)&3)*0.4", expr);

    const j1939_spn_t *hours = j1939_spn_find("EngineTotalHours");

    TEST_ASSERT_NOT_NULL(hours);
    TEST_ASSERT_TRUE(ap_j1939_std_expression(hours, expr, sizeof(expr)) > 0);
    TEST_ASSERT_EQUAL_STRING("(B0+B1*256+B2*65536+B3*16777216)*0.05", expr);

    /* refused: a field across bytes that is not whole, a short buffer */
    bits.bit = 6;
    bits.bits = 4;
    TEST_ASSERT_EQUAL_size_t(0, ap_j1939_std_expression(&bits, expr,
                                                       sizeof(expr)));
    TEST_ASSERT_EQUAL_size_t(0, ap_j1939_std_expression(spd, expr, 8));
    TEST_ASSERT_EQUAL_size_t(0, ap_j1939_std_expression(NULL, expr,
                                                       sizeof(expr)));
}

void test_j1939_std_expression_whole_table_crosscheck(void)
{
    /* every row's expression parses, and over a frame of a valid raw
       value it equals the j1939 component's own decoder; the operational
       range then drops the "not available" frame */
    size_t count = 0;
    const j1939_spn_t *tab = j1939_spn_table(&count);

    TEST_ASSERT_NOT_NULL(tab);
    TEST_ASSERT_TRUE(count >= 20);

    for (size_t i = 0; i < count; i++)
    {
        const j1939_spn_t *spn = &tab[i];
        char expr[AP_EXPR_LEN];
        char msg[96];

        snprintf(msg, sizeof(msg), "%s", spn->name);
        TEST_ASSERT_TRUE_MESSAGE(ap_j1939_std_expression(spn, expr,
                                                         sizeof(expr)) > 0,
                                 msg);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, expression_parser_check(expr, NULL,
                                                                  NULL, 0),
                                  msg);

        /* a frame carrying raw = half the valid range, little-endian */
        uint8_t frame[8];
        uint32_t raw_max = (spn->bits >= 32) ? 0xFAFFFFFFu
                                             : ((1u << spn->bits) - 1u);
        uint32_t raw = raw_max / 2;

        memset(frame, 0xFF, sizeof(frame));

        if (spn->bits % 8 == 0 && spn->bit == 0)
        {
            for (unsigned b = 0; b < spn->bits / 8; b++)
            {
                frame[spn->byte + b] = (uint8_t)(raw >> (8 * b));
            }
        }
        else
        {
            frame[spn->byte] = (uint8_t)((frame[spn->byte] &
                                          ~(((1u << spn->bits) - 1u)
                                            << spn->bit)) |
                                         (raw << spn->bit));
        }

        double ours = 0, theirs = 0;
        j1939_raw_t cls = j1939_spn_decode(spn, frame, 8, &theirs);

        TEST_ASSERT_EQUAL_MESSAGE(J1939_RAW_VALID, cls, msg);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, expression_parser_eval(
                                              expr, frame, 8, 12.0, &ours),
                                  msg);
        TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(fabs(theirs) * 1e-9 + 1e-6, theirs,
                                          ours, msg);
        TEST_ASSERT_TRUE_MESSAGE(ours >= spn->min && ours <= spn->max, msg);

        /* all ones = not available: the expression yields a number above
           max, which the row's clamp drops */
        memset(frame, 0xFF, sizeof(frame));
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, expression_parser_eval(
                                              expr, frame, 8, 12.0, &ours),
                                  msg);
        TEST_ASSERT_TRUE_MESSAGE(ours > spn->max, msg);
    }
}

void run_j1939_rows_tests(void)
{
    RUN_TEST(test_pgn_cmd_parse_shapes);
    RUN_TEST(test_pgn_cmd_parse_is_not_a_chip_request);
    RUN_TEST(test_pgn_cmd_parse_rejects_malformed);
    RUN_TEST(test_pgn_cmd_format_roundtrip);
    RUN_TEST(test_config_parse_pgn_rows);
    RUN_TEST(test_config_parse_pgn_rows_refused);
    RUN_TEST(test_sched_classes_masked_pick);
    RUN_TEST(test_sched_passive_floor);
    RUN_TEST(test_j1939_std_expression_shapes);
    RUN_TEST(test_j1939_std_expression_whole_table_crosscheck);
}
