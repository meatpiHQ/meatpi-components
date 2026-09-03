# usb_host_manager

WiCAN USB host policy (feature layer). The WiCAN Pro USB connector is
shared between the CH342 (device role: PC console/flash) and the
ESP32-S3 OTG host, muxed by GPIO11. When `enabled` (role `host`), a
presence task watches the OTG ID pin (GPIO39, low = device attached,
debounced 3×200 ms): on attach it flips the mux to the host, ensures
VBUS on (GPIO10 `USB_OTG_PWR_EN`, shared rail with `sleep_manager` —
NO power-cycle since 2026-08-24: the mux flip itself gives the DWC2 its
connect edge, and a cycle rebooted an attached ESPNetLink and killed its
GPS fix on every WiCAN reboot) and
starts `usb_eth_host` (CherryUSB enumerates the adapter — ASIX / CDC-ECM /
CDC-NCM / RNDIS — into ONE esp_netif, DHCP or static). On detach
everything tears down and the mux returns to the CH342. Role `device`
makes the WiCAN a USB-Ethernet/serial device instead (`usb_net_device`,
`usb_cdc_device`).

Uplink semantics: no WiFi/USB arbitration — the wired uplink raises
`DEV_STATUS_BIT_ETH_CONNECTED`; `prefer_usb_route` (default off =
wifi-first) only sets the default netif when the wire has an IP. Policy
consumers (`espnetlink_link`) read the status each tick — it is derived,
not event-driven.

## API

| Function | One-liner |
|---|---|
| `usb_host_manager_init()` | Settings/log/event descriptors. No hardware. |
| `usb_host_manager_start()` | Pins + presence task when enabled (host or device role). |
| `usb_host_manager_stop()` | Tear the host stack down, mux back to the CH342 (also the sleep path). |
| `usb_host_manager_status(out)` | `enabled, device_present, host_active, eth_connected, driver, ip, attaches, vid, pid, vbus_on` — `vid`/`pid` (2026-08-24) = the enumerated device behind the active USB-Ethernet driver (0 when none; the ESPNetLink is `303A:4007`), cleared when the IP is lost. |
| `usb_host_manager_set_vbus(on)` | Drive the connector's VBUS rail while host mode is active (open-drain, board pull-up: power-on default ON; off→on reboots the attached device). `ESP_ERR_INVALID_STATE` when the host is down. The dongle re-pair lever; also `usb vbus <0\|1>`. |
| `usb_host_manager_register_http()` | `GET /api/usb` (`components/HTTP_API.md` §6e9). |
| `usb_host_manager_register_cli()` | `usb` (+ bench debug `mux`/`vbus`/`suspend`/`resume`); self-registered on the settings apply. |

## Dependencies

`usb_eth_host` (the CherryUSB host + netif glue; its `on_eth_ip_up` /
`on_eth_ip_lost` pair — balanced since 2026-08-24 even when the netif is
simply stopped/destroyed on an unplug or the ESPNetLink's data-line cut),
`usb_net_device`, `usb_cdc_device`, `settings_manager`, `log_manager`,
`dev_status_manager`, `event_manager`, `cmdline_manager`,
`http_server_manager`, `esp_driver_gpio`. Start after `wifi_manager`.

## Settings (`"usb_host_manager"`, version 1)

`enabled` (**true** since 2026-08-31 — zero-touch ESPNetLink; the mux only leaves the CH342 on an attached device), `role` (`host`|`device`, host), `device_class`
(`ncm`|`rndis`|`cdc`, ncm), `ip_mode` (`dhcp`|`static`), `static_ip`,
`static_netmask` (255.255.255.0), `static_gw`, `prefer_usb_route` (false),
`cli` (true). Kconfig: `WICAN_USB_ID_GPIO` (39), `WICAN_USB_MODE_GPIO` (11),
device VID/PIDs.

## Memory footprint (estimated)

- Presence task `usb_presence`: 3072 × 4 B stack in PSRAM, prio 4.
- Static: ~0.3 KB `.bss` (status + debouncer).
- The CherryUSB host stack and the eth shared buffer (4.6 KB internal DMA)
  belong to `usb_eth_host`/`cherryusb` and exist only while a device is
  attached (`cherryusb/PROVENANCE.md`).

## Testing

Host suite (`host_test/`): the pure presence debouncer. Everything else
is live USB: `tools/testbench/usb/espnetlink_bench.py` (`.\test.ps1
espnetlink`), the USB-Ethernet bench (`usbeth`), and `espnetlink_link`'s
bench sequence (attach → identify → cut → drop → VBUS recovery).
