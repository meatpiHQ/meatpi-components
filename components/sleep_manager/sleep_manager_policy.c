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
 * @file sleep_manager_policy.c
 * @brief The pure voltage ladder (legacy sleep_mode.c semantics,
 *        host-tested; no esp deps). Wrap-safe deadline math: deadlines
 *        are stored as absolute ms and compared by signed subtraction.
 */
#include "sleep_manager_private.h"

static bool reached(uint32_t deadline, uint32_t now)
{
    return (int32_t)(now - deadline) >= 0;
}

void sm_policy_init(sm_policy_t *p)
{
    p->state = SLEEP_MANAGER_NORMAL;
    p->t_low = 0;
    p->t_stable = 0;
    p->t_periodic = 0;
}

sm_action_t sm_policy_eval(sm_policy_t *p, const sm_cfg_t *cfg,
                           float volts, uint32_t now_ms)
{
    switch (p->state)
    {
        case SLEEP_MANAGER_NORMAL:
            if (volts < cfg->sleep_v)
            {
                p->state = SLEEP_MANAGER_LOW_VOLTAGE;
                p->t_low = now_ms + cfg->delay_ms;
            }

            break;

        case SLEEP_MANAGER_LOW_VOLTAGE:
            if (volts >= cfg->wake_v)
            {
                p->state = SLEEP_MANAGER_NORMAL;
            }
            else if (reached(p->t_low, now_ms))
            {
                p->state = SLEEP_MANAGER_SLEEPING;
                p->t_periodic = now_ms + cfg->interval_ms;
                return SM_ACT_ENTER_SLEEP;
            }

            break;

        case SLEEP_MANAGER_SLEEPING:
            if (volts >= cfg->wake_v)
            {
                p->state = SLEEP_MANAGER_WAKE_PENDING;
                p->t_stable = now_ms + SM_WAKE_STABLE_MS;
            }
            else if (cfg->periodic && volts > SM_CRITICAL_V &&
                     reached(p->t_periodic, now_ms))
            {
                return SM_ACT_PERIODIC_WAKE;
            }

            break;

        case SLEEP_MANAGER_WAKE_PENDING:
            if (volts < cfg->wake_v)
            {
                p->state = SLEEP_MANAGER_SLEEPING; /* dipped again */
            }
            else if (reached(p->t_stable, now_ms))
            {
                return SM_ACT_WAKE_REBOOT;
            }

            break;
    }

    return SM_ACT_NONE;
}
