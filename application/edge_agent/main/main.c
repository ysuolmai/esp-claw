/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_fs.h"
#include "app_status_led.h"
#include "claw_paths.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "cap_lua.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"

#define APP_ENABLE_MEM_LOG        (0)

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;
static bool s_force_provision_this_boot;
static char s_voice_server_url[APP_CONFIG_STR_LEN];

#define APP_FORCE_PROVISION_MAGIC 0xC1A05A3U
#define APP_VOICE_PTT_SCRIPT_PATH "/system/scripts/voice_stream_supermini.lua"
#define APP_VOICE_PTT_JOB_NAME    "voice_ptt"
#define APP_VOICE_PTT_EXCLUSIVE   "voice"
#define APP_BUTTON_POLL_MS        50

static RTC_NOINIT_ATTR uint32_t s_force_provision_magic;

typedef struct {
    char uri[APP_CONFIG_STR_LEN];
    char escaped_uri[(APP_CONFIG_STR_LEN * 2) + 1];
    char args[(APP_CONFIG_STR_LEN * 2) + 256];
    char output[256];
} main_voice_ptt_start_buf_t;

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void log_wifi_startup_config(const app_config_t *config)
{
    ESP_LOGI(TAG,
             "Wi-Fi startup STA: ssid=%s pwd_len=%u",
             config->wifi_ssid[0] ? config->wifi_ssid : "(empty)",
             (unsigned)strlen(config->wifi_password));

    ESP_LOGI(TAG,
             "Wi-Fi startup AP: ssid=%s pwd_len=%u behavior=%s",
             config->ap_ssid[0] ? config->ap_ssid : "(auto:mac-suffix)",
             (unsigned)strlen(config->ap_password),
             config->ap_behavior[0] ? config->ap_behavior : "keep");
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    app_status_led_set_provisioning(status.ap_active && !status.sta_connected);

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }
}

static bool main_boot_button_configured(void)
{
#if CONFIG_APP_WIFI_FORCE_PROVISION_GPIO >= 0
    return CONFIG_APP_WIFI_FORCE_PROVISION_GPIO >= 0;
#else
    return false;
#endif
}

static bool main_voice_ptt_configured(void)
{
#if CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_AUDIO && CONFIG_APP_VOICE_PTT_GPIO >= 0
    return true;
#else
    return false;
#endif
}

static esp_err_t main_configure_boot_button_gpio(void)
{
#if CONFIG_APP_WIFI_FORCE_PROVISION_GPIO >= 0
    if (!main_boot_button_configured()) {
        return ESP_OK;
    }

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_APP_WIFI_FORCE_PROVISION_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
#else
    return ESP_OK;
#endif
}

static esp_err_t main_configure_voice_ptt_gpio(void)
{
#if CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_AUDIO && CONFIG_APP_VOICE_PTT_GPIO >= 0
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_APP_VOICE_PTT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_APP_VOICE_PTT_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = CONFIG_APP_VOICE_PTT_ACTIVE_LEVEL == 0 ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
#else
    return ESP_OK;
#endif
}

static bool main_boot_button_pressed_now(void)
{
#if CONFIG_APP_WIFI_FORCE_PROVISION_GPIO >= 0
    return main_boot_button_configured() &&
           gpio_get_level((gpio_num_t)CONFIG_APP_WIFI_FORCE_PROVISION_GPIO) == 0;
#else
    return false;
#endif
}

static bool main_voice_ptt_pressed_now(void)
{
#if CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_AUDIO && CONFIG_APP_VOICE_PTT_GPIO >= 0
    return main_voice_ptt_configured() &&
           gpio_get_level((gpio_num_t)CONFIG_APP_VOICE_PTT_GPIO) == CONFIG_APP_VOICE_PTT_ACTIVE_LEVEL;
#else
    return false;
#endif
}

static bool main_boot_button_is_held_for(uint32_t hold_ms)
{
#if CONFIG_APP_WIFI_FORCE_PROVISION_GPIO >= 0
    if (!main_boot_button_configured()) {
        return false;
    }
    if (gpio_get_level((gpio_num_t)CONFIG_APP_WIFI_FORCE_PROVISION_GPIO) != 0) {
        return false;
    }

    const uint32_t step_ms = 50;
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < hold_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        if (gpio_get_level((gpio_num_t)CONFIG_APP_WIFI_FORCE_PROVISION_GPIO) != 0) {
            return false;
        }
        elapsed_ms += step_ms;
    }
    return true;
#else
    (void)hold_ms;
    return false;
#endif
}

static bool main_json_escape_string(const char *src, char *dst, size_t dst_size)
{
    size_t off = 0;

    if (!src || !dst || dst_size == 0) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        const char *escape = NULL;
        char control_escape[7] = {0};

        switch (*p) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*p < 0x20) {
                snprintf(control_escape, sizeof(control_escape), "\\u%04x", (unsigned)*p);
                escape = control_escape;
            }
            break;
        }

        if (escape) {
            size_t len = strlen(escape);
            if (off + len >= dst_size) {
                return false;
            }
            memcpy(dst + off, escape, len);
            off += len;
            continue;
        }

        if (off + 1 >= dst_size) {
            return false;
        }
        dst[off++] = (char)*p;
    }

    dst[off] = '\0';
    return true;
}

static void main_load_voice_server_url(char *uri, size_t uri_size)
{
    app_config_t *config = NULL;
    bool loaded = false;

    if (!uri || uri_size == 0) {
        return;
    }
    uri[0] = '\0';

    config = calloc(1, sizeof(*config));
    if (config) {
        if (app_config_load(config) == ESP_OK) {
            strlcpy(uri, config->voice_server_url, uri_size);
            loaded = true;
        }
        free(config);
    }

    if (!loaded && !uri[0]) {
        strlcpy(uri, s_voice_server_url, uri_size);
    }
}

static esp_err_t main_start_voice_ptt_job(bool *out_consumed)
{
    if (out_consumed) {
        *out_consumed = false;
    }
#if CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_AUDIO && CONFIG_APP_VOICE_PTT_GPIO >= 0
    wifi_manager_status_t wifi_status = {0};
    main_voice_ptt_start_buf_t *buf = NULL;
    int len;
    esp_err_t err = ESP_OK;

    wifi_manager_get_status(&wifi_status);
    if (!wifi_status.sta_connected) {
        ESP_LOGW(TAG, "BOOT voice PTT ignored: STA Wi-Fi is not connected");
        return ESP_ERR_INVALID_STATE;
    }

    buf = calloc(1, sizeof(*buf));
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    main_load_voice_server_url(buf->uri, sizeof(buf->uri));
    if (buf->uri[0] == '\0') {
        ESP_LOGW(TAG, "BOOT voice PTT ignored: voice_server_url is empty");
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    if (!main_json_escape_string(buf->uri, buf->escaped_uri, sizeof(buf->escaped_uri))) {
        ESP_LOGW(TAG, "BOOT voice PTT ignored: voice_server_url is too long");
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (out_consumed) {
        *out_consumed = true;
    }

    len = snprintf(buf->args,
                   sizeof(buf->args),
                   "{\"uri\":\"%s\",\"record_ms\":%u,\"min_record_ms\":%u,"
                   "\"playback_timeout_ms\":%u,\"stop_gpio\":%d,\"stop_level\":%d}",
                   buf->escaped_uri,
                   (unsigned)CONFIG_APP_VOICE_PTT_MAX_RECORD_MS,
                   (unsigned)CONFIG_APP_VOICE_PTT_MIN_RECORD_MS,
                   (unsigned)CONFIG_APP_VOICE_PTT_PLAYBACK_TIMEOUT_MS,
                   CONFIG_APP_VOICE_PTT_GPIO,
                   CONFIG_APP_VOICE_PTT_ACTIVE_LEVEL == 0 ? 1 : 0);
    if (len <= 0 || len >= (int)sizeof(buf->args)) {
        ESP_LOGW(TAG, "BOOT voice PTT ignored: script args are too long");
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = cap_lua_run_script_async(APP_VOICE_PTT_SCRIPT_PATH,
                                   buf->args,
                                   0,
                                   APP_VOICE_PTT_JOB_NAME,
                                   APP_VOICE_PTT_EXCLUSIVE,
                                   true,
                                   buf->output,
                                   sizeof(buf->output));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BOOT voice PTT started: %s", buf->output);
    } else {
        ESP_LOGW(TAG, "BOOT voice PTT start failed: %s (%s)", esp_err_to_name(err), buf->output);
    }

cleanup:
    free(buf);
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static bool main_take_force_provision_request(void)
{
    if (s_force_provision_magic == APP_FORCE_PROVISION_MAGIC) {
        s_force_provision_magic = 0;
        ESP_LOGW(TAG, "One-shot BOOT provisioning request consumed; saved STA credentials are kept");
        return true;
    }

    if (!main_boot_button_configured()) {
        return false;
    }

    if (main_boot_button_is_held_for(CONFIG_APP_WIFI_FORCE_PROVISION_HOLD_MS)) {
        ESP_LOGW(TAG,
                 "BOOT GPIO%d held for %d ms during boot; forcing AP provisioning for this boot",
                 CONFIG_APP_WIFI_FORCE_PROVISION_GPIO,
                 CONFIG_APP_WIFI_FORCE_PROVISION_HOLD_MS);
        return true;
    }

    return false;
}

static void main_boot_button_monitor_task(void *arg)
{
    (void)arg;
#if CONFIG_APP_WIFI_FORCE_PROVISION_GPIO >= 0 || (CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_AUDIO && CONFIG_APP_VOICE_PTT_GPIO >= 0)
    uint32_t held_ms = 0;
    bool fired = false;
    bool ptt_press_active = false;
    bool ptt_started = false;
    bool ptt_consumed = false;

    while (1) {
        bool boot_held = main_boot_button_pressed_now();
        bool ptt_pressed = main_voice_ptt_pressed_now();

        if (ptt_pressed && !ptt_press_active) {
            bool consumed = false;
            ptt_started = main_start_voice_ptt_job(&consumed) == ESP_OK;
            ptt_consumed = consumed;
            if (ptt_consumed) {
                held_ms = 0;
                fired = false;
            }
        }
        if (!ptt_pressed && ptt_press_active && ptt_started) {
            ESP_LOGI(TAG, "BOOT voice PTT released; recording will stop via GPIO");
        }
        if (!ptt_pressed) {
            ptt_started = false;
            ptt_consumed = false;
        }
        ptt_press_active = ptt_pressed;

        if (boot_held && !ptt_consumed) {
            if (held_ms < CONFIG_APP_WIFI_FORCE_PROVISION_HOLD_MS) {
                held_ms += APP_BUTTON_POLL_MS;
            }
            if (!fired && held_ms >= CONFIG_APP_WIFI_FORCE_PROVISION_HOLD_MS) {
                fired = true;
                s_force_provision_magic = APP_FORCE_PROVISION_MAGIC;
                ESP_LOGW(TAG,
                         "BOOT GPIO%d held for %d ms; restarting into AP provisioning without clearing saved Wi-Fi",
                         CONFIG_APP_WIFI_FORCE_PROVISION_GPIO,
                         CONFIG_APP_WIFI_FORCE_PROVISION_HOLD_MS);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            held_ms = 0;
            fired = false;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
    }
#else
    vTaskDelete(NULL);
#endif
}

static void main_start_boot_button_monitor(void)
{
    if ((!main_boot_button_configured() && !main_voice_ptt_configured()) || s_force_provision_this_boot) {
        return;
    }

    BaseType_t ok = xTaskCreate(main_boot_button_monitor_task,
                                "boot_button",
                                main_voice_ptt_configured() ? 4096 : 2048,
                                NULL,
                                3,
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create BOOT button monitor task");
    } else {
        if (main_boot_button_configured()) {
            ESP_LOGI(TAG,
                     "BOOT GPIO%d long press (%d ms) will force Wi-Fi provisioning on next boot",
                     CONFIG_APP_WIFI_FORCE_PROVISION_GPIO,
                     CONFIG_APP_WIFI_FORCE_PROVISION_HOLD_MS);
        }
        if (main_voice_ptt_configured()) {
            ESP_LOGI(TAG,
                     "BOOT GPIO%d push-to-talk starts streaming voice when STA Wi-Fi and voice_server_url are ready",
                     CONFIG_APP_VOICE_PTT_GPIO);
        }
    }
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");

    return app_config_save(config);
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    esp_err_t ret = ESP_OK;
    cap_im_wechat_qr_login_status_t *raw = NULL;

    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    raw = calloc(1, sizeof(*raw));
    ESP_RETURN_ON_FALSE(raw, ESP_ERR_NO_MEM, TAG, "Failed to allocate login status");

    ESP_GOTO_ON_ERROR(cap_im_wechat_qr_login_get_status(raw), cleanup, TAG,
                      "Failed to query WeChat login status");

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));

cleanup:
    free(raw);
    return ret;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    strlcpy(s_voice_server_url, s_config->voice_server_url, sizeof(s_voice_server_url));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(main_configure_boot_button_gpio());
    ESP_ERROR_CHECK(main_configure_voice_ptt_gpio());
    s_force_provision_this_boot = main_take_force_provision_request();
    ESP_ERROR_CHECK(app_claw_ui_start());
    if (app_status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "Status LED initialization failed; continuing without LED status");
    }
    ESP_ERROR_CHECK(app_fs_init());

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    log_wifi_startup_config(s_config);
    if (s_force_provision_this_boot) {
        ESP_LOGW(TAG, "Force provisioning is active for this boot; saved STA credentials remain unchanged");
    }

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_force_provision_this_boot ? "" : s_config->wifi_ssid,
        .sta_password = s_force_provision_this_boot ? "" : s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (!s_force_provision_this_boot && s_config->wifi_ssid[0] != '\0') {
            esp_err_t wait_err = wifi_manager_wait_connected(30000);
            if (wait_err == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else if (wait_err == ESP_FAIL) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGW(TAG,
                         "Wi-Fi STA failed after retries: mode=%s ap_active=%d ap_ip=%s",
                         status.mode ? status.mode : "off",
                         status.ap_active,
                         status.ap_ip ? status.ap_ip : "0.0.0.0");
            } else if (wait_err == ESP_ERR_TIMEOUT) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGW(TAG,
                         "Wi-Fi STA wait timeout: mode=%s ap_active=%d sta_configured=%d",
                         status.mode ? status.mode : "off",
                         status.ap_active,
                         status.sta_configured);
            } else {
                ESP_LOGW(TAG, "Wi-Fi STA wait returned error: %s", esp_err_to_name(wait_err));
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        app_status_led_set_provisioning(status.ap_active && !status.sta_connected);
        http_server_log_admin_auth_hint();
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    ESP_ERROR_CHECK(app_claw_start(s_claw_config));
    main_start_boot_button_monitor();
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
