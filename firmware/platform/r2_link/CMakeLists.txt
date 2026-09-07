# Always registered; the SOURCE is what is conditional (see r2_link.c).
#
# Guarding the registration itself with if(CONFIG_BT_NIMBLE_ENABLED) does not
# work: CONFIG_* is not available during the early expansion pass, so the guard
# silently took the else branch and dropped REQUIRES, breaking every app that
# DOES use BLE with "fatal error: r2_packet.h". Compiling the body to nothing
# via #if in the .c file is the pattern that holds in both passes.
idf_component_register(SRCS "r2_link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt r2_packet)
