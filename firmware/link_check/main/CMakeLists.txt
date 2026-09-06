idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash r2_link r2_ops r2_gate r2_packet)

# The LED phase is OFF by default and must be asked for by name:
#   idf.py -DLINK_CHECK_LEDS=ON build
#
# CLAUDE.md fixes the bring-up order -- read-only, then LEDs, then audio, dome,
# stance, locomotion -- and requires each actuator tier to be individually
# opt-in, never bundled. A build flag is what "individually opt-in" looks like
# in firmware: you cannot get the LED phase by accident, and the default image
# cannot drive anything but reads.
option(LINK_CHECK_LEDS "Run one LED step (raises the gate ceiling to 'leds')" OFF)
# Which step. One per flash, held indefinitely, so the observer sets the pace.
set(LINK_CHECK_LED_STEP "1" CACHE STRING "LED step to fire (1-7)")
if(LINK_CHECK_LEDS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        LINK_CHECK_LEDS=1
        LINK_CHECK_LED_STEP=${LINK_CHECK_LED_STEP})
    message(STATUS "link_check: LED step ${LINK_CHECK_LED_STEP} ENABLED")
endif()
