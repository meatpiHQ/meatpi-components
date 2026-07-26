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

/**
 * @file battery_monitor_adc.c
 * @brief The ADC layer: oneshot unit + calibration (curve fitting with
 *        line-fitting fallback, per the IDF v6 oneshot example), 8-sample
 *        averaging, divider/offset scaling (Kconfig per hardware
 *        revision). Ported from the field-proven legacy sleep_mode.c.
 */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "battery_monitor_private.h"

static const char *TAG = "battery_monitor";

#define BM_ADC_UNIT    ADC_UNIT_1
#define BM_ADC_CHANNEL ((adc_channel_t)CONFIG_WICAN_BATT_ADC_CHANNEL)
#define BM_ADC_ATTEN   ADC_ATTEN_DB_6
#define BM_SAMPLES     8

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static bool s_calibrated;

esp_err_t bm_adc_init(void)
{
    if (s_unit != NULL)
    {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BM_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);

    if (err != ESP_OK)
    {
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg =
    {
        .atten = BM_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    err = adc_oneshot_config_channel(s_unit, BM_ADC_CHANNEL, &chan_cfg);

    if (err != ESP_OK)
    {
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_cfg =
    {
        .unit_id = BM_ADC_UNIT,
        .chan = BM_ADC_CHANNEL,
        .atten = BM_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    s_calibrated =
        adc_cali_create_scheme_curve_fitting(&curve_cfg, &s_cali) == ESP_OK;
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_calibrated)
    {
        adc_cali_line_fitting_config_t line_cfg =
        {
            .unit_id = BM_ADC_UNIT,
            .atten = BM_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };

        s_calibrated =
            adc_cali_create_scheme_line_fitting(&line_cfg, &s_cali) ==
            ESP_OK;
    }
#endif

    if (!s_calibrated)
    {
        ESP_LOGW(TAG, "no eFuse calibration; using the ideal transfer");
    }

    return ESP_OK;
}

esp_err_t bm_adc_read(float *out)
{
    if (s_unit == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    int sum_mv = 0;
    int valid = 0;

    for (int i = 0; i < BM_SAMPLES; i++)
    {
        int raw;

        if (adc_oneshot_read(s_unit, BM_ADC_CHANNEL, &raw) != ESP_OK)
        {
            continue;
        }

        int mv;

        if (s_calibrated)
        {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK)
            {
                continue;
            }
        }
        else
        {
            mv = (raw * 3300) / 4095;
        }

        sum_mv += mv;
        valid++;
    }

    if (valid == 0)
    {
        return ESP_FAIL;
    }

    /* pin mV -> battery volts: divider ratio + bench offset (Kconfig) */
    float pin_mv = (float)sum_mv / (float)valid;
    float batt_mv = pin_mv * ((float)CONFIG_WICAN_BATT_DIVIDER_X100 /
                              100.0f) +
                    (float)CONFIG_WICAN_BATT_OFFSET_MV;

    *out = batt_mv / 1000.0f;
    return ESP_OK;
}
