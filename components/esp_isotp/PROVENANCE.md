# esp_isotp — vendored component

Espressif upstream **esp_isotp v0.1.1** (isotp-c based), copied into
an earlier meatpi project and adopted here 2026-07-07 with it. Uses the
NEW `esp_driver_twai` node API. `src/isotp_config.h` is force-included
via CMake (upstream arrangement) to pin the isotp-c config.

## Local changes (this repo)

None beyond the inherited isotp_config.h arrangement.
