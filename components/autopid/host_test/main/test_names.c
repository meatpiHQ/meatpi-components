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

/* Parameter names made unique (autopid_names.c) and the SAE table that
   must never need it again (2026-10-06, OxySensor1_Volt under 0x14 and
   0x24: a car answering both got a file its loader refused at boot). */
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "cJSON.h"

#include "autopid_private.h"
#include "obd2_standard_pids.h" /* the vendored table, walked below */

static const char *name_at(const cJSON *pids, int row, int prm)
{
    const cJSON *p = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pids, row),
                                         "parameters"), prm);
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(p, "name");

    return cJSON_IsString(n) ? n->valuestring : "";
}

void test_names_dedupe_suffixes_the_later_repeats(void)
{
    cJSON *root = cJSON_Parse(
        "{\"pids\":["
        "{\"name\":\"a\",\"cmd\":\"0114\",\"parameters\":["
        "{\"name\":\"OxySensor1_Volt\"},{\"name\":\"OxySensor1_STFT\"}]},"
        "{\"name\":\"b\",\"cmd\":\"0124\",\"parameters\":["
        "{\"name\":\"OxySensor1_FAER\"},{\"name\":\"OxySensor1_Volt\"}]},"
        "{\"name\":\"c\",\"cmd\":\"0134\",\"parameters\":["
        "{\"name\":\"OxySensor1_FAER\"},{\"name\":\"OxySensor1_Volt\"}]}]}");
    cJSON *pids = cJSON_GetObjectItemCaseSensitive(root, "pids");

    TEST_ASSERT_EQUAL_INT(3, ap_names_dedupe(pids));
    TEST_ASSERT_EQUAL_STRING("OxySensor1_Volt", name_at(pids, 0, 0));
    TEST_ASSERT_EQUAL_STRING("OxySensor1_FAER", name_at(pids, 1, 0));
    TEST_ASSERT_EQUAL_STRING("OxySensor1_Volt_2", name_at(pids, 1, 1));
    TEST_ASSERT_EQUAL_STRING("OxySensor1_FAER_2", name_at(pids, 2, 0));
    TEST_ASSERT_EQUAL_STRING("OxySensor1_Volt_3", name_at(pids, 2, 1));

    /* a second pass changes nothing: the names are unique now */
    TEST_ASSERT_EQUAL_INT(0, ap_names_dedupe(pids));
    cJSON_Delete(root);
}

void test_names_dedupe_skips_a_taken_suffix_and_same_row_repeats(void)
{
    /* temp_2 exists already: the repeat becomes temp_3; and a name used
       twice INSIDE one row is a repeat too */
    cJSON *root = cJSON_Parse(
        "{\"pids\":["
        "{\"name\":\"a\",\"cmd\":\"0105\",\"parameters\":["
        "{\"name\":\"temp\"},{\"name\":\"temp_2\"},{\"name\":\"temp\"}]}]}");
    cJSON *pids = cJSON_GetObjectItemCaseSensitive(root, "pids");

    TEST_ASSERT_EQUAL_INT(1, ap_names_dedupe(pids));
    TEST_ASSERT_EQUAL_STRING("temp_3", name_at(pids, 0, 2));
    cJSON_Delete(root);

    /* nothing to do: nothing touched, nothing counted */
    root = cJSON_Parse("{\"pids\":[{\"name\":\"a\",\"parameters\":["
                       "{\"name\":\"x\"},{\"name\":\"y\"}]},"
                       "{\"name\":\"b\",\"parameters\":[]},{\"name\":\"c\"}]}");
    TEST_ASSERT_EQUAL_INT(0, ap_names_dedupe(
        cJSON_GetObjectItemCaseSensitive(root, "pids")));
    cJSON_Delete(root);
    TEST_ASSERT_EQUAL_INT(0, ap_names_dedupe(NULL));
}

void test_config_repair_names_makes_the_file_parse(void)
{
    /* ~1 MB struct: STATIC, never on the task stack (§2) */
    static ap_config_t cfg;
    char err[96] = "";
    const char *dupes =
        "{\"pids\":["
        "{\"name\":\"a\",\"cmd\":\"0114\",\"parameters\":["
        "{\"name\":\"OxySensor1_Volt\",\"expression\":\"B2\"}]},"
        "{\"name\":\"b\",\"cmd\":\"0124\",\"parameters\":["
        "{\"name\":\"OxySensor1_Volt\",\"expression\":\"B2\"}]}]}";
    int renamed = -1;

    /* the parser refuses the repeat... */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_config_parse(dupes, &cfg, err, sizeof(err)));

    /* ...the repaired text it takes */
    char *fixed = ap_config_repair_names(dupes, &renamed);

    TEST_ASSERT_NOT_NULL(fixed);
    TEST_ASSERT_EQUAL_INT(1, renamed);
    TEST_ASSERT_NOT_NULL(strstr(fixed, "\"OxySensor1_Volt_2\""));
    TEST_ASSERT_EQUAL(ESP_OK, ap_config_parse(fixed, &cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL(2, cfg.n_pids);
    TEST_ASSERT_EQUAL(2, cfg.n_params);
    free(fixed);

    /* a clean file is left alone: no copy, no count */
    renamed = -1;
    TEST_ASSERT_NULL(ap_config_repair_names("{\"pids\":[{\"name\":\"a\","
                                            "\"cmd\":\"0105\",\"parameters\":["
                                            "{\"name\":\"t\",\"expression\":"
                                            "\"B2\"}]}]}", &renamed));
    TEST_ASSERT_EQUAL_INT(0, renamed);

    /* text that is not JSON: not ours to judge */
    renamed = -1;
    TEST_ASSERT_NULL(ap_config_repair_names("nonsense", &renamed));
    TEST_ASSERT_EQUAL_INT(0, renamed);
}

void test_std_table_has_no_parameter_name_twice(void)
{
    /* the vendored table itself: a name under two PIDs is what put a car's
       file beyond its own loader (21 pairs until 2026-10-06) */
    int params = 0, rows = 0;

    for (int a = 0; a < PID_ARRAY_SIZE; a++)
    {
        const std_pid_t *pa = get_pid((uint8_t)a);

        if (pa == NULL || pa->params == NULL)
        {
            continue;
        }

        rows++;

        for (int b = a + 1; b < PID_ARRAY_SIZE; b++)
        {
            const std_pid_t *pb = get_pid((uint8_t)b);

            if (pb != NULL && pa->base_name != NULL && pb->base_name != NULL)
            {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(
                    0, strcmp(pa->base_name, pb->base_name), pa->base_name);
            }
        }

        for (int i = 0; i < pa->num_params; i++)
        {
            const char *na = pa->params[i].name;

            params++;
            TEST_ASSERT_NOT_NULL(na);
            TEST_ASSERT_TRUE_MESSAGE(strlen(na) < AP_NAME_LEN, na);

            /* against the parameters after it: the rest of this row, then
               every later row */
            for (int j = i + 1; j < pa->num_params; j++)
            {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(
                    0, strcmp(na, pa->params[j].name), na);
            }

            for (int b = a + 1; b < PID_ARRAY_SIZE; b++)
            {
                const std_pid_t *pb = get_pid((uint8_t)b);

                for (int j = 0; pb != NULL && j < pb->num_params; j++)
                {
                    TEST_ASSERT_NOT_EQUAL_MESSAGE(
                        0, strcmp(na, pb->params[j].name), na);
                }
            }
        }
    }

    TEST_ASSERT_TRUE(rows > 100);
    TEST_ASSERT_TRUE(params > 150);
}

void run_names_tests(void)
{
    RUN_TEST(test_names_dedupe_suffixes_the_later_repeats);
    RUN_TEST(test_names_dedupe_skips_a_taken_suffix_and_same_row_repeats);
    RUN_TEST(test_config_repair_names_makes_the_file_parse);
    RUN_TEST(test_std_table_has_no_parameter_name_twice);
}
