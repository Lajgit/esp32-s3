#ifndef NFC_H
#define NFC_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NFC_USER_MEMORY_SIZE 512
#define NFC_PAGE_SIZE        16
#define NFC_UID_SIZE         8

esp_err_t nfc_init(void);
esp_err_t nfc_read(uint16_t address, uint8_t *data, size_t len);
esp_err_t nfc_write(uint16_t address, const uint8_t *data, size_t len);
esp_err_t nfc_read_uid(uint8_t uid[NFC_UID_SIZE]);
esp_err_t nfc_write_ndef_text(const char *text);
esp_err_t nfc_write_ndef_uri(const char *https_uri_suffix);
esp_err_t nfc_self_test(void);

#ifdef __cplusplus
}
#endif

#endif
