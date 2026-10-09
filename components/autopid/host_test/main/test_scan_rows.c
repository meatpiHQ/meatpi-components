/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* The detection's rows as a new car's tables (autopid_scan_rows.c): the
   shape the config loader accepts, and the rule of 2026-10-09 (Ali: "it
   should not start polling unless the user enables the standard PIDs and
   enables the PIDs they want"): every stored row carries "enabled": false
   and polls nothing until ticked. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "cJSON.h"

#include "autopid_private.h"   /* brings autopid_dialect.h */

static const char *SUPPORTED =
    "[{\"pid\":12,\"cmd\":\"010C\",\"name\":\"EngineRPM\","
    "\"parameters\":[{\"name\":\"EngineRPM\",\"expression\":\"[B2:B3]*0.25\","
    "\"unit\":\"rpm\"}]},"
    "{\"pid\":13,\"cmd\":\"010D\",\"name\":\"VehicleSpeed\","
    "\"parameters\":[{\"name\":\"VehicleSpeed\",\"expression\":\"B2\","
    "\"unit\":\"km/h\"}]},"
    "{\"cmd\":\"22F40C\",\"name\":\"EngineRPM_uds\",\"init\":\"ATSH18DA00F1\","
    "\"parameters\":[{\"name\":\"EngineRPM_uds\",\"expression\":\"[B3:B4]*0.25\","
    "\"unit\":\"rpm\"}]},"
    "{\"name\":\"no command\"}]";

static void test_scan_rows_config_shape_every_row_off(void)
{
    cJSON *sup = cJSON_Parse(SUPPORTED);

    TEST_ASSERT_NOT_NULL(sup);

    char *doc = ap_scan_rows_config(sup);

    TEST_ASSERT_NOT_NULL(doc);

    cJSON *root = cJSON_Parse(doc);

    TEST_ASSERT_NOT_NULL(root);

    const cJSON *pids = cJSON_GetObjectItemCaseSensitive(root, "pids");
    const cJSON *row = NULL;

    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "groups")));
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "filters")));
    TEST_ASSERT_TRUE(cJSON_IsArray(pids));
    /* the row without a command is skipped */
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(pids));

    cJSON_ArrayForEach(row, pids)
    {
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(row, "enabled");

        TEST_ASSERT_EQUAL_STRING("std", cJSON_GetObjectItemCaseSensitive(row, "type")->valuestring);
        TEST_ASSERT_EQUAL_STRING("default", cJSON_GetObjectItemCaseSensitive(row, "group")->valuestring);
        TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(row, "parameters")));
        /* stored, not read */
        TEST_ASSERT_TRUE(cJSON_IsBool(en));
        TEST_ASSERT_TRUE(cJSON_IsFalse(en));
    }

    row = cJSON_GetArrayItem(pids, 0);
    TEST_ASSERT_EQUAL_STRING("010C", cJSON_GetObjectItemCaseSensitive(row, "cmd")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(row, "init"));

    /* a uds row keeps its ECU */
    row = cJSON_GetArrayItem(pids, 2);
    TEST_ASSERT_EQUAL_STRING("ATSH18DA00F1", cJSON_GetObjectItemCaseSensitive(row, "init")->valuestring);

    cJSON_Delete(root);
    cJSON_Delete(sup);
    free(doc);
}

static void test_scan_rows_loader_keeps_them_off(void)
{
    cJSON *sup = cJSON_Parse(SUPPORTED);

    TEST_ASSERT_NOT_NULL(sup);

    char *doc = ap_scan_rows_config(sup);

    TEST_ASSERT_NOT_NULL(doc);

    /* the loader takes the document as it is and nothing polls until a
       row is ticked (the scheduler's entry gate reads pids[i].enabled) */
    ap_config_t *cfg = calloc(1, sizeof(*cfg));
    char err[96] = "";

    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(doc, cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(3, cfg->n_pids);

    for (int i = 0; i < cfg->n_pids; i++)
    {
        TEST_ASSERT_FALSE(cfg->pids[i].enabled);
        TEST_ASSERT_EQUAL_INT(AP_PID_STD, cfg->pids[i].type);
    }

    free(cfg);
    cJSON_Delete(sup);
    free(doc);
}

static void test_scan_rows_empty_and_null(void)
{
    cJSON *sup = cJSON_CreateArray();
    char *doc = ap_scan_rows_config(sup);

    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL_STRING("{\"groups\":[],\"pids\":[],\"filters\":[]}", doc);
    free(doc);
    cJSON_Delete(sup);

    doc = ap_scan_rows_config(NULL);
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL_STRING("{\"groups\":[],\"pids\":[],\"filters\":[]}", doc);
    free(doc);
}

void run_scan_rows_tests(void)
{
    RUN_TEST(test_scan_rows_config_shape_every_row_off);
    RUN_TEST(test_scan_rows_loader_keeps_them_off);
    RUN_TEST(test_scan_rows_empty_and_null);
}
