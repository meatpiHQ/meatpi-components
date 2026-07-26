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
 * @file usb_net_device.c
 * @brief CherryUSB DEVICE stack (NCM/RNDIS) bridged to an esp_netif with a
 *        DHCP server (see include/usb_net_device.h for the model).
 *
 * Context rules: every CherryUSB device callback (endpoint completions,
 * class notify, the bus event handler) runs in the USB INTERRUPT on the ESP
 * port — they only flip state and notify the worker task with FromISR
 * primitives. The worker owns esp_netif calls and endpoint re-arming; lwIP
 * calls und_transmit() from its own task.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usbd_core.h"
#include "usbd_cdc_ncm.h"
#include "usbd_rndis.h"
#include "rndis_protocol.h"

#include "usb_net_device.h"

static const char *TAG = "usb_net_device";

#define UND_EP_OUT 0x02
#define UND_EP_IN  0x81
#define UND_EP_INT 0x83
#define UND_MPS    64   /* full speed bulk */
#define UND_MAX_SEG 1514

/* one NTB (NCM) / one RNDIS message each way; must be USB-DMA => INTERNAL */
#define UND_BUF_CAP 2048

#define UND_BIT_RX         0x01
#define UND_BIT_LINK_UP    0x02
#define UND_BIT_LINK_DOWN  0x04
#define UND_BIT_CONFIGURED 0x08

/* NOT 192.168.80.x — that's the WiFi AP's subnet (both netifs can be up
 * at once, and a duplicate IP/subnet breaks lwIP routing; bench-hit
 * 2026-07-08: the PC's DHCP OFFER went out the AP netif). */
#define UND_DEFAULT_IP_A 192
#define UND_DEFAULT_IP_B 168
#define UND_DEFAULT_IP_C 82
#define UND_DEFAULT_IP_D 1

/* ---- descriptors ----------------------------------------------------------- */

#define UND_STR_MAC_IDX 4 /* 1..3 = mfc/product/serial (USB_DEVICE_DESCRIPTOR_INIT) */

static const uint8_t s_device_desc[] =
{
    /* composite (IAD) device; VID/PID from Kconfig (16D0:1262 — NOT 1261,
     * that's the MeatPi USB-CAN product; see the Kconfig help) */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01,
                               CONFIG_WICAN_USB_DEV_VID,
                               CONFIG_WICAN_USB_DEV_PID, 0x0100, 0x01),
};

static const uint8_t s_config_desc_ncm[] =
{
    /* extra parens: WBVAL() doesn't parenthesize its argument */
    USB_CONFIG_DESCRIPTOR_INIT((9 + CDC_NCM_ALT_DESCRIPTOR_LEN), 0x02, 0x01,
                               USB_CONFIG_BUS_POWERED, 250),
    CDC_NCM_ALT_DESCRIPTOR_INIT(0x00, UND_EP_INT, UND_EP_OUT, UND_EP_IN,
                                UND_MPS, UND_MAX_SEG, UND_STR_MAC_IDX),
};

static const uint8_t s_config_desc_rndis[] =
{
    USB_CONFIG_DESCRIPTOR_INIT((9 + CDC_RNDIS_DESCRIPTOR_LEN), 0x02, 0x01,
                               USB_CONFIG_BUS_POWERED, 250),
    CDC_RNDIS_DESCRIPTOR_INIT(0x00, UND_EP_INT, UND_EP_OUT, UND_EP_IN,
                              UND_MPS, 0x00),
};

/* ---- state ------------------------------------------------------------------ */

static usb_net_device_class_t s_class;
static usb_net_device_status_t s_st;
static char s_serial[13];
static char s_mac_str[13];   /* host-side MAC as 12 hex chars (NCM iMACAddress) */
static uint8_t s_host_mac[6];

static esp_netif_t *s_netif;
static TaskHandle_t s_worker;
static StaticTask_t s_worker_tcb;                       /* internal: FreeRTOS */
static StackType_t s_worker_stack[3072] EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_tx_done;

static uint8_t *s_rx_buf; /* INTERNAL|DMA, UND_BUF_CAP */
static uint8_t *s_tx_buf;
static volatile uint32_t s_rx_len;
static volatile bool s_link_up;
static bool s_started;

/* RNDIS class global: rndis_bulk_out points it at the frame payload */
extern volatile uint8_t *g_rndis_rx_data_buffer;

typedef struct
{
    esp_netif_driver_base_t base;
} und_driver_t;

static und_driver_t s_driver;

/* ---- ISR-side: CherryUSB callbacks ----------------------------------------- */

static void notify_worker_from_isr(uint32_t bits)
{
    BaseType_t woken = pdFALSE;

    if (s_worker != NULL)
    {
        xTaskNotifyFromISR(s_worker, bits, eSetBits, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void und_usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event)
    {
        case USBD_EVENT_CONFIGURED:
            s_st.configured = true;
            notify_worker_from_isr(UND_BIT_CONFIGURED);
            break;
        case USBD_EVENT_RESET:
            s_st.configured = false;
            notify_worker_from_isr(UND_BIT_LINK_DOWN);
            break;
        default:
            break;
    }
}

/* NCM hooks */
void usbd_cdc_ncm_data_recv_done(uint32_t len)
{
    s_rx_len = len;
    notify_worker_from_isr(UND_BIT_RX);
}

void usbd_cdc_ncm_data_send_done(uint32_t len)
{
    BaseType_t woken = pdFALSE;

    (void)len;
    xSemaphoreGiveFromISR(s_tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

void usbd_cdc_ncm_link_event(bool up)
{
    notify_worker_from_isr(up ? UND_BIT_LINK_UP : UND_BIT_LINK_DOWN);
}

/* RNDIS hooks (no alt-setting link concept: link = configured + filter,
 * approximated as configured — see the worker) */
void usbd_rndis_data_recv_done(uint32_t len)
{
    s_rx_len = len;
    notify_worker_from_isr(UND_BIT_RX);
}

void usbd_rndis_data_send_done(uint32_t len)
{
    BaseType_t woken = pdFALSE;

    (void)len;
    xSemaphoreGiveFromISR(s_tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- netif RX (worker context) ---------------------------------------------- */

static void und_deliver_frame(const uint8_t *frame, uint16_t len, void *arg)
{
    /* copy out of the DMA buffer (plain malloc lands in PSRAM per the
     * project's ALWAYSINTERNAL=32 policy); esp_netif frees it via
     * driver_free_rx_buffer once lwIP is done */
    void *copy = malloc(len);

    (void)arg;

    if (copy == NULL)
    {
        s_st.rx_drops++;
        return;
    }

    memcpy(copy, frame, len);

    if (esp_netif_receive(s_netif, copy, len, copy) != ESP_OK)
    {
        s_st.rx_drops++; /* esp_netif frees the buffer even on error */
        return;
    }

    s_st.rx_frames++;
}

static void und_arm_read(void)
{
    int ret;

    if (s_class == USB_NET_DEVICE_CLASS_NCM)
    {
        ret = usbd_cdc_ncm_start_read(s_rx_buf, UND_BUF_CAP);
    }
    else
    {
        ret = usbd_rndis_start_read(s_rx_buf, UND_BUF_CAP);
    }

    if (ret != 0)
    {
        ESP_LOGD(TAG, "arm read failed (%d) — link down?", ret);
    }
}

static void und_handle_rx(void)
{
    uint32_t len = s_rx_len;

    if (len == 0)
    {
        return;
    }

    if (s_class == USB_NET_DEVICE_CLASS_NCM)
    {
        if (usbd_cdc_ncm_parse_ntb(s_rx_buf, len, und_deliver_frame, NULL) < 0)
        {
            s_st.rx_drops++;
        }
    }
    else
    {
        /* rndis_bulk_out already validated the header and pointed
         * g_rndis_rx_data_buffer at the payload */
        und_deliver_frame((const uint8_t *)g_rndis_rx_data_buffer,
                          (uint16_t)len, NULL);
    }

    s_rx_len = 0;
    und_arm_read();
}

/* ---- netif TX (lwIP task context) -------------------------------------------- */

static esp_err_t und_transmit(void *h, void *buffer, size_t len)
{
    int ret;

    (void)h;

    if (!s_link_up || len > UND_MAX_SEG)
    {
        s_st.tx_drops++;
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        s_st.tx_drops++;
        return ESP_FAIL;
    }

    /* previous transfer must have completed */
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        s_st.tx_drops++;
        xSemaphoreGive(s_tx_lock);
        return ESP_FAIL;
    }

    if (s_class == USB_NET_DEVICE_CLASS_NCM)
    {
        ret = usbd_cdc_ncm_eth_start_write(s_tx_buf, UND_BUF_CAP,
                                           buffer, len);
    }
    else
    {
        rndis_data_packet_t *hdr = (rndis_data_packet_t *)s_tx_buf;

        memset(hdr, 0, sizeof(*hdr));
        hdr->MessageType = REMOTE_NDIS_PACKET_MSG;
        hdr->MessageLength = sizeof(*hdr) + len;
        hdr->DataOffset = sizeof(*hdr) - sizeof(rndis_generic_msg_t);
        hdr->DataLength = len;
        memcpy(s_tx_buf + sizeof(*hdr), buffer, len);

        ret = usbd_rndis_start_write(s_tx_buf, sizeof(*hdr) + len);
    }

    if (ret < 0)
    {
        s_st.tx_drops++;
        xSemaphoreGive(s_tx_done); /* nothing in flight */
        xSemaphoreGive(s_tx_lock);
        return ESP_FAIL;
    }

    s_st.tx_frames++;
    xSemaphoreGive(s_tx_lock);
    return ESP_OK;
}

static void und_l2_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t und_post_attach(esp_netif_t *netif, void *args)
{
    und_driver_t *drv = (und_driver_t *)args;
    const esp_netif_driver_ifconfig_t ifcfg =
    {
        .handle = drv,
        .transmit = und_transmit,
        .driver_free_rx_buffer = und_l2_free,
    };

    drv->base.netif = netif;
    return esp_netif_set_driver_config(netif, &ifcfg);
}

/* ---- worker ------------------------------------------------------------------ */

static void und_link_set(bool up)
{
    if (up == s_link_up)
    {
        return;
    }

    s_link_up = up;
    s_st.link_up = up;

    if (up)
    {
        esp_netif_action_connected(s_netif, NULL, 0, NULL);
        und_arm_read();
        ESP_LOGI(TAG, "link up (%s) — host opened the data path",
                 s_st.device_class);
    }
    else
    {
        esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
        /* unblock any sender waiting on a completion that won't come */
        xSemaphoreGive(s_tx_done);
        ESP_LOGI(TAG, "link down");
    }
}

static void und_worker(void *arg)
{
    uint32_t bits;

    (void)arg;

    while (true)
    {
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (bits & UND_BIT_LINK_DOWN)
        {
            und_link_set(false);
        }

        if (bits & UND_BIT_CONFIGURED)
        {
            ESP_LOGI(TAG, "host configured the device");

            if (s_class == USB_NET_DEVICE_CLASS_RNDIS)
            {
                /* RNDIS has no alt-setting handshake; give the host a
                 * moment to finish REMOTE_NDIS_INITIALIZE, then declare
                 * the medium connected and open the data path */
                vTaskDelay(pdMS_TO_TICKS(300));
                (void)usbd_rndis_set_connect(true);
                und_link_set(true);
            }
        }

        if (bits & UND_BIT_LINK_UP)
        {
            und_link_set(true);
        }

        if (bits & UND_BIT_RX)
        {
            und_handle_rx();
        }
    }
}

/* ---- descriptor callbacks ----------------------------------------------------- */

static const uint8_t *und_device_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return s_device_desc;
}

static const uint8_t *und_config_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return (s_class == USB_NET_DEVICE_CLASS_NCM) ? s_config_desc_ncm
                                                 : s_config_desc_rndis;
}

static const uint8_t *und_device_quality_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return NULL; /* full-speed only */
}

static const char *und_string_descriptor_cb(uint8_t speed, uint8_t index)
{
    (void)speed;

    switch (index)
    {
        case 0:
            return "\x09\x04"; /* langid 0x0409 */
        case 1:
            return "MeatPi Electronics";
        case 2:
            return "WiCAN Pro";
        case 3:
            return s_serial;
        case UND_STR_MAC_IDX:
            return s_mac_str;
        default:
            return NULL;
    }
}

static const struct usb_descriptor s_usbd_descriptor =
{
    .device_descriptor_callback = und_device_descriptor_cb,
    .config_descriptor_callback = und_config_descriptor_cb,
    .device_quality_descriptor_callback = und_device_quality_descriptor_cb,
    .string_descriptor_callback = und_string_descriptor_cb,
};

/* ---- lifecycle ----------------------------------------------------------------- */

static void und_make_macs(uint8_t dev_mac[6])
{
    esp_read_mac(dev_mac, ESP_MAC_ETH);
    dev_mac[0] |= 0x02; /* locally administered */
    dev_mac[0] &= (uint8_t)~0x01;

    /* the host NIC's MAC (what iMACAddress / RNDIS OID report) must differ
     * from the netif's or ARP on a two-node link goes nowhere */
    memcpy(s_host_mac, dev_mac, 6);
    s_host_mac[5] ^= 0x55;

    snprintf(s_mac_str, sizeof(s_mac_str), "%02X%02X%02X%02X%02X%02X",
             s_host_mac[0], s_host_mac[1], s_host_mac[2],
             s_host_mac[3], s_host_mac[4], s_host_mac[5]);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             dev_mac[0], dev_mac[1], dev_mac[2],
             dev_mac[3], dev_mac[4], dev_mac[5]);
}

esp_err_t usb_net_device_start(const usb_net_device_config_t *cfg)
{
    static struct usbd_interface s_intf0, s_intf1;
    static esp_netif_ip_info_t s_ip_info;
    uint8_t dev_mac[6];

    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_class = cfg->device_class;
    snprintf(s_st.device_class, sizeof(s_st.device_class), "%s",
             (s_class == USB_NET_DEVICE_CLASS_NCM) ? "ncm" : "rndis");

    s_rx_buf = heap_caps_malloc(UND_BUF_CAP,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_tx_buf = heap_caps_malloc(UND_BUF_CAP,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (s_rx_buf == NULL || s_tx_buf == NULL)
    {
        heap_caps_free(s_rx_buf);
        heap_caps_free(s_tx_buf);
        s_rx_buf = NULL;
        s_tx_buf = NULL;
        ESP_LOGE(TAG, "no internal DMA RAM for the USB buffers");
        return ESP_ERR_NO_MEM;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    s_tx_done = xSemaphoreCreateBinary();

    if (s_tx_lock == NULL || s_tx_done == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_tx_done); /* nothing in flight */

    und_make_macs(dev_mac);

    /* -- netif + DHCP server -- */
    s_ip_info.ip.addr = (cfg->ip.addr != 0) ? cfg->ip.addr :
        ESP_IP4TOADDR(UND_DEFAULT_IP_A, UND_DEFAULT_IP_B,
                      UND_DEFAULT_IP_C, UND_DEFAULT_IP_D);
    s_ip_info.netmask.addr = (cfg->netmask.addr != 0) ? cfg->netmask.addr :
        ESP_IP4TOADDR(255, 255, 255, 0);
    /* Management link, NOT a router: leave gw 0.0.0.0 so the DHCP OFFER
     * carries no router option (dhcpserver.c skips option 3 for an any
     * gw). Advertising ourselves as default gateway made every host
     * install a top-priority default route through us and blackhole its
     * real traffic (bench PC, 2026-07-17). A genuine gateway device
     * (the espnetlink dongle) is the opposite case and keeps its gw. */
    s_ip_info.gw.addr = 0;

    esp_netif_inherent_config_t base_cfg =
        (esp_netif_inherent_config_t)ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t netif_cfg = (esp_netif_config_t)ESP_NETIF_DEFAULT_ETH();

    base_cfg.if_key = "USBND";
    base_cfg.if_desc = "usb_net_device";
    base_cfg.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP;
    base_cfg.ip_info = &s_ip_info;
    base_cfg.get_ip_event = 0; /* DHCP server, not client */
    base_cfg.lost_ip_event = 0;
    base_cfg.route_prio = 10;
    netif_cfg.base = &base_cfg;
    netif_cfg.driver = NULL;

    s_netif = esp_netif_new(&netif_cfg);

    if (s_netif == NULL)
    {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }

    s_driver.base.post_attach = und_post_attach;
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, &s_driver), TAG,
                        "netif attach failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_netif, dev_mac), TAG,
                        "netif set mac failed");

    snprintf(s_st.ip, sizeof(s_st.ip), IPSTR, IP2STR(&s_ip_info.ip));

    esp_netif_action_start(s_netif, NULL, 0, NULL); /* + DHCP server */

    /* belt & braces with gw==0 above: clear OFFER_ROUTER so the server
     * can never advertise a gateway (option 32 = the offer-flag toggle,
     * settable only while the DHCP server runs) */
    uint8_t router_off = 0;

    (void)esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                 &router_off, sizeof(router_off));

    s_worker = xTaskCreateStatic(und_worker, "usb_net_dev",
                                 sizeof(s_worker_stack) /
                                 sizeof(s_worker_stack[0]),
                                 NULL, 6, s_worker_stack, &s_worker_tcb);

    if (s_worker == NULL)
    {
        return ESP_FAIL;
    }

    /* -- device stack: descriptors, interfaces, go -- */
    usbd_desc_register(0, &s_usbd_descriptor);

    if (s_class == USB_NET_DEVICE_CLASS_NCM)
    {
        usbd_add_interface(0, usbd_cdc_ncm_init_intf(&s_intf0, UND_EP_INT,
                                                     UND_EP_OUT, UND_EP_IN));
        usbd_add_interface(0, usbd_cdc_ncm_init_data_intf(&s_intf1));
    }
    else
    {
        usbd_add_interface(0, usbd_rndis_init_intf(&s_intf0, UND_EP_OUT,
                                                   UND_EP_IN, UND_EP_INT,
                                                   s_host_mac));
        memset(&s_intf1, 0, sizeof(s_intf1)); /* data intf: class handles all */
        usbd_add_interface(0, &s_intf1);
    }

    if (usbd_initialize(0, ESP_USBD_BASE, und_usbd_event_handler) != 0)
    {
        ESP_LOGE(TAG, "usbd_initialize failed");
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "up: class=%s ip=%s vid=0x%04x pid=0x%04x — waiting for "
             "the host", s_st.device_class, s_st.ip,
             CONFIG_WICAN_USB_DEV_VID, CONFIG_WICAN_USB_DEV_PID);
    return ESP_OK;
}

esp_err_t usb_net_device_get_status(usb_net_device_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_st;
    return ESP_OK;
}
