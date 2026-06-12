/* SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "lwip/ip_addr.h"
#include "mdns.h"

/* Built-in fallback WiFi network, configurable via 'idf.py menuconfig'
 * ("USB MSC Device Demo" -> "WiFi Settings"). Used only until a
 * config.txt on the SD card or a previous NVS configuration provides a
 * network to join. Empty (the default) means "no fallback network". */
#define ESP_WIFI_STA_SSID       CONFIG_ESP_WIFI_ROUTE_SSID
#define ESP_WIFI_STA_PASS       CONFIG_ESP_WIFI_ROUTE_PASSWORD
#define ESP_WIFI_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#define WIFI_SSID_LEN           32
#define WIFI_PASSWORD_LEN       64
#define WIFI_DEVNAME_LEN        32

#define DEFAULT_DEVICE_NAME     "bernina-stick"

#define SD_CONFIG_PATH          "/disk/config.txt"
#define SD_CONFIG_LINE_MAX      128

static const char *TAG = "wifi";

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
    }
}

static void wifi_init_sta(char *ssid, char *password)
{
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    snprintf((char *)wifi_config.sta.ssid, WIFI_SSID_LEN, "%s", ssid);
    snprintf((char *)wifi_config.sta.password, WIFI_PASSWORD_LEN, "%s", password);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "wifi_init_sta finished, connecting to SSID:%s", ssid);
}

static void nvs_get_str_log(esp_err_t err, char *key, char *value)
{
    switch (err) {
    case ESP_OK:
        ESP_LOGI(TAG, "%s = %s", key, value);
        break;
    case ESP_ERR_NVS_NOT_FOUND:
        ESP_LOGI(TAG, "%s : Can't find in NVS!", key);
        break;
    default:
        ESP_LOGE(TAG, "Error (%s) reading!", esp_err_to_name(err));
    }
}

static esp_err_t from_nvs_set_value(char *key, char *value)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("memory", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return ESP_FAIL;
    } else {
        err = nvs_set_str(my_handle, key, value);
        ESP_LOGI(TAG, "set %s is %s!,the err is %d\n", key, (err == ESP_OK) ? "succeed" : "failed", err);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "NVS close Done\n");
    }
    return ESP_OK;
}

static esp_err_t from_nvs_get_value(char *key, char *value, size_t *size)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("memory", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return ESP_FAIL;
    } else {
        err = nvs_get_str(my_handle, key, value, size);
        nvs_get_str_log(err, key, value);
        nvs_close(my_handle);
    }
    return err;
}

/* Trim leading/trailing spaces, tabs, CR and LF in place and return a
 * pointer to the first non-whitespace character. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

/* --- Bernina file-based WiFi configuration ---------------------------------
 * On startup (after the SD card has been mounted), look for a "config.txt"
 * file in the root of the SD card. It may contain "DeviceName", "SSID" and
 * "Password" entries, one "Key=Value" per line; keys are case-insensitive,
 * blank lines and lines starting with '#' are ignored. Any value found
 * there overwrites the corresponding setting stored in NVS - even if the
 * device is currently connected with the old settings. The file is always
 * deleted afterwards, so the plaintext WiFi password doesn't linger on the
 * SD card, which is later exposed to the host (e.g. the embroidery machine)
 * as a USB mass-storage device. */
static void apply_sd_config(void)
{
    FILE *f = fopen(SD_CONFIG_PATH, "r");
    if (!f) {
        return;
    }

    ESP_LOGI(TAG, "Found %s, applying WiFi configuration", SD_CONFIG_PATH);

    char line[SD_CONFIG_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (l[0] == '\0' || l[0] == '#') {
            continue;
        }

        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(l);
        char *value = trim(eq + 1);

        if (!strcasecmp(key, "DeviceName")) {
            from_nvs_set_value("devname", value);
            ESP_LOGI(TAG, "config.txt: DeviceName = %s", value);
        } else if (!strcasecmp(key, "SSID")) {
            from_nvs_set_value("stassid", value);
            ESP_LOGI(TAG, "config.txt: SSID = %s", value);
        } else if (!strcasecmp(key, "Password")) {
            from_nvs_set_value("stapasswd", value);
            ESP_LOGI(TAG, "config.txt: Password = (hidden)");
        } else {
            ESP_LOGW(TAG, "config.txt: ignoring unknown key '%s'", key);
        }
    }

    fclose(f);

    if (unlink(SD_CONFIG_PATH) == 0) {
        ESP_LOGI(TAG, "Deleted %s", SD_CONFIG_PATH);
    } else {
        ESP_LOGE(TAG, "Failed to delete %s (%s)", SD_CONFIG_PATH, strerror(errno));
    }
}

void app_wifi_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* The SD card is already mounted at this point - let config.txt (if
     * present) override the stored WiFi settings before we read them. */
    apply_sd_config();

    // Initialize networking stack
    ESP_ERROR_CHECK(esp_netif_init());
    // Create default event loop needed by the  main app
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char str[WIFI_PASSWORD_LEN];
    size_t str_size;
    char sta_ssid[WIFI_SSID_LEN] = "";
    char sta_passwd[WIFI_PASSWORD_LEN] = "";
    char devname[WIFI_DEVNAME_LEN] = DEFAULT_DEVICE_NAME;

    strlcpy(sta_ssid, ESP_WIFI_STA_SSID, sizeof(sta_ssid));
    strlcpy(sta_passwd, ESP_WIFI_STA_PASS, sizeof(sta_passwd));

    str_size = sizeof(str);
    if (from_nvs_get_value("stassid", str, &str_size) == ESP_OK) {
        strlcpy(sta_ssid, str, sizeof(sta_ssid));
    }

    str_size = sizeof(str);
    if (from_nvs_get_value("stapasswd", str, &str_size) == ESP_OK) {
        strlcpy(sta_passwd, str, sizeof(sta_passwd));
    }

    str_size = sizeof(str);
    if (from_nvs_get_value("devname", str, &str_size) == ESP_OK && strlen(str) > 0) {
        strlcpy(devname, str, sizeof(devname));
    }

    if (strlen(sta_ssid) == 0) {
        ESP_LOGW(TAG, "No WiFi SSID configured. WiFi will be off.");
        return;
    }

    esp_netif_t *wifi_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(wifi_sta_netif, devname));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    wifi_init_sta(sta_ssid, sta_passwd);

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Make the device reachable as http://<devname>.local */
    esp_err_t mdns_ret = mdns_init();
    if (mdns_ret == ESP_OK) {
        mdns_hostname_set(devname);
        mdns_instance_name_set(devname);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS started: http://%s.local", devname);
    } else {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(mdns_ret));
    }
}
