#include "button.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_DEBOUNCE_MS 30

static const char *TAG = "button";

static const gpio_num_t s_button_pins[BUTTON_COUNT] = {
    [BUTTON_SW2] = GPIO_NUM_10,
    [BUTTON_SW3] = GPIO_NUM_9,
    [BUTTON_SW4] = GPIO_NUM_3,
    [BUTTON_SW5] = GPIO_NUM_11,
};

static const char *s_button_names[BUTTON_COUNT] = {
    [BUTTON_SW2] = "SW2",
    [BUTTON_SW3] = "SW3",
    [BUTTON_SW4] = "SW4",
    [BUTTON_SW5] = "SW5",
};

static bool s_state[BUTTON_COUNT];
static bool s_last_raw[BUTTON_COUNT];
static TickType_t s_last_change[BUTTON_COUNT];

static bool button_read_raw(button_id_t id)
{
    return gpio_get_level(s_button_pins[id]) == BUTTON_ACTIVE_LEVEL;
}

esp_err_t button_init(void)
{
    uint64_t pin_mask = 0;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        pin_mask |= 1ULL << s_button_pins[i];
    }

    gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "config button gpio failed");

    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        s_state[i] = button_read_raw((button_id_t)i);
        s_last_raw[i] = s_state[i];
        s_last_change[i] = now;
    }

    return ESP_OK;
}

bool button_get_event(button_event_t *event)
{
    if (!event) {
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    for (int i = 0; i < BUTTON_COUNT; i++) {
        bool raw = button_read_raw((button_id_t)i);
        if (raw != s_last_raw[i]) {
            s_last_raw[i] = raw;
            s_last_change[i] = now;
        }

        if ((now - s_last_change[i]) >= debounce_ticks && raw != s_state[i]) {
            s_state[i] = raw;
            event->id = (button_id_t)i;
            event->pressed = raw;
            return true;
        }
    }

    return false;
}

bool button_is_pressed(button_id_t id)
{
    if (id < 0 || id >= BUTTON_COUNT) {
        return false;
    }

    return s_state[id];
}

const char *button_name(button_id_t id)
{
    if (id < 0 || id >= BUTTON_COUNT) {
        return "UNKNOWN";
    }

    return s_button_names[id];
}
