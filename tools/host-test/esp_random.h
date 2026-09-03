#pragma once
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t esp_random(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }
