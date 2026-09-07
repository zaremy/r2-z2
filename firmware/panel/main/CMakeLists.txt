idf_component_register(SRCS "main.c" "panel_ui.c" "panel_shot.c" "panel_touch.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer bt nvs_flash
                                r2_link r2_ops r2_gate r2_packet r2_telemetry)

# P2: time the panel's own reconnect (#101, gates AC8). OFF by default --
# deliberately dropping the BLE link over and over is a measurement, not
# resting behaviour, and must not be reachable by accident.
#   idf.py -DPANEL_P2_RECONNECT=ON build
option(PANEL_P2_RECONNECT "Time the panel's own BLE reconnect (P2)" OFF)
if(PANEL_P2_RECONNECT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_P2_RECONNECT=1)
    message(STATUS "panel: P2 reconnect timing ENABLED")
endif()
