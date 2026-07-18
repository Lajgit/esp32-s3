#include "nfc.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NFC_I2C_PORT I2C_NUM_0
#define NFC_PIN_SCL GPIO_NUM_17
#define NFC_PIN_SDA GPIO_NUM_18
#define NFC_PIN_GPO GPIO_NUM_21
#define NFC_I2C_FREQ_HZ 100000

#define ST25DV_USER_I2C_ADDR   0x53
#define ST25DV_SYSTEM_I2C_ADDR 0x57
#define ST25DV_UID_ADDR        0x0018
#define ST25DV_WRITE_DELAY_MS 50
#define ST25DV_READ_RETRY_COUNT 10
#define ST25DV_NDEF_MAX_TEXT_LEN 64
#define ST25DV_NDEF_MAX_URI_LEN 64

static const char *TAG = "nfc";
static bool s_initialized;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_user_dev;
static i2c_master_dev_handle_t s_system_dev;

static void nfc_scan_i2c_bus(void)
{
    ESP_LOGI(TAG, "scan nfc i2c bus start");

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "found i2c device at 0x%02X", addr);
        }
    }

    ESP_LOGI(TAG, "scan nfc i2c bus end");
}

static i2c_master_dev_handle_t st25dv_get_device(uint8_t dev_addr)
{
    if (dev_addr == ST25DV_USER_I2C_ADDR) {
        return s_user_dev;
    }

    return s_system_dev;
}

static esp_err_t st25dv_read_device(uint8_t dev_addr, uint16_t mem_addr, uint8_t *data, size_t len)
{
    uint8_t addr_buf[2] = {
        (uint8_t)(mem_addr >> 8),
        (uint8_t)mem_addr,
    };
    esp_err_t ret = ESP_OK;

    for (int i = 0; i < ST25DV_READ_RETRY_COUNT; i++) {
        ret = i2c_master_transmit_receive(st25dv_get_device(dev_addr), addr_buf, sizeof(addr_buf),
                                          data, len, 100);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "read 0x%02X memory 0x%04X failed: %s", dev_addr, mem_addr,
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return ret;
}

static esp_err_t st25dv_write_page(uint16_t mem_addr, const uint8_t *data, size_t len)
{
    uint8_t buffer[2 + NFC_PAGE_SIZE] = {
        (uint8_t)(mem_addr >> 8),
        (uint8_t)mem_addr,
    };

    ESP_RETURN_ON_FALSE(len > 0 && len <= NFC_PAGE_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "write page length invalid");
    memcpy(&buffer[2], data, len);

    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_user_dev, buffer, len + 2, 100),
                        TAG, "write st25dv page failed");
    vTaskDelay(pdMS_TO_TICKS(ST25DV_WRITE_DELAY_MS));

    return ESP_OK;
}

esp_err_t nfc_init(void)
{
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = NFC_I2C_PORT,
        .sda_io_num = NFC_PIN_SDA,
        .scl_io_num = NFC_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_config, &s_i2c_bus), TAG,
                        "create nfc i2c bus failed");

    i2c_device_config_t user_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ST25DV_USER_I2C_ADDR,
        .scl_speed_hz = NFC_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &user_config, &s_user_dev), TAG,
                        "add st25dv user memory device failed");

    i2c_device_config_t system_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ST25DV_SYSTEM_I2C_ADDR,
        .scl_speed_hz = NFC_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &system_config, &s_system_dev), TAG,
                        "add st25dv system device failed");

    gpio_config_t gpo_config = {
        .pin_bit_mask = 1ULL << NFC_PIN_GPO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpo_config), TAG, "config nfc gpo failed");

    vTaskDelay(pdMS_TO_TICKS(10));
    nfc_scan_i2c_bus();

    uint8_t probe = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_device(ST25DV_USER_I2C_ADDR, 0x0000, &probe, 1), TAG,
                        "probe st25dv user memory failed");

    s_initialized = true;
    return ESP_OK;
}

esp_err_t nfc_read(uint16_t address, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "nfc is not initialized");
    ESP_RETURN_ON_FALSE(data || len == 0, ESP_ERR_INVALID_ARG, TAG, "read buffer invalid");
    ESP_RETURN_ON_FALSE((size_t)address + len <= NFC_USER_MEMORY_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "read range out of user memory");

    if (len == 0) {
        return ESP_OK;
    }

    return st25dv_read_device(ST25DV_USER_I2C_ADDR, address, data, len);
}

esp_err_t nfc_write(uint16_t address, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "nfc is not initialized");
    ESP_RETURN_ON_FALSE(data || len == 0, ESP_ERR_INVALID_ARG, TAG, "write data invalid");
    ESP_RETURN_ON_FALSE((size_t)address + len <= NFC_USER_MEMORY_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "write range out of user memory");

    while (len > 0) {
        size_t page_offset = address % NFC_PAGE_SIZE;
        size_t write_len = NFC_PAGE_SIZE - page_offset;
        if (write_len > len) {
            write_len = len;
        }

        ESP_RETURN_ON_ERROR(st25dv_write_page(address, data, write_len), TAG,
                            "write st25dv user memory failed");
        address += write_len;
        data += write_len;
        len -= write_len;
    }

    return ESP_OK;
}

esp_err_t nfc_read_uid(uint8_t uid[NFC_UID_SIZE])
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "nfc is not initialized");
    ESP_RETURN_ON_FALSE(uid, ESP_ERR_INVALID_ARG, TAG, "uid buffer invalid");

    return st25dv_read_device(ST25DV_SYSTEM_I2C_ADDR, ST25DV_UID_ADDR, uid, NFC_UID_SIZE);
}

esp_err_t nfc_write_ndef_text(const char *text)
{
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "ndef text invalid");

    size_t text_len = strlen(text);
    ESP_RETURN_ON_FALSE(text_len > 0 && text_len <= ST25DV_NDEF_MAX_TEXT_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "ndef text length invalid");

    uint8_t image[4 + 2 + 4 + 1 + 2 + ST25DV_NDEF_MAX_TEXT_LEN + 1] = {0};
    size_t index = 0;
    uint8_t ndef_payload_len = (uint8_t)(1 + 2 + text_len);
    uint8_t ndef_record_len = (uint8_t)(4 + ndef_payload_len);

    image[index++] = 0xE1;
    image[index++] = 0x40;
    image[index++] = 0x40;
    image[index++] = 0x05;

    image[index++] = 0x03;
    image[index++] = ndef_record_len;

    image[index++] = 0xD1;
    image[index++] = 0x01;
    image[index++] = ndef_payload_len;
    image[index++] = 0x54;
    image[index++] = 0x02;
    image[index++] = 'e';
    image[index++] = 'n';
    memcpy(&image[index], text, text_len);
    index += text_len;
    image[index++] = 0xFE;

    ESP_RETURN_ON_ERROR(nfc_write(0x0000, image, index), TAG, "write ndef text failed");

    return ESP_OK;
}

esp_err_t nfc_write_ndef_uri(const char *https_uri_suffix)
{
    ESP_RETURN_ON_FALSE(https_uri_suffix, ESP_ERR_INVALID_ARG, TAG, "ndef uri invalid");

    size_t uri_suffix_len = strlen(https_uri_suffix);
    ESP_RETURN_ON_FALSE(uri_suffix_len > 0 && uri_suffix_len <= ST25DV_NDEF_MAX_URI_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "ndef uri length invalid");

    uint8_t image[4 + 2 + 4 + 1 + ST25DV_NDEF_MAX_URI_LEN + 1] = {0};
    size_t index = 0;
    uint8_t ndef_payload_len = (uint8_t)(1 + uri_suffix_len);
    uint8_t ndef_record_len = (uint8_t)(4 + ndef_payload_len);

    image[index++] = 0xE1;
    image[index++] = 0x40;
    image[index++] = 0x40;
    image[index++] = 0x05;

    image[index++] = 0x03;
    image[index++] = ndef_record_len;

    image[index++] = 0xD1;
    image[index++] = 0x01;
    image[index++] = ndef_payload_len;
    image[index++] = 0x55;
    image[index++] = 0x04;
    memcpy(&image[index], https_uri_suffix, uri_suffix_len);
    index += uri_suffix_len;
    image[index++] = 0xFE;

    ESP_RETURN_ON_ERROR(nfc_write(0x0000, image, index), TAG, "write ndef uri failed");

    return ESP_OK;
}

esp_err_t nfc_self_test(void)
{
    static const char test_uri_suffix[] = "example.com/card";
    uint8_t readback[32] = {0};

    ESP_RETURN_ON_ERROR(nfc_write_ndef_uri(test_uri_suffix), TAG, "nfc write ndef self test failed");
    ESP_RETURN_ON_ERROR(nfc_read(0x0000, readback, sizeof(readback)), TAG,
                        "nfc read ndef self test failed");
    ESP_RETURN_ON_FALSE(readback[0] == 0xE1 && readback[4] == 0x03, ESP_FAIL, TAG,
                        "nfc ndef self test compare failed");

    return ESP_OK;
}
