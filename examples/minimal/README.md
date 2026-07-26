# minimal

Smallest useful meatpi-components consumer: settings + logging + CLI +
the CAN core + the bus-conversation gate + the add-on hook component
(`ext_manager`, no-op in stock builds). Doubles as the build-integration
check for the repo.

```sh
idf.py set-target esp32s3
idf.py build
```

ESP-IDF v6.0.2, ESP32-S3 with octal PSRAM (WiCAN Pro-class hardware —
for quad-PSRAM boards change `CONFIG_SPIRAM_MODE` in
`sdkconfig.defaults`).
