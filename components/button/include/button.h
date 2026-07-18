#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_SW2 = 0,
    BUTTON_SW3,
    BUTTON_SW4,
    BUTTON_SW5,
    BUTTON_COUNT,
} button_id_t;

typedef struct {
    button_id_t id;
    bool pressed;
} button_event_t;

esp_err_t button_init(void);
bool button_get_event(button_event_t *event);
bool button_is_pressed(button_id_t id);
const char *button_name(button_id_t id);

#ifdef __cplusplus
}
#endif

#endif
