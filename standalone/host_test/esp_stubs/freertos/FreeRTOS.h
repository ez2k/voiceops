#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10)
#define portMAX_DELAY     0xffffffffu
#define pdPASS            1
#define pdTRUE            1
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZE(m)  ((void)(m))
#define taskENTER_CRITICAL(m)  ((void)(m))
#define taskEXIT_CRITICAL(m)   ((void)(m))
