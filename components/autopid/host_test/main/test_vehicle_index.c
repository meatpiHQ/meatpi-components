/**
 * @file test_vehicle_index.c
 * @brief Host suite for autopid_vehicle_index.c (TASK_quick_setup.md,
 *        second pass): key derivation, find by key / VIN / fingerprint,
 *        the fingerprint SUBSET rule, the LRU eviction pick, the
 *        change-guarded last_seen touch and the vehicles.json round trip
 *        with its bounds. Run from test_main.c's app_main.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

#define VIN_A "1WCAN0FW0P0000001"       /* the ECU simulator               */
#define VIN_B "WVWZZZ1KZ7W000001"
#define VIN_C "KMHL14JA5PA000003"

static const ap_veh_ecu_t ECUS_READY[] =
{
    { 0x7E8, 0xBE7FB813 }, { 0x7E9, 0x80000001 }, { 0x7EA, 0x00000000 },
};
static const ap_veh_ecu_t ECUS_ACCESSORY[] =
{
    { 0x7E8, 0xBE7FB813 }, { 0x7E9, 0x80000001 },
};
static const ap_veh_ecu_t ECUS_OTHER_MAIN[] =
{
    { 0x7E8, 0xBE7FB812 }, { 0x7E9, 0x80000001 },
};
static const ap_veh_ecu_t ECUS_OTHER_AUX[] =
{
    { 0x7E8, 0xBE7FB813 }, { 0x7E9, 0x80000003 },
};

static ap_veh_entry_t entry(const char *vin, const ap_veh_ecu_t *ecus,
                            size_t n, int64_t first, int64_t last)
{
    ap_veh_entry_t e;

    memset(&e, 0, sizeof(e));
    snprintf(e.vin, sizeof(e.vin), "%s", vin);

    if (ecus != NULL && n > 0)
    {
        memcpy(e.ecus, ecus, n * sizeof(*ecus));
        e.n_ecus = (uint8_t)n;
        ap_veh_fingerprint(ecus, n, e.fingerprint);
    }

    ap_vidx_key_for(e.vin, e.fingerprint, e.key);
    ap_vidx_default_name(e.vin, e.fingerprint, e.name);
    snprintf(e.protocol, sizeof(e.protocol), "6");
    e.first_seen = first;
    e.last_seen = last;
    return e;
}

/* ---- keys + names ------------------------------------------------------------- */

void test_vidx_key_and_name(void)
{
    char key[AP_VEH_KEY_LEN], name[AP_VEH_NAME_LEN], fp[AP_FP_LEN];

    ap_veh_fingerprint(ECUS_READY, 3, fp);

    /* the VIN is the key when it is valid, the fingerprint otherwise */
    ap_vidx_key_for(VIN_A, fp, key);
    TEST_ASSERT_EQUAL_STRING(VIN_A, key);
    ap_vidx_key_for("", fp, key);
    TEST_ASSERT_EQUAL(11, strlen(key));
    TEST_ASSERT_EQUAL_STRING_LEN("fp:", key, 3);
    TEST_ASSERT_EQUAL_STRING(fp, key + 3);
    ap_vidx_key_for("not a vin", fp, key);
    TEST_ASSERT_EQUAL_STRING_LEN("fp:", key, 3);
    ap_vidx_key_for(NULL, "", key);
    TEST_ASSERT_EQUAL_STRING("", key);
    ap_vidx_key_for("", "DEADBEEF", key);        /* upper case = invalid */
    TEST_ASSERT_EQUAL_STRING("", key);

    /* key validity = what a route / a file name may carry */
    TEST_ASSERT_TRUE(ap_vidx_key_valid(VIN_A));
    TEST_ASSERT_TRUE(ap_vidx_key_valid("fp:deadbeef"));
    TEST_ASSERT_FALSE(ap_vidx_key_valid("fp:DEADBEEF"));
    TEST_ASSERT_FALSE(ap_vidx_key_valid("fp:deadbee"));
    TEST_ASSERT_FALSE(ap_vidx_key_valid("../config"));
    TEST_ASSERT_FALSE(ap_vidx_key_valid("1WCAN0FW0P000000"));
    TEST_ASSERT_FALSE(ap_vidx_key_valid(""));
    TEST_ASSERT_FALSE(ap_vidx_key_valid(NULL));

    /* default names: "<WMI> <last 4>" / "Car <4 hex>" */
    ap_vidx_default_name(VIN_A, "", name);
    TEST_ASSERT_EQUAL_STRING("1WC 0001", name);
    ap_vidx_default_name("", "deadbeef", name);
    TEST_ASSERT_EQUAL_STRING("Car dead", name);
    ap_vidx_default_name("", "", name);
    TEST_ASSERT_EQUAL_STRING("Vehicle", name);
}

/* ---- find + the subset rule ------------------------------------------------------ */

void test_vidx_find_by_key_vin_fp(void)
{
    ap_veh_index_t idx;
    ap_veh_entry_t a = entry(VIN_A, ECUS_READY, 3, 100, 100);
    ap_veh_entry_t b = entry("", ECUS_OTHER_MAIN, 2, 200, 200);

    ap_vidx_init(&idx);
    TEST_ASSERT_EQUAL(0, idx.n);
    TEST_ASSERT_EQUAL(-1, idx.current);
    TEST_ASSERT_EQUAL(0, ap_vidx_add(&idx, &a, NULL));
    TEST_ASSERT_EQUAL(1, ap_vidx_add(&idx, &b, NULL));
    TEST_ASSERT_EQUAL(-1, ap_vidx_add(&idx, &a, NULL)); /* duplicate key */
    TEST_ASSERT_EQUAL(2, idx.n);

    TEST_ASSERT_EQUAL(0, ap_vidx_find_key(&idx, VIN_A));
    TEST_ASSERT_EQUAL(1, ap_vidx_find_key(&idx, b.key));
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_key(&idx, VIN_B));
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_key(&idx, ""));
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_key(NULL, VIN_A));

    TEST_ASSERT_EQUAL(0, ap_vidx_find_vin(&idx, VIN_A));
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_vin(&idx, VIN_B));
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_vin(&idx, ""));  /* "" never matches */

    TEST_ASSERT_EQUAL(0, ap_vidx_find_fp(&idx, a.fingerprint));
    TEST_ASSERT_EQUAL(1, ap_vidx_find_fp(&idx, b.fingerprint));
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_fp(&idx, "00000000"));
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_fp(&idx, NULL));
}

void test_vidx_subset_rule(void)
{
    /* the same car in accessory mode (fewer ECUs) IS the same car */
    TEST_ASSERT_TRUE(ap_veh_ecus_subset(ECUS_READY, 3, ECUS_ACCESSORY, 2));
    TEST_ASSERT_TRUE(ap_veh_ecus_subset(ECUS_ACCESSORY, 2, ECUS_READY, 3));
    TEST_ASSERT_TRUE(ap_veh_ecus_subset(ECUS_READY, 3, ECUS_READY, 3));

    /* a different main ECU bitmap = another car, however many match */
    TEST_ASSERT_FALSE(ap_veh_ecus_subset(ECUS_READY, 3, ECUS_OTHER_MAIN, 2));
    /* a shared main ECU but a differing secondary = not a subset */
    TEST_ASSERT_FALSE(ap_veh_ecus_subset(ECUS_READY, 3, ECUS_OTHER_AUX, 2));

    /* headers-off prints (one UINT32_MAX entry) compare as equal sets */
    ap_veh_ecu_t off1[] = { { UINT32_MAX, 0xBE7FB813 } };
    ap_veh_ecu_t off2[] = { { UINT32_MAX, 0xBE7FB812 } };

    TEST_ASSERT_TRUE(ap_veh_ecus_subset(off1, 1, off1, 1));
    TEST_ASSERT_FALSE(ap_veh_ecus_subset(off1, 1, off2, 1));

    /* empty sets never match */
    TEST_ASSERT_FALSE(ap_veh_ecus_subset(ECUS_READY, 3, ECUS_READY, 0));
    TEST_ASSERT_FALSE(ap_veh_ecus_subset(NULL, 0, ECUS_READY, 3));
}

void test_vidx_match_policy(void)
{
    ap_veh_index_t idx;
    ap_veh_entry_t a = entry(VIN_A, ECUS_READY, 3, 100, 100);  /* VIN car  */
    ap_veh_entry_t b = entry("", ECUS_OTHER_MAIN, 2, 200, 200); /* fp car  */
    char fp_ready[AP_FP_LEN], fp_acc[AP_FP_LEN], fp_other[AP_FP_LEN];
    bool exact = false;

    ap_veh_fingerprint(ECUS_READY, 3, fp_ready);
    ap_veh_fingerprint(ECUS_ACCESSORY, 2, fp_acc);
    ap_veh_fingerprint(ECUS_OTHER_MAIN, 2, fp_other);

    ap_vidx_init(&idx);
    ap_vidx_add(&idx, &a, NULL);
    ap_vidx_add(&idx, &b, NULL);

    /* case C: the VIN decides alone, whatever the responders say */
    TEST_ASSERT_EQUAL(0, ap_vidx_match(&idx, VIN_A, ECUS_OTHER_MAIN, 2,
                                       fp_other, &exact));
    TEST_ASSERT_TRUE(exact);

    /* case E/F: another VIN = another car, even on the same ECU set */
    TEST_ASSERT_EQUAL(-1, ap_vidx_match(&idx, VIN_B, ECUS_READY, 3,
                                        fp_ready, &exact));

    /* case G: no VIN, exact fingerprint */
    TEST_ASSERT_EQUAL(0, ap_vidx_match(&idx, "", ECUS_READY, 3, fp_ready,
                                       &exact));
    TEST_ASSERT_TRUE(exact);
    TEST_ASSERT_EQUAL(1, ap_vidx_match(&idx, NULL, ECUS_OTHER_MAIN, 2,
                                       fp_other, &exact));

    /* case H: fingerprint drift, the subset rule finds the VIN car */
    TEST_ASSERT_EQUAL(0, ap_vidx_match(&idx, "", ECUS_ACCESSORY, 2, fp_acc,
                                       &exact));
    TEST_ASSERT_FALSE(exact);

    /* case D/no identity: nothing to compare = no match */
    TEST_ASSERT_EQUAL(-1, ap_vidx_match(&idx, "", NULL, 0, "", &exact));
    TEST_ASSERT_TRUE(exact);

    /* a fp-keyed car whose VIN answers later is adopted, not duplicated */
    TEST_ASSERT_EQUAL(1, ap_vidx_match(&idx, VIN_C, ECUS_OTHER_MAIN, 2,
                                       fp_other, &exact));
    TEST_ASSERT_TRUE(exact);
    /* ... but never a VIN car: VIN_C on car A's responders is a new car */
    TEST_ASSERT_EQUAL(-1, ap_vidx_match(&idx, VIN_C, ECUS_READY, 3,
                                        fp_ready, &exact));

    TEST_ASSERT_EQUAL(-1, ap_vidx_match(NULL, VIN_A, NULL, 0, NULL, NULL));
}

/* ---- LRU eviction + remove ---------------------------------------------------- */

void test_vidx_lru_and_eviction(void)
{
    ap_veh_index_t idx;
    ap_veh_entry_t evicted;
    char vin[AP_VIN_LEN];

    ap_vidx_init(&idx);
    TEST_ASSERT_EQUAL(-1, ap_vidx_lru(&idx));

    /* eight cars, seen at 1000, 2000, ... 8000; car 3 was first seen
       earlier than its twin on last_seen to exercise the tie-break */
    for (int i = 0; i < AP_VEH_MAX; i++)
    {
        snprintf(vin, sizeof(vin), "1WCAN0FW0P000000%d", i + 1);

        ap_veh_entry_t e = entry(vin, NULL, 0, 1000 * (i + 1),
                                 1000 * (i + 1));

        TEST_ASSERT_EQUAL(i, ap_vidx_add(&idx, &e, &evicted));
        TEST_ASSERT_EQUAL_STRING("", evicted.key);
    }

    TEST_ASSERT_EQUAL(AP_VEH_MAX, idx.n);

    /* the oldest last_seen goes, never the current car */
    TEST_ASSERT_EQUAL(0, ap_vidx_lru(&idx));
    idx.current = 0;
    TEST_ASSERT_EQUAL(1, ap_vidx_lru(&idx));

    /* a tie on last_seen: the older first_seen goes */
    idx.v[5].last_seen = 2000;
    idx.v[5].first_seen = 500;
    TEST_ASSERT_EQUAL(5, ap_vidx_lru(&idx));
    idx.v[5].last_seen = 6000;
    idx.v[5].first_seen = 6000;

    /* the ninth car evicts entry 1 and keeps `current` pointing at car 1 */
    ap_veh_entry_t nine = entry(VIN_B, NULL, 0, 9000, 9000);
    int i = ap_vidx_add(&idx, &nine, &evicted);

    TEST_ASSERT_EQUAL(AP_VEH_MAX - 1, i);
    TEST_ASSERT_EQUAL_STRING("1WCAN0FW0P0000002", evicted.key);
    TEST_ASSERT_EQUAL(AP_VEH_MAX, idx.n);
    TEST_ASSERT_EQUAL(0, idx.current);
    TEST_ASSERT_EQUAL_STRING("1WCAN0FW0P0000001", idx.v[0].key);
    TEST_ASSERT_EQUAL_STRING(VIN_B, idx.v[AP_VEH_MAX - 1].key);
    TEST_ASSERT_EQUAL(-1, ap_vidx_find_key(&idx, "1WCAN0FW0P0000002"));

    /* evicting around the current car moves its index: car 6 is current
       (index 4), car 1 at index 0 (1000) is now the oldest and goes */
    idx.current = 4;
    i = ap_vidx_lru(&idx);
    TEST_ASSERT_EQUAL(0, i);

    ap_veh_entry_t ten = entry(VIN_C, NULL, 0, 10000, 10000);

    TEST_ASSERT_EQUAL(AP_VEH_MAX - 1, ap_vidx_add(&idx, &ten, &evicted));
    TEST_ASSERT_EQUAL_STRING("1WCAN0FW0P0000001", evicted.key);
    TEST_ASSERT_EQUAL(3, idx.current);
    TEST_ASSERT_EQUAL_STRING("1WCAN0FW0P0000006", idx.v[3].key);

    /* remove: the current car removed = no current; others shift */
    ap_vidx_remove(&idx, 3);
    TEST_ASSERT_EQUAL(-1, idx.current);
    TEST_ASSERT_EQUAL(AP_VEH_MAX - 1, idx.n);
    idx.current = 5;
    ap_vidx_remove(&idx, 0);
    TEST_ASSERT_EQUAL(4, idx.current);
    ap_vidx_remove(&idx, 99);               /* out of range: no-op      */
    TEST_ASSERT_EQUAL(AP_VEH_MAX - 2, idx.n);

    /* a lone current car cannot be evicted */
    ap_vidx_init(&idx);
    ap_vidx_add(&idx, &nine, NULL);
    idx.current = 0;
    TEST_ASSERT_EQUAL(-1, ap_vidx_lru(&idx));
}

/* ---- the last_seen touch guard ----------------------------------------------------- */

void test_vidx_touch_once_a_day(void)
{
    ap_veh_entry_t e = entry(VIN_A, NULL, 0, 0, 0);

    /* clock unset: never a write */
    TEST_ASSERT_FALSE(ap_vidx_touch(&e, 0));
    TEST_ASSERT_EQUAL_INT64(0, e.last_seen);

    /* first touch with a clock: both stamps, one write */
    TEST_ASSERT_TRUE(ap_vidx_touch(&e, 1759300000));
    TEST_ASSERT_EQUAL_INT64(1759300000, e.first_seen);
    TEST_ASSERT_EQUAL_INT64(1759300000, e.last_seen);

    /* the same day (23 h 59 m later): no write */
    TEST_ASSERT_FALSE(ap_vidx_touch(&e, 1759300000 + 86399));
    TEST_ASSERT_EQUAL_INT64(1759300000, e.last_seen);

    /* a day later: one write */
    TEST_ASSERT_TRUE(ap_vidx_touch(&e, 1759300000 + 86400));
    TEST_ASSERT_EQUAL_INT64(1759300000 + 86400, e.last_seen);
    TEST_ASSERT_EQUAL_INT64(1759300000, e.first_seen);

    /* the clock went backwards: no write */
    TEST_ASSERT_FALSE(ap_vidx_touch(&e, 1759300000));
    TEST_ASSERT_FALSE(ap_vidx_touch(NULL, 1759300000));
}

/* ---- the responder table as text ---------------------------------------------------- */

void test_vidx_ecus_string(void)
{
    char s[AP_VEH_ECUS_STR_LEN];
    ap_veh_ecu_t back[AP_VEH_ECUS_MAX];

    TEST_ASSERT_EQUAL(3, ap_veh_ecus_to_str(ECUS_READY, 3, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("7E8:BE7FB813,7E9:80000001,7EA:00000000", s);
    TEST_ASSERT_EQUAL(3, ap_veh_ecus_from_str(s, back, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(0x7E8, back[0].id);
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813, back[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x7EA, back[2].id);
    TEST_ASSERT_EQUAL_HEX32(0, back[2].bitmap);

    /* headers off + 29-bit */
    ap_veh_ecu_t mixed[] = { { UINT32_MAX, 0x80000001 }, { 0x18DAF110, 1 } };

    TEST_ASSERT_EQUAL(2, ap_veh_ecus_to_str(mixed, 2, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("*:80000001,18DAF110:00000001", s);
    TEST_ASSERT_EQUAL(2, ap_veh_ecus_from_str(s, back, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, back[0].id);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, back[1].id);

    /* empty, junk tokens skipped, the cap honoured */
    TEST_ASSERT_EQUAL(0, ap_veh_ecus_to_str(NULL, 0, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("", s);
    TEST_ASSERT_EQUAL(0, ap_veh_ecus_from_str("", back, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(1, ap_veh_ecus_from_str("junk,7E8:1,:,7E9", back,
                                              AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL_HEX32(0x7E8, back[0].id);
    TEST_ASSERT_EQUAL(1, ap_veh_ecus_from_str("7E8:1,7E9:2", back, 1));
    TEST_ASSERT_EQUAL(-1, ap_veh_ecus_to_str(ECUS_READY, 3, s, 20));
}

/* ---- vehicles.json ----------------------------------------------------------------- */

void test_vidx_json_roundtrip(void)
{
    ap_veh_index_t idx, back;
    ap_veh_entry_t a = entry(VIN_A, ECUS_READY, 3, 1759300000, 1759386400);
    ap_veh_entry_t b = entry("", ECUS_OTHER_MAIN, 2, 1759000000, 1759000000);
    char body[AP_VEH_INDEX_JSON_MAX];

    snprintf(a.name, sizeof(a.name), "Sim \"car\"");  /* escaping     */
    snprintf(a.chip_protocol, sizeof(a.chip_protocol), "6");
    snprintf(a.profile, sizeof(a.profile), "VW: ID.3");
    snprintf(a.specific_init, sizeof(a.specific_init),
             "ATSP7;ATSH17FC007B;ATCRA17FE007B");
    a.std_supported = 42;
    a.scan_ts = 1759300000;
    b.pending_profile = true;
    snprintf(b.protocol, sizeof(b.protocol), "7");
    b.dialect = AP_DIALECT_UDS;                       /* a WWH-OBD van */

    ap_vidx_init(&idx);
    ap_vidx_add(&idx, &a, NULL);
    ap_vidx_add(&idx, &b, NULL);
    idx.current = 1;

    int n = ap_vidx_to_json(&idx, body, sizeof(body));

    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL(n, (int)strlen(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"version\":1"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"current\":\"fp:"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"ecus\":\"7E8:BE7FB813,7E9:80000001,"
                                      "7EA:00000000\""));
    TEST_ASSERT_NULL(strstr(body, "\"current\":true"));  /* file: no flag */
    TEST_ASSERT_NOT_NULL(strstr(body, "\"dialect\":\"obd2\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"dialect\":\"uds\""));

    TEST_ASSERT_TRUE(ap_vidx_from_json(body, &back));
    TEST_ASSERT_EQUAL(2, back.n);
    TEST_ASSERT_EQUAL(1, back.current);
    TEST_ASSERT_EQUAL_STRING(VIN_A, back.v[0].key);
    TEST_ASSERT_EQUAL_STRING(VIN_A, back.v[0].vin);
    TEST_ASSERT_EQUAL_STRING(a.fingerprint, back.v[0].fingerprint);
    TEST_ASSERT_EQUAL_STRING("Sim \"car\"", back.v[0].name);
    TEST_ASSERT_EQUAL_STRING("6", back.v[0].protocol);
    TEST_ASSERT_EQUAL_STRING("6", back.v[0].chip_protocol);
    TEST_ASSERT_EQUAL_STRING("VW: ID.3", back.v[0].profile);
    TEST_ASSERT_EQUAL_STRING("ATSP7;ATSH17FC007B;ATCRA17FE007B",
                             back.v[0].specific_init);
    TEST_ASSERT_EQUAL(3, back.v[0].n_ecus);
    TEST_ASSERT_EQUAL_HEX32(0x7E9, back.v[0].ecus[1].id);
    TEST_ASSERT_EQUAL(42, back.v[0].std_supported);
    TEST_ASSERT_FALSE(back.v[0].pending_profile);
    TEST_ASSERT_EQUAL_INT64(1759300000, back.v[0].first_seen);
    TEST_ASSERT_EQUAL_INT64(1759386400, back.v[0].last_seen);
    TEST_ASSERT_EQUAL_INT64(1759300000, back.v[0].scan_ts);
    TEST_ASSERT_EQUAL_STRING(b.key, back.v[1].key);
    TEST_ASSERT_EQUAL_STRING("", back.v[1].vin);
    TEST_ASSERT_EQUAL_STRING("7", back.v[1].protocol);
    TEST_ASSERT_TRUE(back.v[1].pending_profile);
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, back.v[0].dialect);
    TEST_ASSERT_EQUAL(AP_DIALECT_UDS, back.v[1].dialect);

    /* a store written before dialects existed has no such field: every
       car in it is an OBD-II car; an unknown word is one too */
    TEST_ASSERT_TRUE(ap_vidx_from_json(
        "{\"version\":1,\"current\":\"\",\"vehicles\":["
        "{\"key\":\"" VIN_A "\",\"vin\":\"" VIN_A "\",\"protocol\":\"6\"},"
        "{\"key\":\"fp:0123abcd\",\"fingerprint\":\"0123abcd\","
        "\"dialect\":\"klingon\"}]}",
        &back));
    TEST_ASSERT_EQUAL(2, back.n);
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, back.v[0].dialect);
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, back.v[1].dialect);

    /* an empty index round-trips empty */
    ap_vidx_init(&idx);
    TEST_ASSERT_TRUE(ap_vidx_to_json(&idx, body, sizeof(body)) > 0);
    TEST_ASSERT_TRUE(ap_vidx_from_json(body, &back));
    TEST_ASSERT_EQUAL(0, back.n);
    TEST_ASSERT_EQUAL(-1, back.current);

    /* too small a buffer */
    TEST_ASSERT_EQUAL(-1, ap_vidx_to_json(&idx, body, 16));
}

void test_vidx_json_full_index_fits(void)
{
    /* eight cars with every field at its cap must fit the file bound */
    ap_veh_index_t idx;
    char body[AP_VEH_INDEX_JSON_MAX];
    char vin[AP_VIN_LEN];

    ap_vidx_init(&idx);

    for (int i = 0; i < AP_VEH_MAX; i++)
    {
        snprintf(vin, sizeof(vin), "1WCAN0FW0P000000%d", i + 1);

        ap_veh_entry_t e = entry(vin, ECUS_READY, 3, 1759300000, 1759300000);

        memset(e.name, 'N', sizeof(e.name) - 1);
        memset(e.profile, 'P', sizeof(e.profile) - 1);
        memset(e.specific_init, 'I', sizeof(e.specific_init) - 1);
        snprintf(e.chip_protocol, sizeof(e.chip_protocol), "6");
        e.n_ecus = AP_VEH_ECUS_MAX;

        for (int k = 0; k < AP_VEH_ECUS_MAX; k++)
        {
            e.ecus[k].id = 0x18DAF110 + (uint32_t)k;
            e.ecus[k].bitmap = 0xFFFFFFFFu;
        }

        ap_veh_fingerprint(e.ecus, e.n_ecus, e.fingerprint);
        e.std_supported = 65535;
        e.scan_ts = 1759300000;
        TEST_ASSERT_EQUAL(i, ap_vidx_add(&idx, &e, NULL));
    }

    idx.current = AP_VEH_MAX - 1;

    int n = ap_vidx_to_json(&idx, body, sizeof(body));

    TEST_ASSERT_TRUE_MESSAGE(n > 0, "full index exceeds AP_VEH_INDEX_JSON_MAX");
    printf("  full vehicles.json = %d B of %d\n", n, AP_VEH_INDEX_JSON_MAX);

    ap_veh_index_t back;

    TEST_ASSERT_TRUE(ap_vidx_from_json(body, &back));
    TEST_ASSERT_EQUAL(AP_VEH_MAX, back.n);
    TEST_ASSERT_EQUAL(AP_VEH_MAX - 1, back.current);
    TEST_ASSERT_EQUAL(AP_VEH_ECUS_MAX, back.v[0].n_ecus);
}

void test_vidx_json_rejects_and_bounds(void)
{
    ap_veh_index_t back;

    TEST_ASSERT_FALSE(ap_vidx_from_json("garbage", &back));
    TEST_ASSERT_EQUAL(0, back.n);
    TEST_ASSERT_EQUAL(-1, back.current);
    TEST_ASSERT_FALSE(ap_vidx_from_json("[1,2]", &back));
    TEST_ASSERT_FALSE(ap_vidx_from_json("{\"vehicles\":[]}", &back));
    TEST_ASSERT_FALSE(ap_vidx_from_json("{\"version\":2,\"vehicles\":[]}",
                                        &back));
    TEST_ASSERT_FALSE(ap_vidx_from_json("", &back));
    TEST_ASSERT_FALSE(ap_vidx_from_json(NULL, &back));
    TEST_ASSERT_FALSE(ap_vidx_from_json("{}", NULL));

    /* no vehicles key = an empty index; a current naming no entry = none */
    TEST_ASSERT_TRUE(ap_vidx_from_json("{\"version\":1,\"current\":\"X\"}",
                                       &back));
    TEST_ASSERT_EQUAL(0, back.n);
    TEST_ASSERT_EQUAL(-1, back.current);

    /* entries without a usable key are dropped, duplicates too, a bad
       VIN / protocol is sanitized, a key is derived when missing */
    TEST_ASSERT_TRUE(ap_vidx_from_json(
        "{\"version\":1,\"current\":\"" VIN_A "\",\"vehicles\":["
        "{\"key\":\"" VIN_A "\",\"vin\":\"" VIN_A "\",\"protocol\":\"x\"},"
        "{\"key\":\"" VIN_A "\",\"vin\":\"" VIN_A "\"},"
        "{\"name\":\"no key at all\"},"
        "{\"vin\":\"not a vin\",\"ecus\":\"7E8:BE7FB813\",\"protocol\":\"a\"},"
        "{\"key\":\"../etc\",\"vin\":\"" VIN_B "\"},"
        "{\"key\":\"fp:DEADBEEF\",\"fingerprint\":\"DEADBEEF\"},"
        "{\"key\":\"" VIN_C "\",\"std_supported\":70000,\"first_seen\":-5}"
        "]}",
        &back));
    TEST_ASSERT_EQUAL(4, back.n);
    TEST_ASSERT_EQUAL(0, back.current);
    TEST_ASSERT_EQUAL_STRING(VIN_A, back.v[0].key);
    TEST_ASSERT_EQUAL_STRING("", back.v[0].protocol);      /* 'x' dropped */
    TEST_ASSERT_EQUAL_STRING_LEN("fp:", back.v[1].key, 3); /* derived     */
    TEST_ASSERT_EQUAL_STRING("", back.v[1].vin);
    TEST_ASSERT_EQUAL(1, back.v[1].n_ecus);
    TEST_ASSERT_EQUAL_STRING("A", back.v[1].protocol);     /* upcased     */
    TEST_ASSERT_EQUAL_STRING(VIN_B, back.v[2].key);        /* re-derived  */
    TEST_ASSERT_EQUAL_STRING(VIN_C, back.v[3].key);
    TEST_ASSERT_EQUAL(0, back.v[3].std_supported);         /* out of range */
    TEST_ASSERT_EQUAL_INT64(0, back.v[3].first_seen);

    /* more than AP_VEH_MAX entries: the tail is dropped */
    char body[2048];
    size_t w = (size_t)snprintf(body, sizeof(body),
                                "{\"version\":1,\"vehicles\":[");

    for (int i = 0; i < AP_VEH_MAX + 3; i++)
    {
        w += (size_t)snprintf(body + w, sizeof(body) - w,
                              "%s{\"key\":\"1WCAN0FW0P00000%02d\"}",
                              (i > 0) ? "," : "", i + 1);
    }

    snprintf(body + w, sizeof(body) - w, "]}");
    TEST_ASSERT_TRUE(ap_vidx_from_json(body, &back));
    TEST_ASSERT_EQUAL(AP_VEH_MAX, back.n);
    TEST_ASSERT_EQUAL_STRING("1WCAN0FW0P0000008", back.v[AP_VEH_MAX - 1].key);
}

/* ---- runner ------------------------------------------------------------------------ */

void run_vehicle_index_tests(void)
{
    RUN_TEST(test_vidx_key_and_name);
    RUN_TEST(test_vidx_find_by_key_vin_fp);
    RUN_TEST(test_vidx_subset_rule);
    RUN_TEST(test_vidx_match_policy);
    RUN_TEST(test_vidx_lru_and_eviction);
    RUN_TEST(test_vidx_touch_once_a_day);
    RUN_TEST(test_vidx_ecus_string);
    RUN_TEST(test_vidx_json_roundtrip);
    RUN_TEST(test_vidx_json_full_index_fits);
    RUN_TEST(test_vidx_json_rejects_and_bounds);
}
