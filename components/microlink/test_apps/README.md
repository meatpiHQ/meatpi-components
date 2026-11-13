# MicroLink lifecycle target test

From the firmware checkout: `.\test.ps1 target microlink`.
This flashes a test app. Restore the main firmware with `.\build.ps1 -Flash COMx`.
The app inherits the main firmware's configuration and partition table.

The production init/stop/start/destroy functions run against the real FreeRTOS
kernel, with one deliberately blocked task in the instance's DERP task slot.
No network is contacted: the client is created with a dummy auth key that is
never sent (init refuses a config without one), and start is refused before
any socket is opened. Existing NVS is not erased. A unit that has run
Tailscale before only has its device keys read (`Keys loaded from NVS`); a
unit that never did gets them generated and stored, as the firmware's first
Tailscale start would. A stop must return
ESP_ERR_TIMEOUT at its 30-second join deadline without losing that task's
handle. A start is refused while it is still owned. A second stop through
destroy must also time out and leave the instance allocated. After releasing
the task, destroy must succeed.

Expected in order: the two `MICROLINK TEST:` markers, Unity
`1 Tests 0 Failures 0 Ignored`, then `TEST DONE`. About 61 seconds, zero E
lines and no crash. Four W lines are expected: `a task is still running
30000 ms after the stop request` twice, `start refused: tasks are still
owned by this instance` and `destroy deferred: a task still owns the
instance`. A failing Unity assertion never prints `TEST DONE`.

Needs a WiCAN Pro and its console/flash UART, ESP-IDF selected by test.ps1;
no CAN wiring, simulator, public server or special host package.
The actual network-loss reproduction is `.\test.ps1 tspublic stop-dial`.
