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
 * @file rtc_manager_rx8130.c
 * @brief RX8130CE chip layer on the shared i2c_bus. Control-register
 *        bring-up values are the field-proven legacy ones (they configure
 *        the backup-capacitor charging path — do not "clean up").
 */
#include "esp_log.h"

#include "i2c_bus.h"

#include "rtc_manager_private.h"

static const char *TAG = "rtc_manager";

#define RX8130_ADDR       0x32
#define RX8130_SPEED_HZ   400000
#define RX8130_TIMEOUT_MS 100

#define RX8130_REG_SEC      0x10 /* ..0x16: min hour week day month year */
#define RX8130_REG_EVT_CTRL 0x1C
#define RX8130_REG_EVT1     0x1D
#define RX8130_REG_EVT2     0x1E
#define RX8130_REG_EVT3     0x1F
#define RX8130_REG_CTRL1    0x30
#define RX8130_REG_CTRL2    0x32

static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    return i2c_master_transmit(s_dev, buf, sizeof(buf),
                               RX8130_TIMEOUT_MS);
}

static esp_err_t regs_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len,
                                       RX8130_TIMEOUT_MS);
}

esp_err_t rtc_rx8130_init(void)
{
    esp_err_t err = i2c_bus_add_device(RX8130_ADDR, RX8130_SPEED_HZ,
                                       &s_dev);

    if (err != ESP_OK)
    {
        return err;
    }

    /* legacy bring-up (backup-cap charging + event filter config) */
    err = reg_write(RX8130_REG_CTRL1, 0x00);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RX8130 not responding: %s", esp_err_to_name(err));
        return err;
    }

    reg_write(RX8130_REG_CTRL2, 0xC7);
    reg_write(RX8130_REG_EVT_CTRL, 0x04);
    reg_write(RX8130_REG_EVT1, 0x00);
    reg_write(RX8130_REG_EVT2, 0x40);
    reg_write(RX8130_REG_EVT3, 0x10);
    return ESP_OK;
}

esp_err_t rtc_rx8130_read(uint8_t regs[RTC_REGS_LEN])
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* coherent read: burst twice until the seconds register agrees (a
     * rollover between bursts would tear the date fields) */
    for (int attempt = 0; attempt < 3; attempt++)
    {
        uint8_t again[RTC_REGS_LEN];
        esp_err_t err = regs_read(RX8130_REG_SEC, regs, RTC_REGS_LEN);

        if (err != ESP_OK)
        {
            return err;
        }

        err = regs_read(RX8130_REG_SEC, again, RTC_REGS_LEN);

        if (err != ESP_OK)
        {
            return err;
        }

        if (regs[0] == again[0])
        {
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t rtc_rx8130_write(const uint8_t regs[RTC_REGS_LEN])
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[1 + RTC_REGS_LEN] = { RX8130_REG_SEC };

    for (int i = 0; i < RTC_REGS_LEN; i++)
    {
        buf[1 + i] = regs[i];
    }

    return i2c_master_transmit(s_dev, buf, sizeof(buf),
                               RX8130_TIMEOUT_MS);
}
