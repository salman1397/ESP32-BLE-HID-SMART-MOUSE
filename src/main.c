#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#include "esp_hid_gap.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include <stdlib.h>

#define TAG "SMART_MOUSE"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define I2C_FREQ_HZ 400000

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_ADDR_PRIMARY 0x3C
#define OLED_ADDR_SECONDARY 0x3D
#define OLED_CONTROL_CMD 0x00
#define OLED_CONTROL_DATA 0x40

#define JOY_X_PIN ADC1_CHANNEL_5
#define JOY_Y_PIN ADC1_CHANNEL_4
#define JOY_SW_GPIO GPIO_NUM_25
#define JOY_CENTER_DEFAULT 2048
#define JOY_DEADZONE 300
#define SW_DEBOUNCE_MS 50
#define SW_HOLD_MS 500
#define SW_LONG_MS 3000

#define LOOP_MS 20
#define MENU_STEP_MS 220
#define DOUBLE_CLICK_MS 300

#define WIFI_SSID_CFG      "YOUR_WIFI_SSID"
#define WIFI_PASS_CFG      "YOUR_WIFI_PASSWORD"
#define WEATHER_API_KEY    "YOUR_OPENWEATHERMAP_API_KEY"
#define WEATHER_CONNECT_MS 15000
#define CITY_COUNT         4
#define SLEEP_TIMEOUT_MS   30000

#define WIFI_GOT_IP_BIT    BIT0
#define WIFI_FAIL_BIT      BIT1

#define BTN_LEFT 0x01
#define BTN_RIGHT 0x02
#define BTN_MIDDLE 0x04

typedef enum {
    MODE_NORMAL = 0,
    MODE_RIGHT_CURSOR,
    MODE_LEFT_CURSOR,
    MODE_SCROLL,
    MODE_GAME,
    MODE_WEATHER,
    MODE_COUNT
} mouse_mode_t;

typedef enum {
    UI_RUN = 0,
    UI_MENU,
    UI_LEVEL_SELECT,
    UI_WEATHER_CITY,
    UI_WEATHER_FETCH,
    UI_WEATHER_SHOW,
    UI_SLEEP,
} ui_state_t;

typedef enum {
    LEVEL_EASY = 0,
    LEVEL_MID,
    LEVEL_HARD
} game_level_t;

typedef struct {
    int raw_x;
    int raw_y;
    int center_x;
    int center_y;
    bool sw_raw;
    bool sw_stable;
    uint32_t sw_last_change_ms;
    uint32_t sw_press_start_ms;
    bool sw_long_fired;
    bool ev_short_release;
    bool ev_mid_release;
    bool ev_long_press;
} input_state_t;

typedef struct {
    float ball_x;
    float ball_y;
    float ball_vx;
    float ball_vy;
    float paddle_x;
    float speed_scale;
    uint32_t score;
    bool running;
    uint32_t last_speedup_ms;
} game_state_t;

typedef struct {
    float temp;
    float humidity;
    float wind_speed;
    char  description[32];
    char  city[16];
    bool  valid;
    bool  fetching;
    char  error[32];
} weather_data_t;

static uint8_t s_oled_addr = OLED_ADDR_PRIMARY;
static uint8_t s_oled_buffer[OLED_WIDTH * OLED_PAGES];

static esp_hidd_dev_t *s_hid_dev = NULL;
static bool s_ble_connected = false;

static mouse_mode_t s_mode = MODE_NORMAL;
static ui_state_t s_ui_state = UI_RUN;
static int s_menu_index = 0;
static game_level_t s_level = LEVEL_MID;

static input_state_t s_in = {
    .center_x = JOY_CENTER_DEFAULT,
    .center_y = JOY_CENTER_DEFAULT,
    .sw_raw = true,
    .sw_stable = true,
};

static game_state_t s_game = {0};

static weather_data_t      s_weather          = {0};
static int                 s_city_index       = 0;
static bool                s_wifi_ever_inited = false;
static EventGroupHandle_t  s_wifi_eg          = NULL;

static const char *s_city_names[]   = {"Bengaluru", "Kolkata", "Mumbai", "Delhi"};
static const char *s_city_queries[] = {
    "Bengaluru,IN", "Kolkata,IN", "Mumbai,IN", "Delhi,IN"
};

static uint8_t s_buttons = 0;
static bool s_scroll_smooth = true;
static uint32_t s_last_menu_step_ms = 0;
static uint32_t s_last_short_click_ms = 0;
static uint32_t s_last_activity_ms = 0;
static bool     s_battery_sent = false;

// 5x7 font for ASCII 0x20..0x7F
static const uint8_t font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E},
    {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07},
    {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00},
    {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C},
    {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00},
    {0x10, 0x08, 0x08, 0x10, 0x08}, {0x78, 0x46, 0x41, 0x46, 0x78}};

static const uint8_t mouse_report_map[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
    0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75,
    0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0
};

static esp_hid_raw_report_map_t report_maps[] = {
    {.data = (uint8_t *)mouse_report_map, .len = sizeof(mouse_report_map)},
};

static esp_hid_device_config_t hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x27DB,
    .version = 0x0100,
    .device_name = "Shushant Mouse",
    .manufacturer_name = "Shushant",
    .serial_number = "SM-0001",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t oled_send_cmd(uint8_t cmd)
{
    uint8_t payload[2] = {OLED_CONTROL_CMD, cmd};
    return i2c_master_write_to_device(I2C_PORT, s_oled_addr, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

static esp_err_t oled_send_data(const uint8_t *data, size_t len)
{
    uint8_t tx[17];
    tx[0] = OLED_CONTROL_DATA;
    while (len > 0) {
        size_t chunk = len > 16 ? 16 : len;
        memcpy(&tx[1], data, chunk);
        esp_err_t err = i2c_master_write_to_device(I2C_PORT, s_oled_addr, tx, chunk + 1, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static void oled_draw_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return;
    }
    uint16_t index = x + (y / 8) * OLED_WIDTH;
    uint8_t mask = (uint8_t)(1U << (y % 8));
    if (on) {
        s_oled_buffer[index] |= mask;
    } else {
        s_oled_buffer[index] &= (uint8_t)~mask;
    }
}

static void oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on)
{
    for (uint8_t yy = y; yy < (uint8_t)(y + h); yy++) {
        for (uint8_t xx = x; xx < (uint8_t)(x + w); xx++) {
            oled_draw_pixel(xx, yy, on);
        }
    }
}

static void oled_draw_char(uint8_t x, uint8_t y, char c)
{
    if (c < 32 || c > 127) {
        c = '?';
    }
    const uint8_t *glyph = font_5x7[(uint8_t)c - 32U];
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (uint8_t row = 0; row < 8; row++) {
            bool on = ((bits >> row) & 0x01U) != 0U;
            oled_draw_pixel((uint8_t)(x + col), (uint8_t)(y + row), on);
        }
    }
    for (uint8_t row = 0; row < 8; row++) {
        oled_draw_pixel((uint8_t)(x + 5), (uint8_t)(y + row), false);
    }
}

static void oled_draw_text(uint8_t x, uint8_t y, const char *text)
{
    uint8_t cx = x;
    uint8_t cy = y;
    while (*text != '\0') {
        if (*text == '\n') {
            cx = x;
            cy = (uint8_t)(cy + 8);
            text++;
            continue;
        }
        if ((cx + 6) > OLED_WIDTH) {
            cx = x;
            cy = (uint8_t)(cy + 8);
        }
        if ((cy + 8) > OLED_HEIGHT) {
            break;
        }
        oled_draw_char(cx, cy, *text);
        cx = (uint8_t)(cx + 6);
        text++;
    }
}

static void oled_clear_buffer(void)
{
    memset(s_oled_buffer, 0, sizeof(s_oled_buffer));
}

static esp_err_t oled_update_screen(void)
{
    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        esp_err_t err = oled_send_cmd((uint8_t)(0xB0U + page));
        if (err != ESP_OK) return err;
        err = oled_send_cmd(0x00);
        if (err != ESP_OK) return err;
        err = oled_send_cmd(0x10);
        if (err != ESP_OK) return err;
        err = oled_send_data(&s_oled_buffer[page * OLED_WIDTH], OLED_WIDTH);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t oled_probe(uint8_t addr)
{
    uint8_t payload[2] = {OLED_CONTROL_CMD, 0xAE};
    return i2c_master_write_to_device(I2C_PORT, addr, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

static esp_err_t oled_init_display(void)
{
    static const uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1,
        0xC8, 0xDA, 0x12, 0x81, 0x8F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        esp_err_t err = oled_send_cmd(init_cmds[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t oled_i2c_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return ESP_OK;
}

static int clamp_i32(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int8_t joy_axis_to_delta(int raw, int center, int max_step)
{
    int delta = raw - center;
    int abs_delta = delta >= 0 ? delta : -delta;
    if (abs_delta <= JOY_DEADZONE) {
        return 0;
    }

    int full_range = (delta >= 0) ? (4095 - center) : center;
    float denom = (float)(full_range - JOY_DEADZONE);
    if (denom < 1.0f) denom = 1.0f;
    float norm = (float)(abs_delta - JOY_DEADZONE) / denom;
    if (norm > 1.0f) norm = 1.0f;
    float curved = powf(norm, 1.45f);
    int val = (int)lroundf(curved * (float)max_step);
    if (delta < 0) val = -val;
    val = clamp_i32(val, -127, 127);
    return (int8_t)val;
}

static void hid_send_report(int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{
    if (s_hid_dev == NULL || !s_ble_connected) {
        return;
    }
    uint8_t report[5];
    report[0] = s_buttons;
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;
    report[3] = (uint8_t)wheel;
    report[4] = (uint8_t)pan;
    esp_hidd_dev_input_set(s_hid_dev, 0, 0, report, sizeof(report));
}

static void hid_click(uint8_t button)
{
    s_buttons |= button;
    hid_send_report(0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_buttons &= (uint8_t)~button;
    hid_send_report(0, 0, 0, 0);
}

static void hid_release_all(void)
{
    if (s_buttons != 0U) {
        s_buttons = 0;
        hid_send_report(0, 0, 0, 0);
    }
}

static void game_reset(void)
{
    s_game.ball_x = 64.0f;
    s_game.ball_y = 20.0f;
    s_game.ball_vx = (s_level == LEVEL_EASY) ? 22.0f : (s_level == LEVEL_MID ? 34.0f : 48.0f);
    s_game.ball_vy = 26.0f;
    s_game.paddle_x = 48.0f;
    s_game.speed_scale = 1.0f;
    s_game.score = 0;
    s_game.running = true;
    s_game.last_speedup_ms = now_ms();
}

static void game_update(float dt)
{
    if (!s_game.running) {
        return;
    }

    // Y axis = LEFT/RIGHT; negate so RIGHT(Y→0)→positive delta→paddle moves right
    int joy_delta = -(s_in.raw_y - s_in.center_y);
    s_game.paddle_x += (float)joy_delta * 0.0009f;
    if (s_game.paddle_x < 2.0f) s_game.paddle_x = 2.0f;
    if (s_game.paddle_x > 126.0f - 24.0f) s_game.paddle_x = 126.0f - 24.0f;

    float g = 92.0f * s_game.speed_scale;
    s_game.ball_vy += g * dt;
    s_game.ball_x += s_game.ball_vx * dt;
    s_game.ball_y += s_game.ball_vy * dt;

    if (s_game.ball_x <= 2.0f) {
        s_game.ball_x = 2.0f;
        s_game.ball_vx = fabsf(s_game.ball_vx);
    }
    if (s_game.ball_x >= 125.0f) {
        s_game.ball_x = 125.0f;
        s_game.ball_vx = -fabsf(s_game.ball_vx);
    }
    if (s_game.ball_y <= 10.0f) {
        s_game.ball_y = 10.0f;
        s_game.ball_vy = fabsf(s_game.ball_vy);
    }

    float paddle_y = 58.0f;
    float paddle_w = 24.0f;
    if (s_game.ball_y >= paddle_y - 1.0f && s_game.ball_y <= paddle_y + 3.0f && s_game.ball_vy > 0.0f) {
        if (s_game.ball_x >= s_game.paddle_x && s_game.ball_x <= s_game.paddle_x + paddle_w) {
            float rel = (s_game.ball_x - (s_game.paddle_x + paddle_w * 0.5f)) / (paddle_w * 0.5f);
            if (rel < -1.0f) rel = -1.0f;
            if (rel > 1.0f) rel = 1.0f;
            s_game.ball_vx += rel * 42.0f;
            s_game.ball_vy = -fabsf(s_game.ball_vy) * 0.96f - 12.0f;
            s_game.score++;
        }
    }

    if (s_game.ball_y > 63.0f) {
        s_game.running = false;
    }

    uint32_t t = now_ms();
    if ((t - s_game.last_speedup_ms) >= 5000U) {
        s_game.speed_scale *= 1.08f;
        s_game.last_speedup_ms = t;
    }
}

static void render_game(void)
{
    char line[24];
    oled_clear_buffer();
    snprintf(line, sizeof(line), "LV:%d SC:%lu", (int)s_level + 1, (unsigned long)s_game.score);
    oled_draw_text(2, 0, line);

    for (uint8_t x = 0; x < OLED_WIDTH; x++) {
        oled_draw_pixel(x, 9, true);
    }
    for (uint8_t y = 10; y < OLED_HEIGHT; y++) {
        oled_draw_pixel(0, y, true);
        oled_draw_pixel(127, y, true);
    }

    oled_fill_rect((uint8_t)s_game.paddle_x, 58, 24, 3, true);
    oled_fill_rect((uint8_t)s_game.ball_x, (uint8_t)s_game.ball_y, 2, 2, true);

    if (!s_game.running) {
        oled_draw_text(2, 24, "GAME OVER");
        oled_draw_text(2, 34, "SHORT PRESS RESTART");
    }
}

// ─── WiFi / Weather ─────────────────────────────────────────────────────────

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_GOT_IP_BIT);
    }
}

static esp_err_t wifi_sta_connect(void)
{
    s_wifi_eg = xEventGroupCreate();
    if (!s_wifi_eg) return ESP_ERR_NO_MEM;

    if (!s_wifi_ever_inited) {
        esp_netif_init();
        esp_err_t el = esp_event_loop_create_default();
        if (el != ESP_OK && el != ESP_ERR_INVALID_STATE) {
            vEventGroupDelete(s_wifi_eg);
            return el;
        }
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t wi = esp_wifi_init(&init_cfg);
        if (wi != ESP_OK) {
            vEventGroupDelete(s_wifi_eg);
            return wi;
        }
        s_wifi_ever_inited = true;
    }

    esp_event_handler_instance_t hnd_wifi, hnd_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_cb, NULL, &hnd_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_cb, NULL, &hnd_ip);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     WIFI_SSID_CFG,
            sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, WIFI_PASS_CFG,
            sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                                           WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WEATHER_CONNECT_MS));
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, hnd_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, hnd_ip);
    vEventGroupDelete(s_wifi_eg);
    s_wifi_eg = NULL;

    if (bits & WIFI_GOT_IP_BIT) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

static void wifi_sta_stop(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}

static esp_err_t weather_http_fetch(const char *city_q, weather_data_t *out)
{
    char url[256];
    snprintf(url, sizeof(url),
        "http://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=metric",
        city_q, WEATHER_API_KEY);

    static char resp_buf[2048];
    int resp_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(out->error, sizeof(out->error), "HTTP init fail");
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(out->error, sizeof(out->error), "HTTP open fail");
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int n;
    while (resp_len < (int)(sizeof(resp_buf) - 1)) {
        n = esp_http_client_read(client,
                                 resp_buf + resp_len,
                                 (int)(sizeof(resp_buf) - 1) - resp_len);
        if (n <= 0) break;
        resp_len += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (resp_len <= 0) {
        snprintf(out->error, sizeof(out->error), "No response");
        return ESP_FAIL;
    }
    resp_buf[resp_len] = '\0';

    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) {
        snprintf(out->error, sizeof(out->error), "JSON err");
        return ESP_FAIL;
    }
    cJSON *main_obj  = cJSON_GetObjectItem(root, "main");
    cJSON *weather_a = cJSON_GetObjectItem(root, "weather");
    cJSON *wind_obj  = cJSON_GetObjectItem(root, "wind");
    if (!main_obj || !weather_a || !wind_obj) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (msg && msg->valuestring) {
            snprintf(out->error, sizeof(out->error), "%.31s", msg->valuestring);
        } else {
            snprintf(out->error, sizeof(out->error), "Bad response");
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *temp_j = cJSON_GetObjectItem(main_obj, "temp");
    cJSON *hum_j  = cJSON_GetObjectItem(main_obj, "humidity");
    cJSON *wspd_j = cJSON_GetObjectItem(wind_obj,  "speed");
    cJSON *w0     = cJSON_GetArrayItem(weather_a, 0);
    cJSON *desc_j = w0 ? cJSON_GetObjectItem(w0, "description") : NULL;

    out->temp       = temp_j ? (float)temp_j->valuedouble : 0.0f;
    out->humidity   = hum_j  ? (float)hum_j->valuedouble  : 0.0f;
    out->wind_speed = wspd_j ? (float)wspd_j->valuedouble : 0.0f;
    if (desc_j && desc_j->valuestring) {
        strncpy(out->description, desc_j->valuestring,
                sizeof(out->description) - 1);
        out->description[sizeof(out->description) - 1] = '\0';
    } else {
        strcpy(out->description, "N/A");
    }
    out->valid = true;
    cJSON_Delete(root);
    return ESP_OK;
}

static void weather_task(void *pvarg)
{
    (void)pvarg;
    strncpy(s_weather.city, s_city_names[s_city_index],
            sizeof(s_weather.city) - 1);
    s_weather.city[sizeof(s_weather.city) - 1] = '\0';
    s_weather.error[0] = '\0';
    s_weather.valid    = false;

    esp_err_t err = wifi_sta_connect();
    if (err != ESP_OK) {
        if (s_weather.error[0] == '\0') {
            snprintf(s_weather.error, sizeof(s_weather.error), "WiFi fail");
        }
        s_weather.fetching = false;
        s_ui_state = UI_WEATHER_SHOW;
        vTaskDelete(NULL);
        return;
    }
    weather_http_fetch(s_city_queries[s_city_index], &s_weather);
    wifi_sta_stop();
    s_weather.fetching = false;
    s_ui_state = UI_WEATHER_SHOW;
    vTaskDelete(NULL);
}

// ─── Render functions ────────────────────────────────────────────────────────

static void render_menu(void)
{
    static const char *items[MODE_COUNT] = {
        "Normal Cursor",
        "Right+Cursor",
        "Left+Cursor",
        "Scroll Mode",
        "Game",
        "Weather"
    };
    oled_clear_buffer();
    oled_draw_text(2, 0, "MODE MENU");
    for (int i = 0; i < MODE_COUNT; i++) {
        char line[24];
        snprintf(line, sizeof(line), "%c %s",
                 (i == s_menu_index) ? '>' : ' ', items[i]);
        oled_draw_text(2, (uint8_t)(9 + i * 9), line);
    }
}

static void render_weather_city(void)
{
    oled_clear_buffer();
    oled_draw_text(2, 0, "WEATHER CITY");
    for (int i = 0; i < CITY_COUNT; i++) {
        char line[22];
        snprintf(line, sizeof(line), "%c %s",
                 (i == s_city_index) ? '>' : ' ', s_city_names[i]);
        oled_draw_text(2, (uint8_t)(12 + i * 10), line);
    }
    oled_draw_text(2, 56, "SW=OK LONG=back");
}

static void render_weather_show(void)
{
    oled_clear_buffer();
    if (s_weather.fetching) {
        oled_draw_text(2, 0,  "WEATHER");
        oled_draw_text(2, 16, "Connecting...");
        oled_draw_text(2, 26, "Please wait");
        return;
    }
    char line[28];
    if (!s_weather.valid) {
        oled_draw_text(2, 0,  "WEATHER ERR");
        oled_draw_text(2, 12, s_weather.error);
        oled_draw_text(2, 56, "LONG=back");
        return;
    }
    oled_draw_text(2, 0, s_weather.city);
    snprintf(line, sizeof(line), "Temp:  %.1fC", (double)s_weather.temp);
    oled_draw_text(2, 10, line);
    snprintf(line, sizeof(line), "Hum:   %.0f%%", (double)s_weather.humidity);
    oled_draw_text(2, 19, line);
    snprintf(line, sizeof(line), "Wind:  %.1fm/s", (double)s_weather.wind_speed);
    oled_draw_text(2, 28, line);
    oled_draw_text(2, 38, s_weather.description);
    oled_draw_text(2, 56, "<>city LONG=back");
}

static void render_level_menu(void)
{
    oled_clear_buffer();
    oled_draw_text(2, 0, "GAME LEVEL");
    oled_draw_text(2, 14, (s_level == LEVEL_EASY) ? "> Easy" : "  Easy");
    oled_draw_text(2, 24, (s_level == LEVEL_MID) ? "> Mid" : "  Mid");
    oled_draw_text(2, 34, (s_level == LEVEL_HARD) ? "> Hard" : "  Hard");
    oled_draw_text(2, 50, "SW=OK LONG=MENU");
}

static void render_run_status(void)
{
    const char *mode_name = "Normal";
    if (s_mode == MODE_RIGHT_CURSOR) mode_name = "Right+Cursor";
    if (s_mode == MODE_LEFT_CURSOR)  mode_name = "Left+Cursor";
    if (s_mode == MODE_SCROLL)       mode_name = "Scroll";
    if (s_mode == MODE_GAME)         mode_name = "Game";
    if (s_mode == MODE_WEATHER)      mode_name = "Weather";

    oled_clear_buffer();

    char line[30];
    snprintf(line, sizeof(line), "%s %s", s_ble_connected ? "BT:ON" : "BT:OFF", mode_name);
    oled_draw_text(2, 0, line);

    if (s_mode == MODE_NORMAL) {
        oled_draw_text(2, 14, "SW short: Left click");
        oled_draw_text(2, 24, "SW 500ms: Right click");
    } else if (s_mode == MODE_RIGHT_CURSOR) {
        snprintf(line, sizeof(line), "R:%s M:%s", (s_buttons & BTN_RIGHT) ? "HOLD" : "OFF", (s_buttons & BTN_MIDDLE) ? "HOLD" : "OFF");
        oled_draw_text(2, 14, line);
        oled_draw_text(2, 24, "Short: Toggle Right");
        oled_draw_text(2, 34, "500-3000: Toggle Mid");
    } else if (s_mode == MODE_LEFT_CURSOR) {
        snprintf(line, sizeof(line), "L:%s M:%s", (s_buttons & BTN_LEFT) ? "HOLD" : "OFF", (s_buttons & BTN_MIDDLE) ? "HOLD" : "OFF");
        oled_draw_text(2, 14, line);
        oled_draw_text(2, 24, "Short: Toggle Left");
        oled_draw_text(2, 34, "500-3000: Toggle Mid");
    } else if (s_mode == MODE_SCROLL) {
        oled_draw_text(2, 14, "Scroll: ^ v < >");
        oled_draw_text(2, 24, s_scroll_smooth ? "Style: Smooth" : "Style: Step");
        oled_draw_text(2, 34, "Short: Toggle style");
    }

    oled_draw_text(2, 54, "Hold 3s: Menu");
}

static void render_sleep(void)
{
    oled_clear_buffer();
    uint8_t design = (uint8_t)((now_ms() / 15000U) % 5U);

    switch (design) {

    case 0: { /* ── NESTED BOXES ── */
        for (int i = 0; i < 4; i++) {
            int m = i * 8 + 2;
            for (int x = m; x < 128 - m; x++) {
                oled_draw_pixel((uint8_t)x, (uint8_t)m,         true);
                oled_draw_pixel((uint8_t)x, (uint8_t)(63 - m),  true);
            }
            for (int y = m; y < 64 - m; y++) {
                oled_draw_pixel((uint8_t)m,         (uint8_t)y, true);
                oled_draw_pixel((uint8_t)(127 - m), (uint8_t)y, true);
            }
        }
        /* diagonal corner accents */
        for (int k = 0; k < 6; k++) {
            oled_draw_pixel((uint8_t)(2 + k), (uint8_t)(2 + k), true);
            oled_draw_pixel((uint8_t)(125 - k), (uint8_t)(2 + k), true);
            oled_draw_pixel((uint8_t)(2 + k), (uint8_t)(61 - k), true);
            oled_draw_pixel((uint8_t)(125 - k), (uint8_t)(61 - k), true);
        }
        oled_fill_rect(20, 22, 88, 20, false);
        oled_draw_text(22, 23, "** SHUSHANT **");
        oled_draw_text(34, 33, "MOUSE v3");
        break;
    }

    case 1: { /* ── CIRCUIT BOARD ── */
        static const uint8_t hy[5] = {6, 18, 32, 46, 58};
        static const uint8_t vx[7] = {14, 28, 46, 64, 82, 100, 114};
        /* horizontal traces */
        for (int i = 0; i < 5; i++)
            for (uint8_t x = 0; x < OLED_WIDTH; x++)
                oled_draw_pixel(x, hy[i], true);
        /* vertical bridges */
        for (int i = 0; i < 7; i++) {
            uint8_t y0 = hy[i % 4];
            uint8_t y1 = hy[(i % 4) + 1];
            for (uint8_t y = y0; y <= y1; y++)
                oled_draw_pixel(vx[i], y, true);
        }
        /* vias */
        for (int i = 0; i < 7; i++) {
            oled_fill_rect((uint8_t)(vx[i] - 2), (uint8_t)(hy[i % 4] - 2),     4, 4, true);
            oled_fill_rect((uint8_t)(vx[i] - 2), (uint8_t)(hy[(i%4)+1] - 2),   4, 4, true);
        }
        oled_fill_rect(26, 25, 76, 13, false);
        oled_draw_text(28, 27, "PCB  DESIGN");
        break;
    }

    case 2: { /* ── OSCILLOSCOPE ── */
        /* axes */
        for (uint8_t x = 0; x < OLED_WIDTH; x++) oled_draw_pixel(x, 31, true);
        for (uint8_t y = 0; y < OLED_HEIGHT; y++) oled_draw_pixel(0,  y, true);
        /* tick marks */
        for (uint8_t x = 16; x < OLED_WIDTH; x += 16)
            for (uint8_t y = 28; y <= 34; y++) oled_draw_pixel(x, y, true);
        /* two sine waves */
        for (int x = 1; x < OLED_WIDTH; x++) {
            float a  = (float)x * 6.2832f / 64.0f;
            int   y1 = 18 + (int)(11.0f * sinf(a));
            int   y2 = 47 + (int)(10.0f * sinf(a + 1.5708f));
            if (y1 >= 0 && y1 < 64) oled_draw_pixel((uint8_t)x, (uint8_t)y1, true);
            if (y2 >= 0 && y2 < 64) oled_draw_pixel((uint8_t)x, (uint8_t)y2, true);
        }
        oled_fill_rect(32, 28, 64, 9, false);
        oled_draw_text(34, 29, "~ SIGNAL ~");
        break;
    }

    case 3: { /* ── CROSSHAIR TARGET ── */
        /* full crosshair */
        for (uint8_t x = 0; x < OLED_WIDTH; x++) oled_draw_pixel(x, 31, true);
        for (uint8_t y = 0; y < OLED_HEIGHT; y++) oled_draw_pixel(63, y, true);
        /* concentric circles r=10,20,28 */
        static const uint8_t cr[3] = {10, 20, 28};
        for (int ri = 0; ri < 3; ri++) {
            for (int t = 0; t < 360; t += 2) {
                float rad = (float)t * 0.017453f;
                int px = 63 + (int)((float)cr[ri] * cosf(rad));
                int py = 31 + (int)((float)cr[ri] * sinf(rad));
                if (px >= 0 && px < 128 && py >= 0 && py < 64)
                    oled_draw_pixel((uint8_t)px, (uint8_t)py, true);
            }
        }
        oled_fill_rect(62, 30, 3, 3, false);
        oled_fill_rect(61, 29, 5, 5, true);
        oled_fill_rect(38, 25, 50, 13, false);
        oled_draw_text(40, 27, "ESP32 HID");
        break;
    }

    case 4: { /* ── BINARY RAIN ── */
        static const char *cols[8] = {
            "10110", "01001", "11010", "00101",
            "10011", "01110", "11001", "00110"
        };
        for (int c = 0; c < 8; c++)
            oled_draw_text((uint8_t)(c * 16), 2,  cols[c]);
        for (int c = 0; c < 8; c++)
            oled_draw_text((uint8_t)(c * 16), 44, cols[(c + 3) % 8]);
        /* centre banner */
        for (uint8_t x = 0; x < OLED_WIDTH; x++) {
            oled_draw_pixel(x, 20, true);
            oled_draw_pixel(x, 32, true);
        }
        oled_fill_rect(0, 21, OLED_WIDTH, 11, false);
        oled_draw_text(8, 23, "BLE MOUSE v3.0");
        break;
    }

    } /* switch */
}

static void ble_hid_event_cb(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;

    esp_hidd_event_t event = (esp_hidd_event_t)id;
    switch (event) {
        case ESP_HIDD_START_EVENT:
            esp_hid_ble_gap_adv_start();
            break;
        case ESP_HIDD_CONNECT_EVENT:
            s_ble_connected = true;
            break;
        case ESP_HIDD_DISCONNECT_EVENT:
            s_ble_connected = false;
            hid_release_all();
            esp_hid_ble_gap_adv_start();
            break;
        default:
            break;
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

static esp_err_t init_ble_mouse(void)
{
    esp_err_t err = esp_hid_gap_init(HID_DEV_MODE);
    if (err != ESP_OK) return err;

    err = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_MOUSE, hid_config.device_name);
    if (err != ESP_OK) return err;

    err = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, ble_hid_event_cb, &s_hid_dev);
    if (err != ESP_OK) return err;

    ble_store_config_init();
    err = esp_nimble_enable(ble_host_task);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

static void joystick_init(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(JOY_X_PIN, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(JOY_Y_PIN, ADC_ATTEN_DB_11);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << JOY_SW_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int32_t sx = 0;
    int32_t sy = 0;
    for (int i = 0; i < 48; i++) {
        sx += adc1_get_raw(JOY_X_PIN);
        sy += adc1_get_raw(JOY_Y_PIN);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_in.center_x = (int)(sx / 48);
    s_in.center_y = (int)(sy / 48);

    s_in.sw_raw = gpio_get_level(JOY_SW_GPIO) != 0;
    s_in.sw_stable = s_in.sw_raw;
    s_in.sw_last_change_ms = now_ms();
}

static int joy_vertical_step(void)
{
    // X axis controls UP/DOWN (X=0 = UP, X=4095 = DOWN)
    int d = s_in.raw_x - s_in.center_x;
    if (d < -1200) return -1;  // joystick UP
    if (d >  1200) return  1;  // joystick DOWN
    return 0;
}

static void update_input(void)
{
    uint32_t t = now_ms();
    s_in.ev_short_release = false;
    s_in.ev_mid_release = false;
    s_in.ev_long_press = false;

    s_in.raw_x = adc1_get_raw(JOY_X_PIN);
    s_in.raw_y = adc1_get_raw(JOY_Y_PIN);

    bool sw_now = gpio_get_level(JOY_SW_GPIO) != 0;
    if (sw_now != s_in.sw_raw) {
        s_in.sw_raw = sw_now;
        s_in.sw_last_change_ms = t;
    }

    if ((t - s_in.sw_last_change_ms) >= SW_DEBOUNCE_MS && s_in.sw_stable != s_in.sw_raw) {
        s_in.sw_stable = s_in.sw_raw;

        if (!s_in.sw_stable) {
            s_in.sw_press_start_ms = t;
            s_in.sw_long_fired = false;
        } else {
            uint32_t hold_ms = t - s_in.sw_press_start_ms;
            if (!s_in.sw_long_fired) {
                if (hold_ms >= SW_HOLD_MS && hold_ms < SW_LONG_MS) {
                    s_in.ev_mid_release = true;
                } else if (hold_ms < SW_HOLD_MS) {
                    s_in.ev_short_release = true;
                }
            }
        }
    }

    if (!s_in.sw_stable && !s_in.sw_long_fired) {
        if ((t - s_in.sw_press_start_ms) >= SW_LONG_MS) {
            s_in.sw_long_fired = true;
            s_in.ev_long_press = true;
        }
    }
}

static void apply_mode_change(mouse_mode_t mode)
{
    hid_release_all();
    s_mode = mode;
    if (mode == MODE_GAME) {
        s_ui_state = UI_LEVEL_SELECT;
    } else if (mode == MODE_WEATHER) {
        s_city_index = 0;
        s_ui_state = UI_WEATHER_CITY;
    } else {
        s_ui_state = UI_RUN;
    }
}

static void handle_menu_logic(void)
{
    uint32_t t = now_ms();
    int step = joy_vertical_step();
    if (step != 0 && (t - s_last_menu_step_ms) >= MENU_STEP_MS) {
        s_menu_index += step;
        if (s_menu_index < 0) s_menu_index = MODE_COUNT - 1;
        if (s_menu_index >= MODE_COUNT) s_menu_index = 0;
        s_last_menu_step_ms = t;
    }

    if (s_in.ev_short_release) {
        apply_mode_change((mouse_mode_t)s_menu_index);
    }
}

static void handle_level_logic(void)
{
    uint32_t t = now_ms();
    int step = joy_vertical_step();
    if (step != 0 && (t - s_last_menu_step_ms) >= MENU_STEP_MS) {
        int lv = (int)s_level + step;
        if (lv < 0) lv = 2;
        if (lv > 2) lv = 0;
        s_level = (game_level_t)lv;
        s_last_menu_step_ms = t;
    }

    if (s_in.ev_short_release) {
        game_reset();
        s_ui_state = UI_RUN;
    }
}

static void handle_weather_city_logic(void)
{
    uint32_t t = now_ms();
    int step = joy_vertical_step();
    if (step != 0 && (t - s_last_menu_step_ms) >= MENU_STEP_MS) {
        s_city_index += step;
        if (s_city_index < 0)          s_city_index = CITY_COUNT - 1;
        if (s_city_index >= CITY_COUNT) s_city_index = 0;
        s_last_menu_step_ms = t;
    }
    if (s_in.ev_short_release && !s_weather.fetching) {
        s_weather.fetching = true;
        s_ui_state = UI_WEATHER_FETCH;
        xTaskCreate(weather_task, "weather", 8192, NULL, 5, NULL);
    }
}

static void handle_weather_show_logic(void)
{
    // Left/right joystick (Y axis) toggles city and re-fetches
    uint32_t t = now_ms();
    int dy = s_in.raw_y - s_in.center_y;
    int city_step = 0;
    if (dy > 1200 && (t - s_last_menu_step_ms) >= MENU_STEP_MS) {
        city_step = -1;  // Y LEFT -> previous city
    } else if (dy < -1200 && (t - s_last_menu_step_ms) >= MENU_STEP_MS) {
        city_step = 1;   // Y RIGHT -> next city
    }
    if (city_step != 0) {
        s_city_index = (s_city_index + city_step + CITY_COUNT) % CITY_COUNT;
        s_last_menu_step_ms = t;
        s_weather.valid    = false;
        s_weather.fetching = true;
        s_weather.error[0] = '\0';
        s_ui_state = UI_WEATHER_FETCH;
        xTaskCreate(weather_task, "weather", 8192, NULL, 5, NULL);
    }
}

static void handle_run_logic(void)
{
    if (s_mode == MODE_GAME) {
        if (!s_game.running && s_in.ev_short_release) {
            game_reset();
        }
        game_update(0.02f);
        return;
    }

    // Y axis = LEFT/RIGHT (Y=4095=LEFT, Y=0=RIGHT): negate so RIGHT(Y→0)→+dx
    int8_t dx = (int8_t)(-joy_axis_to_delta(s_in.raw_y, s_in.center_y, 15));
    // X axis = UP/DOWN   (X=0=UP,    X=4095=DOWN):  X→4095 → +dy = down ✓
    int8_t dy = joy_axis_to_delta(s_in.raw_x, s_in.center_x, 15);
    int8_t wheel = 0;
    int8_t pan = 0;

    if (s_mode == MODE_SCROLL) {
        // X axis → wheel (UP → negative delta → negate → scroll up)
        int8_t sx = joy_axis_to_delta(s_in.raw_x, s_in.center_x, s_scroll_smooth ? 3 : 1);
        // Y axis → pan  (RIGHT → negative delta → negate → pan right)
        int8_t sy = joy_axis_to_delta(s_in.raw_y, s_in.center_y, s_scroll_smooth ? 3 : 1);
        wheel = (int8_t)(-sx);
        pan   = (int8_t)(-sy);
        dx = 0;
        dy = 0;

        if (s_in.ev_short_release) {
            s_scroll_smooth = !s_scroll_smooth;
        }
    } else {
        if (s_mode == MODE_NORMAL) {
            if (s_in.ev_short_release) {
                uint32_t t = now_ms();
                hid_click(BTN_LEFT);
                if ((t - s_last_short_click_ms) <= DOUBLE_CLICK_MS) {
                    hid_click(BTN_LEFT);
                }
                s_last_short_click_ms = t;
            }
            if (s_in.ev_mid_release) {
                hid_click(BTN_RIGHT);
            }
        } else if (s_mode == MODE_RIGHT_CURSOR) {
            if (s_in.ev_short_release) {
                s_buttons ^= BTN_RIGHT;
                hid_send_report(0, 0, 0, 0);
            }
            if (s_in.ev_mid_release) {
                s_buttons ^= BTN_MIDDLE;
                hid_send_report(0, 0, 0, 0);
            }
        } else if (s_mode == MODE_LEFT_CURSOR) {
            if (s_in.ev_short_release) {
                s_buttons ^= BTN_LEFT;
                hid_send_report(0, 0, 0, 0);
            }
            if (s_in.ev_mid_release) {
                s_buttons ^= BTN_MIDDLE;
                hid_send_report(0, 0, 0, 0);
            }
        }
    }

    if (dx != 0 || dy != 0 || wheel != 0 || pan != 0) {
        hid_send_report(dx, dy, wheel, pan);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(oled_i2c_init());
    if (oled_probe(OLED_ADDR_PRIMARY) == ESP_OK) {
        s_oled_addr = OLED_ADDR_PRIMARY;
    } else if (oled_probe(OLED_ADDR_SECONDARY) == ESP_OK) {
        s_oled_addr = OLED_ADDR_SECONDARY;
    } else {
        ESP_LOGE(TAG, "OLED not found at 0x3C or 0x3D");
        return;
    }
    ESP_ERROR_CHECK(oled_init_display());

    joystick_init();

    err = init_ble_mouse();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE mouse init failed: %s", esp_err_to_name(err));
    }

    uint32_t last_oled = 0;
    while (1) {
        update_input();

        /* Send battery level once per connection from main-loop context */
        if (s_ble_connected && !s_battery_sent) {
            esp_hidd_dev_battery_set(s_hid_dev, 100);
            s_battery_sent = true;
        } else if (!s_ble_connected) {
            s_battery_sent = false;
        }

        // Activity tracking and sleep transition
        {
            uint32_t ct = now_ms();
            if (s_in.ev_short_release || s_in.ev_mid_release || s_in.ev_long_press ||
                s_in.raw_x > s_in.center_x + JOY_DEADZONE ||
                s_in.raw_x < s_in.center_x - JOY_DEADZONE ||
                s_in.raw_y > s_in.center_y + JOY_DEADZONE ||
                s_in.raw_y < s_in.center_y - JOY_DEADZONE) {
                s_last_activity_ms = ct;
            }
            if (s_ui_state == UI_RUN &&
                (ct - s_last_activity_ms) > SLEEP_TIMEOUT_MS) {
                s_ui_state = UI_SLEEP;
            }
        }

        if (s_in.ev_long_press) {
            if (s_ui_state == UI_WEATHER_FETCH) {
                /* do not interrupt ongoing fetch */
            } else if (s_ui_state == UI_SLEEP) {
                s_ui_state = UI_RUN;
                s_last_activity_ms = now_ms();
            } else if (s_ui_state == UI_MENU) {
                s_ui_state = UI_RUN;
            } else {
                s_ui_state = UI_MENU;
                s_menu_index = (int)s_mode;
                hid_release_all();
            }
        }

        if (s_ui_state == UI_MENU) {
            handle_menu_logic();
        } else if (s_ui_state == UI_LEVEL_SELECT) {
            handle_level_logic();
        } else if (s_ui_state == UI_WEATHER_CITY) {
            handle_weather_city_logic();
        } else if (s_ui_state == UI_WEATHER_FETCH) {
            /* weather task running — nothing to process */
        } else if (s_ui_state == UI_WEATHER_SHOW) {
            handle_weather_show_logic();
        } else if (s_ui_state == UI_SLEEP) {
            if (s_in.ev_short_release || s_in.ev_mid_release ||
                s_in.raw_x > s_in.center_x + JOY_DEADZONE ||
                s_in.raw_x < s_in.center_x - JOY_DEADZONE ||
                s_in.raw_y > s_in.center_y + JOY_DEADZONE ||
                s_in.raw_y < s_in.center_y - JOY_DEADZONE) {
                s_ui_state = UI_RUN;
                s_last_activity_ms = now_ms();
            }
        } else {
            handle_run_logic();
        }

        uint32_t t = now_ms();
        if ((t - last_oled) >= 70U) {
            if (s_ui_state == UI_MENU) {
                render_menu();
            } else if (s_ui_state == UI_LEVEL_SELECT) {
                render_level_menu();
            } else if (s_ui_state == UI_WEATHER_CITY) {
                render_weather_city();
            } else if (s_ui_state == UI_WEATHER_FETCH ||
                       s_ui_state == UI_WEATHER_SHOW) {
                render_weather_show();
            } else if (s_ui_state == UI_SLEEP) {
                render_sleep();
            } else if (s_mode == MODE_GAME) {
                render_game();
            } else {
                render_run_status();
            }
            oled_update_screen();
            last_oled = t;
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
