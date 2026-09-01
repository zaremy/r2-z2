idf_component_register(SRCS "main.c" "r2_central.c" "coex_stats.c" "coex_wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_timer esp_wifi esp_event esp_netif esp_http_client)

# Arm is chosen at build time: idf.py -B build-arm2 -DCOEX_ARM=2 build
# Reflash between arms. Kept a compile-time switch rather than runtime config so
# each arm's binary is a fixed, re-flashable artifact with no hidden state.
if(DEFINED COEX_ARM)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE COEX_ARM=${COEX_ARM})
endif()
