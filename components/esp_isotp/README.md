# ESP-IDF ISO-TP Component

[![Component Registry](https://components.espressif.com/components/espressif/esp_isotp/badge.svg)](https://components.espressif.com/components/espressif/esp_isotp)

ISO 15765-2 (ISO-TP) for ESP-IDF. Sends/receives large payloads (≤4095 B) over TWAI with segmentation and reassembly.

## Key Features

- Segmentation + flow control for >7 B
- Non-blocking API, ISR-backed
- Multiple links in parallel
- UDS/OBD-II friendly
- Timeouts + sequence checks
- 11-bit and 29-bit IDs

> [!NOTE]
> TWAI-FD (Flexible Data-rate)  is not supported in this version.

## Installation

This repository vendors `esp_isotp` locally under `components/esp_isotp`.

## Configuration

You can configure protocol parameters like timing through the project configuration menu:

```bash
idf.py menuconfig
```

Navigate to `Component config` → `ISO-TP Protocol Configuration`.

## Quick Start Guide

Here's a simple example that initializes the TWAI driver and an ISO-TP link, then echoes back any received data.

### Example Code

```c
#include "esp_isotp.h"
#include "esp_twai_onchip.h"

void app_main(void) {
    // 1) Init TWAI
    twai_onchip_node_config_t twai_cfg = {
        .io_cfg = {.tx = GPIO_NUM_5, .rx = GPIO_NUM_4},
        .bit_timing = {.bitrate = 500000},
        .tx_queue_depth = 16,
    };
    twai_node_handle_t twai_node;
    ESP_ERROR_CHECK(twai_new_node_onchip(&twai_cfg, &twai_node));

    // 2) Create ISO-TP (11-bit IDs, auto-detected)
    esp_isotp_config_t config = {
        .tx_id = 0x7E0,                // request ID (11-bit, auto-detected)
        .rx_id = 0x7E8,                // response ID (11-bit, auto-detected)
        .tx_buffer_size = 4096,
        .rx_buffer_size = 4096,
        .tx_frame_pool_size = 16,
    };
    esp_isotp_handle_t isotp_handle;
    ESP_ERROR_CHECK(esp_isotp_new_transport(twai_node, &config, &isotp_handle));

    // 3) Loop
    uint8_t buffer[4096];
    uint32_t received_size;

    while (1) {
        // This is the engine of the component. Call it frequently!
        esp_isotp_poll(isotp_handle);

        // Check if a full message has been received
        if (esp_isotp_receive(isotp_handle, buffer, sizeof(buffer), &received_size) == ESP_OK) {
            printf("Received %lu bytes\n", received_size);
            // Echo the message back
            esp_isotp_send(isotp_handle, buffer, received_size);
        }

        vTaskDelay(pdMS_TO_TICKS(5)); // A small delay is good practice
    }
}
```

## Shared CAN ownership

By default, `esp_isotp_new_transport()` registers TWAI callbacks and manages
TWAI node enable/disable for the transport.

If your application already owns the TWAI node and needs to keep normal CAN
send/receive available to other tasks, set `externally_managed_twai = true`.
In that mode:

- `esp_isotp` does **not** register TWAI callbacks
- `esp_isotp` does **not** enable or disable the TWAI node
- your CAN owner should forward matching RX frames with `esp_isotp_feed_can_frame()`
- you may provide `can_tx_callback` to send ISO-TP CAN frames through an
  external CAN manager instead of a TWAI node

```c
esp_isotp_config_t cfg = {
    .tx_id = 0x7E0,
    .rx_id = 0x7E8,
    .tx_buffer_size = 4096,
    .rx_buffer_size = 4096,
    .tx_frame_pool_size = 16,
    .externally_managed_twai = true,
};

ESP_ERROR_CHECK(esp_isotp_new_transport(twai_node, &cfg, &isotp_handle));

// From your shared CAN RX task/callback:
ESP_ERROR_CHECK(esp_isotp_feed_can_frame(isotp_handle, &frame));
```

For CAN-manager integrations that do not expose a TWAI node handle, provide a
`can_tx_callback` and pass `NULL` for `twai_node`:

```c
static esp_err_t can_tx_cb(uint32_t id, const uint8_t *data, uint8_t size, void *arg)
{
    can_core_frame_t frame = {
        .id = id,
        .ext = id > 0x7FF,
        .rtr = false,
        .dlc = size,
    };
    memcpy(frame.data, data, size);
    return can_core_transmit(arg, &frame, 0) == ELM327_OK ? ESP_OK : ESP_FAIL;
}

esp_isotp_config_t cfg = {
    .tx_id = 0x7E0,
    .rx_id = 0x7E8,
    .tx_buffer_size = 4096,
    .rx_buffer_size = 4096,
    .tx_frame_pool_size = 16,
    .externally_managed_twai = true,
    .can_tx_callback = can_tx_cb,
    .can_tx_callback_arg = can_handle,
};

ESP_ERROR_CHECK(esp_isotp_new_transport(NULL, &cfg, &isotp_handle));
```

## Extended (29-bit) IDs

- ID format is auto-detected: IDs > 0x7FF use 29-bit extended format, others use 11-bit standard format.
- `tx_id` and `rx_id` must differ and match peer's expected format.

```c
esp_isotp_config_t cfg = {
    .tx_id = 0x18DAF110,    // 29-bit ID (auto-detected as extended)
    .rx_id = 0x18DA10F1,    // 29-bit ID (auto-detected as extended)
    .tx_buffer_size = 4096,
    .rx_buffer_size = 4096,
    .tx_frame_pool_size = 16,
};
```

## Errors

- Common: ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT
- Send: ESP_ERR_NOT_FINISHED when previous TX in progress
- Receive: ESP_ERR_NOT_FOUND when no complete message; ESP_ERR_INVALID_RESPONSE on bad sequence
- Full list: see `esp_isotp.h`

## Checklist

- IDs valid and different; 11-bit vs 29-bit matches `use_extended_id`
- Buffers > 0; size gates max single-message length (≤4095 B)
- Call `esp_isotp_poll()` every 1–10 ms
- TWAI node created and enabled before use
