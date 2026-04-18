#ifndef ESP_HID_GAP_H
#define ESP_HID_GAP_H

#include "esp_err.h"
#include "esp_hid_common.h"

#define HIDD_IDLE_MODE 0x00
#define HIDD_BLE_MODE 0x01

#if CONFIG_BT_NIMBLE_ENABLED
#define HID_DEV_MODE HIDD_BLE_MODE
#else
#define HID_DEV_MODE HIDD_IDLE_MODE
#endif

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_hid_gap_init(uint8_t mode);
esp_err_t esp_hid_ble_gap_adv_init(uint16_t appearance, const char *device_name);
esp_err_t esp_hid_ble_gap_adv_start(void);

#ifdef __cplusplus
}
#endif

#endif
