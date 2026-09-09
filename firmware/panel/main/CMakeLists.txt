idf_component_register(SRCS "main.c" "panel_ui.c" "panel_shot.c" "panel_touch.c"
                            "fonts/michroma_12.c" "fonts/michroma_13.c"
                            "fonts/michroma_16.c" "fonts/michroma_30.c"
                            "fonts/techmono_20.c" "fonts/techmono_24.c"
                            "fonts/techmono_34.c"
                       INCLUDE_DIRS "." "fonts"
                       REQUIRES esp_timer bt nvs_flash
                                r2_link r2_ops r2_gate r2_packet r2_telemetry panel_state
                                # esp_lcd_touch and esp_lvgl_port are needed for
                                # lvgl_port_remove_touch(). Without esp_lcd_touch
                                # on the include path, esp_lvgl_port_touch.h
                                # compiles its whole API out behind
                                # __has_include("esp_lcd_touch.h") -- so the
                                # function silently does not exist and the error
                                # is "implicit declaration", which points at the
                                # call rather than at the missing dependency.
                                esp_lcd_touch espressif__esp_lvgl_port)

# P2: time the panel's own reconnect (#101, gates AC8). OFF by default --
# deliberately dropping the BLE link over and over is a measurement, not
# resting behaviour, and must not be reachable by accident.
#   idf.py -DPANEL_P2_RECONNECT=ON build
option(PANEL_P2_RECONNECT "Time the panel's own BLE reconnect (P2)" OFF)
if(PANEL_P2_RECONNECT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_P2_RECONNECT=1)
    message(STATUS "panel: P2 reconnect timing ENABLED")
endif()

# P4: watch his advertisement without ever connecting (#101). OFF by default --
# a panel that refuses to link to the droid is a measurement rig, not a panel.
#   idf.py -DPANEL_P4_IDLE=ON build
option(PANEL_P4_IDLE "Scan-only: measure his idle timeout (P4)" OFF)
if(PANEL_P4_IDLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_P4_IDLE=1)
    message(STATUS "panel: P4 idle-timeout scan ENABLED (will NOT connect)")
endif()
