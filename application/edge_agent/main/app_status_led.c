/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_status_led.h"

#include "sdkconfig.h"

#if CONFIG_APP_STATUS_LED_WS2812_GPIO >= 0
#include "esp_log.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static volatile bool s_provisioning_active;

#if CONFIG_APP_STATUS_LED_WS2812_GPIO >= 0
static const char *TAG = "status_led";
static led_strip_handle_t s_strip;
static TaskHandle_t s_task;

static void app_status_led_write(bool lit)
{
    if (!s_strip) {
        return;
    }
    if (!lit) {
        led_strip_clear(s_strip);
        return;
    }
    led_strip_set_pixel(s_strip, 0, 8, 4, 0);
    led_strip_refresh(s_strip);
}

static void app_status_led_task(void *arg)
{
    (void)arg;
    bool lit = false;

    while (1) {
        if (s_provisioning_active) {
            lit = !lit;
            app_status_led_write(lit);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_STATUS_LED_AP_BLINK_MS));
            continue;
        }

        if (lit) {
            lit = false;
            app_status_led_write(false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

esp_err_t app_status_led_init(void)
{
#if CONFIG_APP_STATUS_LED_WS2812_GPIO >= 0
    if (s_strip) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_APP_STATUS_LED_WS2812_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10000000,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize WS2812 on GPIO%d: %s",
                 CONFIG_APP_STATUS_LED_WS2812_GPIO,
                 esp_err_to_name(err));
        return err;
    }
    app_status_led_write(false);

    BaseType_t ok = xTaskCreate(app_status_led_task, "status_led", 2048, NULL, 1, &s_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create status LED task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WS2812 status LED initialized on GPIO%d", CONFIG_APP_STATUS_LED_WS2812_GPIO);
#endif
    return ESP_OK;
}

void app_status_led_set_provisioning(bool active)
{
    s_provisioning_active = active;
#if CONFIG_APP_STATUS_LED_WS2812_GPIO >= 0
    if (!active) {
        app_status_led_write(false);
    }
#endif
}
