#include "buzzer.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUZZER_PIN GPIO_NUM_16
#define BUZZER_TIMER LEDC_TIMER_1
#define BUZZER_CHANNEL LEDC_CHANNEL_1
#define BUZZER_DUTY_RES LEDC_TIMER_10_BIT
#define BUZZER_DUTY_ON 512

static const char *TAG = "buzzer";
static bool s_initialized;

esp_err_t buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num = BUZZER_TIMER,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "config buzzer timer failed");

    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "config buzzer channel failed");

    s_initialized = true;
    return ESP_OK;
}

esp_err_t buzzer_on(unsigned int freq_hz)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "buzzer is not initialized");
    ESP_RETURN_ON_ERROR(ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, freq_hz),
                        TAG, "set buzzer frequency failed");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, BUZZER_DUTY_ON),
                        TAG, "set buzzer duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL),
                        TAG, "update buzzer duty failed");

    return ESP_OK;
}

esp_err_t buzzer_off(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "buzzer is not initialized");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0),
                        TAG, "set buzzer off duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL),
                        TAG, "update buzzer off duty failed");

    return ESP_OK;
}

esp_err_t buzzer_beep(unsigned int freq_hz, unsigned int duration_ms)
{
    ESP_RETURN_ON_ERROR(buzzer_on(freq_hz), TAG, "turn buzzer on failed");
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    return buzzer_off();
}
