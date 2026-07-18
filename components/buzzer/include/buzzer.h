#ifndef BUZZER_H
#define BUZZER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t buzzer_init(void);
esp_err_t buzzer_on(unsigned int freq_hz);
esp_err_t buzzer_off(void);
esp_err_t buzzer_beep(unsigned int freq_hz, unsigned int duration_ms);

#ifdef __cplusplus
}
#endif

#endif
