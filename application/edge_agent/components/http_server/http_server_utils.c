/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "http_server_auth";

#define ADMIN_BASIC_MAX_LEN 192
#define ADMIN_FALLBACK_PASSWORD_LEN 64

char *http_server_alloc_scratch_buffer(void)
{
    return heap_caps_malloc_prefer(HTTP_SERVER_SCRATCH_SIZE,
                                   2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool http_server_path_is_safe(const char *path)
{
    return path && path[0] == '/' && strstr(path, "..") == NULL;
}

void http_server_url_decode_inplace(char *value)
{
    if (!value) {
        return;
    }

    char *src = value;
    char *dst = value;
    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hi = src[1];
            char lo = src[2];
            uint8_t decoded = 0;

            if (hi >= '0' && hi <= '9') {
                decoded = (uint8_t)(hi - '0') << 4;
            } else if (hi >= 'A' && hi <= 'F') {
                decoded = (uint8_t)(hi - 'A' + 10) << 4;
            } else if (hi >= 'a' && hi <= 'f') {
                decoded = (uint8_t)(hi - 'a' + 10) << 4;
            } else {
                *dst++ = *src++;
                continue;
            }

            if (lo >= '0' && lo <= '9') {
                decoded |= (uint8_t)(lo - '0');
            } else if (lo >= 'A' && lo <= 'F') {
                decoded |= (uint8_t)(lo - 'A' + 10);
            } else if (lo >= 'a' && lo <= 'f') {
                decoded |= (uint8_t)(lo - 'a' + 10);
            } else {
                *dst++ = *src++;
                continue;
            }

            *dst++ = (char)decoded;
            src += 3;
            continue;
        }

        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

esp_err_t http_server_query_get(httpd_req_t *req, const char *key, char *value, size_t value_size)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char *query = calloc(1, query_len + 1);
    if (!query) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = httpd_req_get_url_query_str(req, query, query_len + 1);
    if (err == ESP_OK) {
        err = httpd_query_key_value(query, key, value, value_size);
        if (err == ESP_OK) {
            http_server_url_decode_inplace(value);
        }
    }

    free(query);
    return err;
}

esp_err_t http_server_resolve_storage_path(const char *relative_path, char *full_path, size_t full_path_size)
{
    http_server_ctx_t *ctx = http_server_ctx();

    if (!http_server_path_is_safe(relative_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(full_path, full_path_size, "%s%s", ctx->storage_base_path, relative_path);
    return (written <= 0 || (size_t)written >= full_path_size) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

bool http_server_build_child_relative_path(const char *base_path,
                                           const char *entry_name,
                                           char *out_path,
                                           size_t out_path_size)
{
    if (!base_path || !entry_name || !out_path || out_path_size == 0) {
        return false;
    }

    if (strcmp(base_path, "/") == 0) {
        if (strlcpy(out_path, "/", out_path_size) >= out_path_size) {
            return false;
        }
    } else if (strlcpy(out_path, base_path, out_path_size) >= out_path_size) {
        return false;
    }

    if (strcmp(base_path, "/") != 0 && strlcat(out_path, "/", out_path_size) >= out_path_size) {
        return false;
    }

    return strlcat(out_path, entry_name, out_path_size) < out_path_size;
}

static bool base64_encode_basic(const uint8_t *input, size_t input_len, char *out, size_t out_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((input_len + 2) / 3) * 4 + 1;
    if (!input || !out || out_size < needed) {
        if (out && out_size) {
            out[0] = '\0';
        }
        return false;
    }

    size_t i = 0;
    size_t o = 0;
    while (i < input_len) {
        size_t remaining = input_len - i;
        uint32_t octet_a = input[i++];
        uint32_t octet_b = (remaining > 1) ? input[i++] : 0;
        uint32_t octet_c = (remaining > 2) ? input[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[o++] = alphabet[(triple >> 18) & 0x3F];
        out[o++] = alphabet[(triple >> 12) & 0x3F];
        out[o++] = (remaining > 1) ? alphabet[(triple >> 6) & 0x3F] : '=';
        out[o++] = (remaining > 2) ? alphabet[triple & 0x3F] : '=';
    }
    out[o] = '\0';
    return true;
}

static bool admin_auth_is_open_provisioning(void)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    if (!ctx->services.get_wifi_status || ctx->services.get_wifi_status(&status) != ESP_OK) {
        return false;
    }
    return status.ap_active && !status.wifi_connected;
}

static void admin_fallback_password(char *out, size_t out_size)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    const char *suffix = NULL;

    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (ctx->services.get_wifi_status && ctx->services.get_wifi_status(&status) == ESP_OK &&
        status.ap_ssid && status.ap_ssid[0]) {
        suffix = strrchr(status.ap_ssid, '-');
        suffix = suffix ? suffix + 1 : status.ap_ssid;
    }
    if (!suffix || !suffix[0]) {
        suffix = "setup";
    }

    snprintf(out, out_size, "esp-claw-%s", suffix);
}

static bool admin_load_credentials(char *user,
                                   size_t user_size,
                                   char *password,
                                   size_t password_size,
                                   bool *password_is_fallback)
{
    http_server_ctx_t *ctx = http_server_ctx();
    app_config_t *config = NULL;
    bool loaded = false;

    if (user && user_size) {
        strlcpy(user, "admin", user_size);
    }
    if (password && password_size) {
        admin_fallback_password(password, password_size);
    }
    if (password_is_fallback) {
        *password_is_fallback = true;
    }

    if (!ctx->services.load_config) {
        return false;
    }

    config = calloc(1, sizeof(*config));
    if (!config) {
        return false;
    }

    if (ctx->services.load_config(config) == ESP_OK) {
        loaded = true;
        if (user && user_size && config->admin_username[0]) {
            strlcpy(user, config->admin_username, user_size);
        }
        if (password && password_size && config->admin_password[0]) {
            strlcpy(password, config->admin_password, password_size);
            if (password_is_fallback) {
                *password_is_fallback = false;
            }
        }
    }

    free(config);
    return loaded;
}

static esp_err_t send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-Claw Admin\", charset=\"UTF-8\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_server_require_admin(httpd_req_t *req)
{
    if (!req || admin_auth_is_open_provisioning()) {
        return ESP_OK;
    }

    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len == 0 || auth_len >= ADMIN_BASIC_MAX_LEN) {
        send_auth_required(req);
        return ESP_FAIL;
    }

    char auth[ADMIN_BASIC_MAX_LEN] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
        strncmp(auth, "Basic ", 6) != 0) {
        send_auth_required(req);
        return ESP_FAIL;
    }

    char user[sizeof(((app_config_t *)0)->admin_username)] = {0};
    char password[ADMIN_FALLBACK_PASSWORD_LEN] = {0};
    char pair[sizeof(user) + sizeof(password) + 2] = {0};
    char encoded[ADMIN_BASIC_MAX_LEN] = {0};

    admin_load_credentials(user, sizeof(user), password, sizeof(password), NULL);
    snprintf(pair, sizeof(pair), "%s:%s", user, password);

    if (!base64_encode_basic((const uint8_t *)pair, strlen(pair), encoded, sizeof(encoded)) ||
        strcmp(auth + 6, encoded) != 0) {
        send_auth_required(req);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void http_server_log_admin_auth_hint(void)
{
    char user[sizeof(((app_config_t *)0)->admin_username)] = {0};
    char password[ADMIN_FALLBACK_PASSWORD_LEN] = {0};
    bool fallback_password = true;

    admin_load_credentials(user, sizeof(user), password, sizeof(password), &fallback_password);

    if (admin_auth_is_open_provisioning()) {
        ESP_LOGW(TAG, "Web Admin is open while provisioning AP is active");
    } else if (fallback_password) {
        ESP_LOGW(TAG, "STA Web Admin Basic Auth: username=%s generated_password=%s", user, password);
    } else {
        ESP_LOGI(TAG, "STA Web Admin Basic Auth: username=%s password=<configured>", user);
    }
}
