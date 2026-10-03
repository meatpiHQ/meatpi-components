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
 * @file j1939_tx.c
 * @brief Active mode: the node's NAME and address claim on the bus, the
 *        answers it owes (its claim on request, a negative acknowledgment to
 *        a request it cannot serve, the transport protocol's clear-to-send /
 *        end-of-message / abort), and the requests its readers send
 *        (j1939_request) with their outcomes (TASK_j1939_wwh.md phase 6).
 *
 * Every frame is decided under the component's lock (the receive task sorts
 * the frames that call for an answer; the claim's clock ticks there too) and
 * SENT without it: decisions go into an outbox, j1939_tx_flush() hands the
 * outbox to can_manager after the lock is released, because a transmit may
 * wait on the driver. j1939_request() is the exception: called by a reader's
 * task, it sends directly.
 *
 * The node only talks once it holds an address (J1939-81): until the claim
 * is through, requests are refused (ESP_ERR_INVALID_STATE) and the transport
 * protocol has no address to answer for. A bus can_manager holds listen-only
 * never becomes transmit-ready: active mode then stays a listener and logs
 * it once (a warning, not an error: the setting may be deliberate).
 */
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "can_manager.h"

#include "j1939_private.h"

static const char *TAG = "j1939";

#define TX_OUTBOX        16  /* frames decided under the lock, sent after it */
#define TX_REQ_SLOTS     16  /* requests whose outcome a reader may ask for  */
#define TX_PRIO_CLAIM    6
#define TX_PRIO_ACK      6
#define TX_PRIO_REQUEST  6
#define TX_PRIO_TP       7
#define TX_BLOCKED_WARN_US 10000000 /* the bus did not open in 10 s: say so */

typedef struct
{
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} out_frame_t;

typedef struct
{
    uint32_t pgn;
    uint8_t  da;
    uint8_t  outcome;   /* j1939_req_outcome_t                              */
    uint8_t  control;   /* the acknowledgment's, when one came              */
    int64_t  sent_us;
} req_slot_t;

static j1939_claim_t s_claim;
static bool s_active;            /* `mode` active for this session           */
static bool s_started;           /* the claim went out                        */
static bool s_blocked_warned;
static int64_t s_first_tick_us;
static j1939_claim_state_t s_last_state = J1939_CLAIM_IDLE;

static out_frame_t s_out[TX_OUTBOX];
static uint8_t     s_out_n;
static req_slot_t  s_req[TX_REQ_SLOTS];

static struct
{
    uint32_t tx_frames;
    uint32_t tx_failed;
    uint32_t outbox_lost;
    uint32_t requests;
    uint32_t acks;
    uint32_t nacks;
    uint32_t nacks_sent;
} s_st;

/* ---- the outbox (lock held) ------------------------------------------------------ */

static void queue_frame(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da,
                        const uint8_t *data, uint8_t dlc)
{
    if (s_out_n >= TX_OUTBOX)
    {
        s_st.outbox_lost++;
        return;
    }

    out_frame_t *f = &s_out[s_out_n++];

    f->id = j1939_id_make(prio, pgn, sa, da);
    f->dlc = dlc;
    memcpy(f->data, data, dlc);
}

static void queue_claim(const j1939_claim_out_t *o)
{
    if (o->send)
    {
        queue_frame(TX_PRIO_CLAIM, J1939_PGN_CLAIM, o->sa, J1939_ADDR_GLOBAL,
                    o->data, J1939_NAME_LEN);
    }
}

/** The transport protocol answers for the address we hold, none otherwise;
 *  and the state changes worth a line. */
static void sync_address(void)
{
    j1939_tp_set_address(j1939_priv_tp(), j1939_claim_ready(&s_claim)
                                              ? s_claim.sa
                                              : J1939_ADDR_NULL);

    if (s_claim.state == s_last_state)
    {
        return;
    }

    s_last_state = s_claim.state;

    switch (s_claim.state)
    {
    case J1939_CLAIM_CLAIMED:
        ESP_LOGI(TAG, "address %u is ours: requests may go out", s_claim.sa);
        break;

    case J1939_CLAIM_CANNOT:
        ESP_LOGW(TAG, "cannot claim an address: every candidate is held by a "
                      "better NAME; active mode stays silent");
        break;

    default:
        break;
    }
}

/* ---- lifecycle ------------------------------------------------------------------- */

void j1939_tx_start(void)
{
    uint8_t mac[6] = { 0 };
    uint8_t name[J1939_NAME_LEN];

    /* the identity number: the device-specific half of the MAC (the OUI is
       the same on every one of ours), 21 bits of it */
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint32_t identity = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) |
                        mac[5];

    /* manufacturer code 0 (none assigned), function: off-board diagnostic
       service tool, arbitrary address capable */
    j1939_name_build(identity, 0, J1939_FUNCTION_TOOL, true, name);
    j1939_claim_init(&s_claim, name, j1939_settings_address());
    s_active = j1939_settings_active();
    s_started = false;
    s_blocked_warned = false;
    s_first_tick_us = 0;
    s_last_state = J1939_CLAIM_IDLE;
    s_out_n = 0;
    memset(s_req, 0, sizeof(s_req));
    j1939_tp_set_address(j1939_priv_tp(), J1939_ADDR_NULL);
}

void j1939_tx_stop(void)
{
    j1939_claim_init(&s_claim, s_claim.name, s_claim.preferred);
    s_started = false;
    s_out_n = 0;
    s_last_state = J1939_CLAIM_IDLE;
    j1939_tp_set_address(j1939_priv_tp(), J1939_ADDR_NULL);
}

void j1939_tx_reset(void)
{
    memset(&s_st, 0, sizeof(s_st));
    memset(&s_claim.stats, 0, sizeof(s_claim.stats));
}

/* ---- the clock (lock held) --------------------------------------------------------- */

void j1939_tx_tick(int64_t now_us)
{
    j1939_claim_out_t o;

    if (!s_active)
    {
        return;
    }

    if (s_first_tick_us == 0)
    {
        s_first_tick_us = now_us;
    }

    if (!s_started)
    {
        if (!can_manager_tx_ready())
        {
            /* the bus is listen-only (can_manager.silent), or its bitrate is
               not proven yet (frames prove it; a silent bus takes seconds) */
            if (!s_blocked_warned &&
                now_us - s_first_tick_us > TX_BLOCKED_WARN_US)
            {
                s_blocked_warned = true;
                ESP_LOGW(TAG, "active mode, but the bus does not let this node "
                              "transmit (can_manager.silent on, or no frame "
                              "proved the bitrate yet): listening only");
            }

            return;
        }

        j1939_claim_start(&s_claim, now_us, &o);
        queue_claim(&o);
        s_started = true;
        ESP_LOGI(TAG, "claiming address %u (NAME %02X%02X%02X%02X%02X%02X%02X"
                      "%02X)", s_claim.sa, s_claim.name[7], s_claim.name[6],
                 s_claim.name[5], s_claim.name[4], s_claim.name[3],
                 s_claim.name[2], s_claim.name[1], s_claim.name[0]);
        return;
    }

    j1939_claim_tick(&s_claim, now_us);
    sync_address();
}

/* ---- frames that call for an answer (lock held) -------------------------------------- */

void j1939_tx_on_claim(uint8_t sa, const uint8_t *name, int64_t now_us)
{
    j1939_claim_out_t o;
    uint8_t before = s_claim.sa;

    if (!s_active || !s_started)
    {
        return;
    }

    /* our own claim, should the controller ever echo it: not a contest */
    if (sa == s_claim.sa && memcmp(name, s_claim.name, J1939_NAME_LEN) == 0)
    {
        return;
    }

    j1939_claim_rx(&s_claim, sa, name, now_us, &o);
    queue_claim(&o);

    if (o.send && s_claim.sa != before)
    {
        if (s_claim.state == J1939_CLAIM_CANNOT)
        {
            ESP_LOGI(TAG, "address %u lost to %02X%02X%02X%02X%02X%02X%02X%02X,"
                          " no candidate left", before, name[7], name[6],
                     name[5], name[4], name[3], name[2], name[1], name[0]);
        }
        else
        {
            ESP_LOGI(TAG, "address %u lost to a better NAME, claiming %u",
                     before, s_claim.sa);
        }
    }
    else if (o.send)
    {
        ESP_LOGI(TAG, "address %u contested by a lesser NAME: kept", before);
    }

    sync_address();
}

void j1939_tx_on_request(uint8_t sa, uint8_t da, uint32_t pgn, int64_t now_us)
{
    j1939_claim_out_t o;

    if (!s_active || !s_started)
    {
        return;
    }

    if (pgn == J1939_PGN_CLAIM)
    {
        /* Request for Address Claimed, to us or to everyone (one addressed
           to another node is its business) */
        if (da == J1939_ADDR_GLOBAL || da == s_claim.sa)
        {
            j1939_claim_request(&s_claim, now_us, &o);
            queue_claim(&o);
        }

        return;
    }

    if (!j1939_claim_ready(&s_claim) || da != s_claim.sa)
    {
        return; /* not ours to answer (a global request is never NACKed) */
    }

    /* a request addressed to this node for a group it does not provide:
       J1939-21 wants a negative acknowledgment, to everyone, naming the
       requester */
    uint8_t d[8];

    j1939_ackm_build(J1939_ACK_NEGATIVE, sa, pgn, d);
    queue_frame(TX_PRIO_ACK, J1939_PGN_ACKM, s_claim.sa, J1939_ADDR_GLOBAL, d,
                8);
    s_st.nacks_sent++;
    ESP_LOGD(TAG, "request for %lX from %02X: not ours to give, NACK",
             (unsigned long)pgn, sa);
}

static req_slot_t *req_find(uint32_t pgn, uint8_t da)
{
    for (size_t i = 0; i < TX_REQ_SLOTS; i++)
    {
        if (s_req[i].outcome != J1939_REQ_NONE && s_req[i].pgn == pgn &&
            s_req[i].da == da)
        {
            return &s_req[i];
        }
    }

    return NULL;
}

void j1939_tx_on_ackm(uint8_t sa, uint8_t da, const uint8_t *data, uint8_t dlc,
                      int64_t now_us)
{
    j1939_ackm_t a;

    (void)now_us;

    if (!s_active || !j1939_claim_ready(&s_claim) ||
        !j1939_ackm_parse(data, dlc, &a))
    {
        return;
    }

    /* for us: addressed to us, or (J1939-21 2006+: sent to everyone) naming
       us as the requester */
    if (da != s_claim.sa && a.address != s_claim.sa)
    {
        return;
    }

    /* the request it answers: the one we sent to that controller, or to
       everyone */
    req_slot_t *r = req_find(a.pgn, sa);

    if (r == NULL)
    {
        r = req_find(a.pgn, J1939_ADDR_GLOBAL);
    }

    if (r == NULL)
    {
        return; /* an acknowledgment of a request we did not make */
    }

    r->control = a.control;
    r->outcome = (a.control == J1939_ACK_POSITIVE) ? J1939_REQ_ACKED
                                                   : J1939_REQ_NACKED;

    if (a.control == J1939_ACK_POSITIVE)
    {
        s_st.acks++;
    }
    else
    {
        s_st.nacks++;
    }

    ESP_LOGD(TAG, "%s from %02X for %lX", j1939_ack_name(a.control), sa,
             (unsigned long)a.pgn);
}

/* ---- sending (lock NOT held) ---------------------------------------------------------- */

static void send_frame(const out_frame_t *f)
{
    if (can_manager_tx_ready() &&
        can_manager_send(f->id, true, false, f->data, f->dlc) == ESP_OK)
    {
        s_st.tx_frames++;
    }
    else
    {
        s_st.tx_failed++;
    }
}

void j1939_tx_flush(void)
{
    out_frame_t f;
    j1939_tp_reply_t r;

    if (!s_active)
    {
        return;
    }

    for (;;)
    {
        bool have = false;

        j1939_lock();

        if (s_out_n > 0)
        {
            f = s_out[0];
            s_out_n--;
            memmove(&s_out[0], &s_out[1], s_out_n * sizeof(s_out[0]));
            have = true;
        }
        else if (j1939_claim_ready(&s_claim) &&
                 j1939_tp_reply_take(j1939_priv_tp(), &r))
        {
            f.id = j1939_id_make(TX_PRIO_TP, J1939_PGN_TP_CM, s_claim.sa, r.da);
            f.dlc = 8;
            memcpy(f.data, r.data, 8);
            have = true;
        }

        j1939_unlock();

        if (!have)
        {
            return;
        }

        send_frame(&f);
    }
}

/* ---- the readers' requests ------------------------------------------------------------ */

bool j1939_active(void)
{
    return s_active && j1939_claim_ready(&s_claim) && can_manager_tx_ready();
}

uint8_t j1939_address(void)
{
    return j1939_claim_ready(&s_claim) ? s_claim.sa : J1939_ADDR_NULL;
}

esp_err_t j1939_request(uint32_t pgn, uint8_t da)
{
    uint8_t d[J1939_REQUEST_LEN];
    out_frame_t f;
    req_slot_t *r;

    if (!j1939_active())
    {
        return ESP_ERR_INVALID_STATE;
    }

    j1939_lock();

    /* the slot of this (group, destination), else a free one, else the
       oldest */
    r = req_find(pgn, da);

    if (r == NULL)
    {
        r = &s_req[0];

        for (size_t i = 0; i < TX_REQ_SLOTS; i++)
        {
            if (s_req[i].outcome == J1939_REQ_NONE)
            {
                r = &s_req[i];
                break;
            }

            if (s_req[i].sent_us < r->sent_us)
            {
                r = &s_req[i];
            }
        }
    }

    r->pgn = pgn;
    r->da = da;
    r->outcome = J1939_REQ_PENDING;
    r->control = 0xFF;
    r->sent_us = esp_timer_get_time();
    j1939_request_build(pgn, d);
    f.id = j1939_id_make(TX_PRIO_REQUEST, J1939_PGN_REQUEST, s_claim.sa, da);
    f.dlc = J1939_REQUEST_LEN;
    memcpy(f.data, d, J1939_REQUEST_LEN);
    j1939_unlock();

    if (!can_manager_tx_ready() ||
        can_manager_send(f.id, true, false, f.data, f.dlc) != ESP_OK)
    {
        j1939_lock();
        s_st.tx_failed++;
        r->outcome = J1939_REQ_NONE; /* nothing to wait for */
        j1939_unlock();
        return ESP_FAIL;
    }

    j1939_lock();
    s_st.tx_frames++;
    s_st.requests++;
    j1939_unlock();
    return ESP_OK;
}

j1939_req_outcome_t j1939_request_outcome(uint32_t pgn, uint8_t da,
                                          uint32_t *age_ms, uint8_t *control)
{
    j1939_req_outcome_t out = J1939_REQ_NONE;

    j1939_lock();

    const req_slot_t *r = req_find(pgn, da);

    if (r != NULL)
    {
        out = (j1939_req_outcome_t)r->outcome;

        if (age_ms != NULL)
        {
            *age_ms = (uint32_t)((esp_timer_get_time() - r->sent_us) / 1000);
        }

        if (control != NULL)
        {
            *control = r->control;
        }
    }

    j1939_unlock();
    return out;
}

const char *j1939_req_outcome_name(j1939_req_outcome_t outcome)
{
    switch (outcome)
    {
    case J1939_REQ_PENDING: return "pending";
    case J1939_REQ_ACKED:   return "acked";
    case J1939_REQ_NACKED:  return "nacked";
    default:                return "none";
    }
}

/* ---- status (lock held) ------------------------------------------------------------- */

void j1939_tx_status(j1939_status_t *out)
{
    const j1939_tp_t *tp = j1939_priv_tp();

    out->active = s_active;
    out->tx_ready = can_manager_tx_ready();
    out->claim = s_claim.state;
    out->address = j1939_claim_ready(&s_claim) ? s_claim.sa : J1939_ADDR_NULL;
    memcpy(out->name, s_claim.name, J1939_NAME_LEN);
    out->claim_stats = s_claim.stats;
    out->tx_frames = s_st.tx_frames;
    out->tx_failed = s_st.tx_failed + s_st.outbox_lost;
    out->requests = s_st.requests;
    out->acks = s_st.acks;
    out->nacks = s_st.nacks;
    out->nacks_sent = s_st.nacks_sent;
    out->tp_to_me = tp->stats.to_me;
    out->tp_cts = tp->stats.cts;
    out->tp_eoma = tp->stats.eoma;
    out->tp_aborts_out = tp->stats.aborts_out;
    out->tp_reply_lost = tp->stats.reply_lost;
}
