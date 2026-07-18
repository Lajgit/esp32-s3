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

#define ST25DV_DEFAULT_USER_I2C_ADDR   0x53
#define ST25DV_DEFAULT_SYSTEM_I2C_ADDR 0x57

#define ST25DV_GPO1_ADDR       0x0000
#define ST25DV_FTM_ADDR        0x000D
#define ST25DV_MEM_SIZE_ADDR   0x0014
#define ST25DV_BLOCK_SIZE_ADDR 0x0016
#define ST25DV_IC_REF_ADDR     0x0017
#define ST25DV_UID_ADDR        0x0018

#define ST25DV_GPO_CTRL_DYN_ADDR 0x2000
#define ST25DV_IT_STS_DYN_ADDR   0x2005
#define ST25DV_MB_CTRL_DYN_ADDR  0x2006
#define ST25DV_MB_LEN_DYN_ADDR   0x2007
#define ST25DV_MAILBOX_ADDR      0x2008

#define ST25DV_GPO_ENABLE         (1U << 0)
#define ST25DV_GPO_FIELD_CHANGE   (1U << 4)
#define ST25DV_FTM_MB_MODE        (1U << 0)
#define ST25DV_WRITE_TIMEOUT_MS   50
#define ST25DV_READ_RETRY_COUNT   10
#define ST25DV_NDEF_MAX_TEXT_LEN  64
#define ST25DV_NDEF_MAX_URI_LEN   64
#define ST25DV_NDEF_IMAGE_MAX_LEN 80

static const char *TAG = "nfc";
static bool s_initialized;
static volatile bool s_gpo_interrupt_pending;
static uint8_t s_user_i2c_addr = ST25DV_DEFAULT_USER_I2C_ADDR;
static uint8_t s_system_i2c_addr = ST25DV_DEFAULT_SYSTEM_I2C_ADDR;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_user_dev;
static i2c_master_dev_handle_t s_system_dev;

static bool st25dv_uid_is_valid(const uint8_t uid[NFC_UID_SIZE])
{
    return uid[6] == 0x02 && uid[7] == 0xE0;
}

static esp_err_t st25dv_add_device(uint8_t address, i2c_master_dev_handle_t *device)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = NFC_I2C_FREQ_HZ,
    };

    return i2c_master_bus_add_device(s_i2c_bus, &config, device);
}

static esp_err_t st25dv_read_handle(i2c_master_dev_handle_t device, uint8_t dev_addr,
                                    uint16_t mem_addr, uint8_t *data, size_t len)
{
    uint8_t addr_buf[2] = {
        (uint8_t)(mem_addr >> 8),
        (uint8_t)mem_addr,
    };
    esp_err_t ret = ESP_OK;

    for (int i = 0; i < ST25DV_READ_RETRY_COUNT; i++) {
        ret = i2c_master_transmit_receive(device, addr_buf, sizeof(addr_buf), data, len, 100);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "读取设备0x%02X地址0x%04X失败：%s", dev_addr, mem_addr,
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return ret;
}

static esp_err_t st25dv_write_handle(i2c_master_dev_handle_t device, uint8_t dev_addr,
                                     uint16_t mem_addr, const uint8_t *data, size_t len)
{
    uint8_t buffer[2 + NFC_MAILBOX_SIZE] = {
        (uint8_t)(mem_addr >> 8),
        (uint8_t)mem_addr,
    };

    ESP_RETURN_ON_FALSE(len > 0 && len <= NFC_MAILBOX_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "写入长度无效");
    memcpy(&buffer[2], data, len);

    esp_err_t ret = i2c_master_transmit(device, buffer, len + 2, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "写入设备0x%02X地址0x%04X失败：%s", dev_addr, mem_addr,
                 esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t st25dv_wait_user_memory_ready(void)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(ST25DV_WRITE_TIMEOUT_MS);
    esp_err_t ret = ESP_ERR_TIMEOUT;

    do {
        ret = i2c_master_probe(s_i2c_bus, s_user_i2c_addr, 5);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((xTaskGetTickCount() - start) < timeout);

    return ret;
}

static esp_err_t st25dv_read_user(uint16_t mem_addr, uint8_t *data, size_t len)
{
    return st25dv_read_handle(s_user_dev, s_user_i2c_addr, mem_addr, data, len);
}

static esp_err_t st25dv_read_system(uint16_t mem_addr, uint8_t *data, size_t len)
{
    return st25dv_read_handle(s_system_dev, s_system_i2c_addr, mem_addr, data, len);
}

static esp_err_t st25dv_write_user_page(uint16_t mem_addr, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(len > 0 && len <= NFC_PAGE_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "单次EEPROM写入长度无效");
    ESP_RETURN_ON_ERROR(st25dv_write_handle(s_user_dev, s_user_i2c_addr, mem_addr, data, len),
                        TAG, "写入用户存储区失败");
    ESP_RETURN_ON_ERROR(st25dv_wait_user_memory_ready(), TAG, "等待EEPROM写入完成超时");

    return ESP_OK;
}

static esp_err_t st25dv_write_dynamic(uint16_t mem_addr, uint8_t value)
{
    return st25dv_write_handle(s_user_dev, s_user_i2c_addr, mem_addr, &value, 1);
}

static esp_err_t st25dv_find_i2c_addresses(uint8_t *user_addr, uint8_t *system_addr)
{
    ESP_LOGI(TAG, "开始扫描NFC I2C总线");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "发现I2C设备：0x%02X", addr);
        }
    }

    /* 用户区地址和系统区地址只相差E2位，通过UID确认设备，兼容被修改过的I2C_CFG。 */
    for (uint8_t candidate = 0x08; candidate <= 0x73; candidate++) {
        if ((candidate & 0x06) != 0x02) {
            continue;
        }

        uint8_t candidate_system = candidate | 0x04;
        if (i2c_master_probe(s_i2c_bus, candidate, 20) != ESP_OK ||
            i2c_master_probe(s_i2c_bus, candidate_system, 20) != ESP_OK) {
            continue;
        }

        i2c_master_dev_handle_t system_device = NULL;
        esp_err_t ret = st25dv_add_device(candidate_system, &system_device);
        if (ret != ESP_OK) {
            continue;
        }

        uint8_t uid[NFC_UID_SIZE] = {0};
        ret = st25dv_read_handle(system_device, candidate_system, ST25DV_UID_ADDR, uid,
                                 sizeof(uid));
        i2c_master_bus_rm_device(system_device);

        if (ret == ESP_OK && st25dv_uid_is_valid(uid)) {
            *user_addr = candidate;
            *system_addr = candidate_system;
            ESP_LOGI(TAG, "确认ST25DV用户区地址0x%02X，系统区地址0x%02X", *user_addr,
                     *system_addr);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static void IRAM_ATTR nfc_gpo_isr(void *arg)
{
    (void)arg;
    s_gpo_interrupt_pending = true;
}

static esp_err_t st25dv_configure_gpo(void)
{
    uint8_t gpo1 = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_system(ST25DV_GPO1_ADDR, &gpo1, 1), TAG,
                        "读取GPO1配置失败");

    if ((gpo1 & ST25DV_GPO_FIELD_CHANGE) == 0) {
        ESP_LOGW(TAG, "GPO1未启用射频场变化中断，需在安全会话中配置系统寄存器");
    }

    /* 动态使能GPO输出不需要密码，不修改芯片的永久系统配置。 */
    ESP_RETURN_ON_ERROR(st25dv_write_dynamic(ST25DV_GPO_CTRL_DYN_ADDR, ST25DV_GPO_ENABLE), TAG,
                        "动态使能GPO失败");

    uint8_t gpo_ctrl = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_GPO_CTRL_DYN_ADDR, &gpo_ctrl, 1), TAG,
                        "读取GPO动态状态失败");
    ESP_RETURN_ON_FALSE((gpo_ctrl & ST25DV_GPO_ENABLE) != 0, ESP_FAIL, TAG,
                        "GPO动态使能校验失败");

    /* 读取IT_STS_Dyn会清除上电前遗留的中断状态。 */
    uint8_t ignored_status = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_IT_STS_DYN_ADDR, &ignored_status, 1), TAG,
                        "清除GPO中断状态失败");

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(NFC_PIN_GPO, nfc_gpo_isr, NULL), TAG,
                        "注册GPO中断失败");
    return ESP_OK;
}

static esp_err_t st25dv_write_and_verify(const uint8_t *image, size_t len)
{
    uint8_t readback[ST25DV_NDEF_IMAGE_MAX_LEN] = {0};

    ESP_RETURN_ON_FALSE(len <= sizeof(readback), ESP_ERR_INVALID_SIZE, TAG, "NDEF镜像过大");
    ESP_RETURN_ON_ERROR(nfc_write(0x0000, image, len), TAG, "写入NDEF失败");
    ESP_RETURN_ON_ERROR(nfc_read(0x0000, readback, len), TAG, "回读NDEF失败");
    ESP_RETURN_ON_FALSE(memcmp(image, readback, len) == 0, ESP_FAIL, TAG,
                        "NDEF完整回读校验失败");

    return ESP_OK;
}

esp_err_t nfc_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_config = {
        .i2c_port = NFC_I2C_PORT,
        .sda_io_num = NFC_PIN_SDA,
        .scl_io_num = NFC_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_config, &s_i2c_bus), TAG,
                        "创建NFC I2C总线失败");

    ESP_RETURN_ON_ERROR(st25dv_find_i2c_addresses(&s_user_i2c_addr, &s_system_i2c_addr), TAG,
                        "未找到有效的ST25DV04KC");
    ESP_RETURN_ON_ERROR(st25dv_add_device(s_user_i2c_addr, &s_user_dev), TAG,
                        "添加ST25DV用户区设备失败");
    ESP_RETURN_ON_ERROR(st25dv_add_device(s_system_i2c_addr, &s_system_dev), TAG,
                        "添加ST25DV系统区设备失败");

    gpio_config_t gpo_config = {
        .pin_bit_mask = 1ULL << NFC_PIN_GPO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpo_config), TAG, "配置NFC GPO失败");

    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t probe = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_user(0x0000, &probe, 1), TAG, "探测用户存储区失败");

    uint8_t uid[NFC_UID_SIZE] = {0};
    ESP_RETURN_ON_ERROR(st25dv_read_system(ST25DV_UID_ADDR, uid, sizeof(uid)), TAG,
                        "探测系统区UID失败");
    ESP_RETURN_ON_FALSE(st25dv_uid_is_valid(uid), ESP_ERR_INVALID_RESPONSE, TAG,
                        "UID厂商标识不符合ST25DV");

    ESP_RETURN_ON_ERROR(st25dv_configure_gpo(), TAG, "初始化GPO功能失败");

    s_initialized = true;
    return ESP_OK;
}

esp_err_t nfc_read(uint16_t address, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");
    ESP_RETURN_ON_FALSE(data || len == 0, ESP_ERR_INVALID_ARG, TAG, "读取缓冲区无效");
    ESP_RETURN_ON_FALSE((size_t)address + len <= NFC_USER_MEMORY_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "读取范围超出用户存储区");

    if (len == 0) {
        return ESP_OK;
    }

    return st25dv_read_user(address, data, len);
}

esp_err_t nfc_write(uint16_t address, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");
    ESP_RETURN_ON_FALSE(data || len == 0, ESP_ERR_INVALID_ARG, TAG, "写入数据无效");
    ESP_RETURN_ON_FALSE((size_t)address + len <= NFC_USER_MEMORY_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "写入范围超出用户存储区");

    uint8_t mailbox_status = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_MB_CTRL_DYN_ADDR, &mailbox_status, 1), TAG,
                        "读取Mailbox状态失败");

    bool restore_mailbox = (mailbox_status & NFC_MAILBOX_ENABLED) != 0;
    if (restore_mailbox) {
        ESP_RETURN_ON_FALSE(
            (mailbox_status & (NFC_MAILBOX_HOST_PUT_MSG | NFC_MAILBOX_RF_PUT_MSG)) == 0,
            ESP_ERR_INVALID_STATE, TAG, "Mailbox存在未处理消息，不能写入用户EEPROM");
        ESP_RETURN_ON_ERROR(st25dv_write_dynamic(ST25DV_MB_CTRL_DYN_ADDR, 0), TAG,
                            "写入EEPROM前关闭Mailbox失败");
    }

    esp_err_t ret = ESP_OK;
    while (len > 0) {
        size_t page_offset = address % NFC_PAGE_SIZE;
        size_t write_len = NFC_PAGE_SIZE - page_offset;
        if (write_len > len) {
            write_len = len;
        }

        ret = st25dv_write_user_page(address, data, write_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "写入ST25DV用户存储区失败：%s", esp_err_to_name(ret));
            break;
        }
        address += write_len;
        data += write_len;
        len -= write_len;
    }

    if (restore_mailbox) {
        esp_err_t restore_ret =
            st25dv_write_dynamic(ST25DV_MB_CTRL_DYN_ADDR, NFC_MAILBOX_ENABLED);
        if (ret == ESP_OK) {
            ret = restore_ret;
        }
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "恢复Mailbox使能状态失败：%s", esp_err_to_name(restore_ret));
        }
    }

    return ret;
}

esp_err_t nfc_read_uid(uint8_t uid[NFC_UID_SIZE])
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");
    ESP_RETURN_ON_FALSE(uid, ESP_ERR_INVALID_ARG, TAG, "UID缓冲区无效");

    ESP_RETURN_ON_ERROR(st25dv_read_system(ST25DV_UID_ADDR, uid, NFC_UID_SIZE), TAG,
                        "读取UID失败");
    ESP_RETURN_ON_FALSE(st25dv_uid_is_valid(uid), ESP_ERR_INVALID_RESPONSE, TAG,
                        "UID厂商标识无效");
    return ESP_OK;
}

esp_err_t nfc_write_ndef_text(const char *text)
{
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "NDEF文本无效");

    size_t text_len = strlen(text);
    ESP_RETURN_ON_FALSE(text_len > 0 && text_len <= ST25DV_NDEF_MAX_TEXT_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "NDEF文本长度无效");

    uint8_t image[ST25DV_NDEF_IMAGE_MAX_LEN] = {0};
    size_t index = 0;
    uint8_t ndef_payload_len = (uint8_t)(1 + 2 + text_len);
    uint8_t ndef_record_len = (uint8_t)(4 + ndef_payload_len);

    /* NFC Forum Type 5四字节CC文件，末字节保留位必须保持为0。 */
    image[index++] = 0xE1;
    image[index++] = 0x40;
    image[index++] = 0x40;
    image[index++] = 0x00;

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

    return st25dv_write_and_verify(image, index);
}

esp_err_t nfc_write_ndef_uri(const char *https_uri_suffix)
{
    ESP_RETURN_ON_FALSE(https_uri_suffix, ESP_ERR_INVALID_ARG, TAG, "NDEF URI无效");

    size_t uri_suffix_len = strlen(https_uri_suffix);
    ESP_RETURN_ON_FALSE(uri_suffix_len > 0 && uri_suffix_len <= ST25DV_NDEF_MAX_URI_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "NDEF URI长度无效");

    uint8_t image[ST25DV_NDEF_IMAGE_MAX_LEN] = {0};
    size_t index = 0;
    uint8_t ndef_payload_len = (uint8_t)(1 + uri_suffix_len);
    uint8_t ndef_record_len = (uint8_t)(4 + ndef_payload_len);

    /* 0x04表示https://前缀，调用者只传入域名及路径。 */
    image[index++] = 0xE1;
    image[index++] = 0x40;
    image[index++] = 0x40;
    image[index++] = 0x00;

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

    return st25dv_write_and_verify(image, index);
}

esp_err_t nfc_ensure_ndef_uri(const char *https_uri_suffix)
{
    ESP_RETURN_ON_FALSE(https_uri_suffix, ESP_ERR_INVALID_ARG, TAG, "默认NDEF URI无效");

    uint8_t header[6] = {0};
    ESP_RETURN_ON_ERROR(nfc_read(0x0000, header, sizeof(header)), TAG, "读取NDEF头失败");

    /* 已存在有效四字节CC和NDEF TLV时保留原数据，避免每次开机重复覆盖EEPROM。 */
    if (header[0] == 0xE1 && (header[1] & 0xF0) == 0x40 &&
        (header[2] == 0x3F || header[2] == 0x40) && header[4] == 0x03 && header[5] > 0) {
        if (header[3] == 0x05) {
            uint8_t fixed_feature = 0x00;
            ESP_LOGI(TAG, "修复旧版本写入的CC保留位0x05");
            ESP_RETURN_ON_ERROR(nfc_write(0x0003, &fixed_feature, 1), TAG, "修复CC文件失败");
        }
        ESP_LOGI(TAG, "检测到已有NDEF数据，保持原内容不变");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "未检测到有效NDEF数据，写入默认URI");
    return nfc_write_ndef_uri(https_uri_suffix);
}

esp_err_t nfc_self_test(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");

    uint8_t uid[NFC_UID_SIZE] = {0};
    ESP_RETURN_ON_ERROR(nfc_read_uid(uid), TAG, "UID自检失败");

    uint8_t memory_info[4] = {0};
    ESP_RETURN_ON_ERROR(st25dv_read_system(ST25DV_MEM_SIZE_ADDR, memory_info, sizeof(memory_info)),
                        TAG, "读取芯片容量信息失败");
    ESP_RETURN_ON_FALSE(memory_info[0] == 0x7F && memory_info[1] == 0x00 &&
                            memory_info[2] == 0x03 && memory_info[3] == 0x50,
                        ESP_ERR_INVALID_RESPONSE, TAG, "芯片容量或型号不是ST25DV04KC");

    uint8_t gpo_ctrl = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_GPO_CTRL_DYN_ADDR, &gpo_ctrl, 1), TAG,
                        "读取GPO动态寄存器失败");
    ESP_RETURN_ON_FALSE((gpo_ctrl & ST25DV_GPO_ENABLE) != 0, ESP_FAIL, TAG,
                        "GPO动态输出未使能");

    uint8_t user_probe = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_user(0x0000, &user_probe, 1), TAG, "用户区读取自检失败");

    ESP_LOGI(TAG, "ST25DV04KC UID：%02X%02X%02X%02X%02X%02X%02X%02X", uid[7], uid[6],
             uid[5], uid[4], uid[3], uid[2], uid[1], uid[0]);
    return ESP_OK;
}

bool nfc_gpo_is_active(void)
{
    /* 当前硬件使用8引脚开漏GPO，低电平表示中断有效。 */
    return gpio_get_level(NFC_PIN_GPO) == 0;
}

esp_err_t nfc_get_interrupt_status(uint8_t *status)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "中断状态缓冲区无效");

    if (!s_gpo_interrupt_pending && !nfc_gpo_is_active()) {
        return ESP_ERR_NOT_FOUND;
    }

    s_gpo_interrupt_pending = false;
    ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_IT_STS_DYN_ADDR, status, 1), TAG,
                        "读取NFC中断状态失败");

    return *status == 0 ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t nfc_mailbox_enable(bool enable)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");

    uint8_t ftm = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_system(ST25DV_FTM_ADDR, &ftm, 1), TAG,
                        "读取FTM配置失败");

    if (enable && (ftm & ST25DV_FTM_MB_MODE) == 0) {
        ESP_LOGW(TAG, "芯片未授权Mailbox模式，需先在I2C安全会话中设置FTM.MB_MODE");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t value = enable ? NFC_MAILBOX_ENABLED : 0;
    if (!enable && (ftm & ST25DV_FTM_MB_MODE) == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(st25dv_write_dynamic(ST25DV_MB_CTRL_DYN_ADDR, value), TAG,
                        "设置Mailbox动态状态失败");

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_MB_CTRL_DYN_ADDR, &status, 1), TAG,
                        "回读Mailbox动态状态失败");
    ESP_RETURN_ON_FALSE(((status & NFC_MAILBOX_ENABLED) != 0) == enable, ESP_FAIL, TAG,
                        "Mailbox使能状态校验失败");
    return ESP_OK;
}

esp_err_t nfc_mailbox_get_status(uint8_t *status, size_t *message_len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "Mailbox状态缓冲区无效");

    ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_MB_CTRL_DYN_ADDR, status, 1), TAG,
                        "读取Mailbox状态失败");

    if (message_len) {
        *message_len = 0;
        if ((*status & (NFC_MAILBOX_HOST_PUT_MSG | NFC_MAILBOX_RF_PUT_MSG)) != 0) {
            uint8_t length_minus_one = 0;
            ESP_RETURN_ON_ERROR(st25dv_read_user(ST25DV_MB_LEN_DYN_ADDR, &length_minus_one, 1),
                                TAG, "读取Mailbox消息长度失败");
            *message_len = (size_t)length_minus_one + 1;
        }
    }

    return ESP_OK;
}

esp_err_t nfc_mailbox_read(uint8_t offset, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");
    ESP_RETURN_ON_FALSE(data || len == 0, ESP_ERR_INVALID_ARG, TAG, "Mailbox读取缓冲区无效");
    ESP_RETURN_ON_FALSE((size_t)offset + len <= NFC_MAILBOX_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "Mailbox读取范围无效");

    if (len == 0) {
        return ESP_OK;
    }

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(nfc_mailbox_get_status(&status, NULL), TAG, "检查Mailbox状态失败");
    ESP_RETURN_ON_FALSE((status & NFC_MAILBOX_ENABLED) != 0, ESP_ERR_INVALID_STATE, TAG,
                        "Mailbox尚未使能");

    return st25dv_read_user((uint16_t)(ST25DV_MAILBOX_ADDR + offset), data, len);
}

esp_err_t nfc_mailbox_write(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "NFC尚未初始化");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Mailbox写入数据无效");
    ESP_RETURN_ON_FALSE(len > 0 && len <= NFC_MAILBOX_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "Mailbox写入长度无效");

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(nfc_mailbox_get_status(&status, NULL), TAG, "检查Mailbox状态失败");
    ESP_RETURN_ON_FALSE((status & NFC_MAILBOX_ENABLED) != 0, ESP_ERR_INVALID_STATE, TAG,
                        "Mailbox尚未使能");
    ESP_RETURN_ON_FALSE((status & (NFC_MAILBOX_HOST_PUT_MSG | NFC_MAILBOX_RF_PUT_MSG)) == 0,
                        ESP_ERR_INVALID_STATE, TAG, "Mailbox中仍有未处理消息");

    /* Mailbox写入必须从0x2008开始，并在同一次I2C事务中完成。 */
    ESP_RETURN_ON_ERROR(st25dv_write_handle(s_user_dev, s_user_i2c_addr, ST25DV_MAILBOX_ADDR,
                                            data, len),
                        TAG, "写入Mailbox失败");

    ESP_RETURN_ON_ERROR(nfc_mailbox_get_status(&status, NULL), TAG, "校验Mailbox写入状态失败");
    ESP_RETURN_ON_FALSE((status & NFC_MAILBOX_HOST_PUT_MSG) != 0, ESP_FAIL, TAG,
                        "Mailbox写入完成状态无效");
    return ESP_OK;
}
