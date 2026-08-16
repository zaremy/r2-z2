# firmware/ — ESP32-S3 backpack

Not started. Foundation decided (D-005): **ESP-IDF v5.5.x + the managed BSP
`waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3`**, NimBLE in central role, LVGL 9.

Prerequisites, in order:

1. Install ESP-IDF v5.5.x (not present on this host).
2. Confirm the board revision before flashing anything — see
   `../docs/research/board-revision.md`. **Do not flash `vthinkxie` firmware.**
3. Follow the first embedded slice in `../docs/research/embedded-path.md`.

Planned layout (do not scaffold ahead of need):

```
platform/  display · touch · power · audio · storage · imu · connectivity
r2/        ble · protocol · commands · capabilities
behavior/  semantic behaviors · idle · mood · scheduler
brain/     state · memory · cloud client · decision boundary
ui/        status · settings · diagnostics · hardware test
```

The first real file will be a port of
`research/external/claude-r2d2-buddy/main/r2d2_central.c` — retargeted C6→S3,
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` 2→1, with `nus_peripheral.c` dropped (D-006).
