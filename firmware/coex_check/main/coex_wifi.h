#pragma once
#include <stdbool.h>
#include <stdint.h>
void     coex_wifi_start(bool with_load);
uint32_t coex_wifi_disconnects(void);
