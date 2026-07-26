# meatpi-components

Reusable ESP-IDF components for MeatPi Electronics devices (WiCAN Pro,
ESPNetLink, and future products). This is the component layer the
firmware projects build on: CAN/TWAI core, OBD stack, bridge/translator
framework, settings/logging/CLI infrastructure, connectivity (WiFi, BLE,
USB, VPN), and more.

**Targets:** ESP-IDF **v6.0.2**, ESP32-S3 (PSRAM). See
`components/*/README.md` for per-component documentation.

## Using

Add the `components/` directory to your project's `EXTRA_COMPONENT_DIRS`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "path/to/meatpi-components/components")
```

## Licensing

- Most components are open source under **AGPL-3.0-or-later** (see
  `LICENSE`). Commercial licensing is available — contact
  ali@meatpi.com.
- Some features of official MeatPi firmware builds come from optional
  add-on component packs that are not part of this repository. The
  attachment points are public (`ext_manager` boot hooks, provider
  registries, the component-pack overlay in the consuming project) —
  builds from this repo alone are fully functional without any pack.
- Third-party components keep their original licenses:

  | Component | License / origin |
  |---|---|
  | `berry` | MIT — berry-lang/berry |
  | `cherryusb` | Apache-2.0 — CherryUSB (with local fixes) |
  | `esp_isotp` | Apache-2.0 + isotp-c (MIT) |
  | `esp_wireguard` | BSD — Tomoyuki Sakurai |
  | `microlink` | MIT — Malone Technologies LLC (Tailscale client, with local mods) |
  | `sqlite3` | Public domain + Siara Logics shim |
  | `wireguard_lwip` | BSD — Daniel Hope + Cryptography Research |

## Contributing

Contribution terms (CLA) are being finalized — until they are published,
please open an issue before submitting code.
