#ifndef NFC_H
#define NFC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NFC_USER_MEMORY_SIZE 512
#define NFC_PAGE_SIZE        16
#define NFC_UID_SIZE         8
#define NFC_MAILBOX_SIZE     256

/* IT_STS_Dyn 中断状态位 */
#define NFC_EVENT_RF_USER       (1U << 0)
#define NFC_EVENT_RF_ACTIVITY   (1U << 1)
#define NFC_EVENT_RF_INTERRUPT  (1U << 2)
#define NFC_EVENT_FIELD_FALLING (1U << 3)
#define NFC_EVENT_FIELD_RISING  (1U << 4)
#define NFC_EVENT_RF_PUT_MSG    (1U << 5)
#define NFC_EVENT_RF_GET_MSG    (1U << 6)
#define NFC_EVENT_RF_WRITE      (1U << 7)

/* MB_CTRL_Dyn 邮箱状态位 */
#define NFC_MAILBOX_ENABLED          (1U << 0)
#define NFC_MAILBOX_HOST_PUT_MSG     (1U << 1)
#define NFC_MAILBOX_RF_PUT_MSG       (1U << 2)
#define NFC_MAILBOX_HOST_MISS_MSG    (1U << 4)
#define NFC_MAILBOX_RF_MISS_MSG      (1U << 5)
#define NFC_MAILBOX_HOST_CURRENT_MSG (1U << 6)
#define NFC_MAILBOX_RF_CURRENT_MSG   (1U << 7)

esp_err_t nfc_init(void);
esp_err_t nfc_read(uint16_t address, uint8_t *data, size_t len);
esp_err_t nfc_write(uint16_t address, const uint8_t *data, size_t len);
esp_err_t nfc_read_uid(uint8_t uid[NFC_UID_SIZE]);
esp_err_t nfc_write_ndef_text(const char *text);
esp_err_t nfc_write_ndef_uri(const char *https_uri_suffix);
esp_err_t nfc_ensure_ndef_uri(const char *https_uri_suffix);
esp_err_t nfc_self_test(void);

bool nfc_gpo_is_active(void);
esp_err_t nfc_get_interrupt_status(uint8_t *status);

esp_err_t nfc_mailbox_enable(bool enable);
esp_err_t nfc_mailbox_get_status(uint8_t *status, size_t *message_len);
esp_err_t nfc_mailbox_read(uint8_t offset, uint8_t *data, size_t len);
esp_err_t nfc_mailbox_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
