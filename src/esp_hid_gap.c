#include <string.h>

#include "esp_hid_gap.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"

static const char *TAG = "HID_GAP";

static struct ble_hs_adv_fields s_adv_fields;

static int hid_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "GAP connect status=%d", event->connect.status);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "GAP disconnect reason=%d", event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "GAP adv complete reason=%d", event->adv_complete.reason);
        break;
    default:
        break;
    }
    return 0;
}

esp_err_t esp_hid_ble_gap_adv_init(uint16_t appearance, const char *device_name)
{
    static ble_uuid16_t hid_uuid;

    memset(&s_adv_fields, 0, sizeof(s_adv_fields));
    s_adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_adv_fields.tx_pwr_lvl_is_present = 1;
    s_adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    s_adv_fields.appearance = appearance;
    s_adv_fields.appearance_is_present = 1;

    s_adv_fields.name = (uint8_t *)device_name;
    s_adv_fields.name_len = (uint8_t)strlen(device_name);
    s_adv_fields.name_is_complete = 1;

    hid_uuid.u.type = BLE_UUID_TYPE_16;
    hid_uuid.value = 0x1812;
    s_adv_fields.uuids16 = &hid_uuid;
    s_adv_fields.num_uuids16 = 1;
    s_adv_fields.uuids16_is_complete = 1;

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    return ESP_OK;
}

esp_err_t esp_hid_ble_gap_adv_start(void)
{
    int rc;
    struct ble_gap_adv_params adv_params;

    rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, hid_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t esp_hid_gap_init(uint8_t mode)
{
    (void)mode;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    bt_cfg.mode = ESP_BT_MODE_BLE;
#endif

    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_nimble_init();
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}
