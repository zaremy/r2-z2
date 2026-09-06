idf_component_register(SRCS "main.c" "panel_ui.c" "panel_shot.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer bt nvs_flash
                                r2_link r2_ops r2_gate r2_packet r2_telemetry)
