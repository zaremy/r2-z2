idf_component_register(SRCS "main.c" "panel_ui.c" "panel_shot.c" "panel_touch.c"
                            "fonts/michroma_12.c" "fonts/michroma_13.c"
                            "fonts/michroma_16.c" "fonts/michroma_30.c"
                            "fonts/michroma_32.c"
                            "fonts/techmono_20.c" "fonts/techmono_24.c"
                            "fonts/techmono_22.c" "fonts/techmono_28.c"
                            "fonts/techmono_34.c" "fonts/techmono_72.c"
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

# Screenshot the wake frame the moment it lands (#101 AC3). OFF by default --
# it replaces the periodic 60 s capture, so a panel built with it stops
# recording the resting face. For verifying the frame on glass, nothing else.
#   idf.py -DPANEL_SHOT_WAKE=ON build
option(PANEL_SHOT_WAKE "Capture the wake frame instead of the periodic shot" OFF)
if(PANEL_SHOT_WAKE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_SHOT_WAKE=1)
    message(STATUS "panel: screenshot ON WAKE FRAME (periodic shots off)")
endif()

# P4: watch his advertisement without ever connecting (#101). OFF by default --
# a panel that refuses to link to the droid is a measurement rig, not a panel.
#   idf.py -DPANEL_P4_IDLE=ON build
option(PANEL_P4_IDLE "Scan-only: measure his idle timeout (P4)" OFF)
if(PANEL_P4_IDLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_P4_IDLE=1)
    message(STATUS "panel: P4 idle-timeout scan ENABLED (will NOT connect)")
endif()
