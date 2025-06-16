#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"
#include "math.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define LED_GPIO_PIN  8
#define TAG "BLINK"
#define BOOT_BUTTON_GPIO GPIO_NUM_9

#define MAX_QUEUE_USERS 99
static int queue_numbers[MAX_QUEUE_USERS];
static int queue_count = 0;
static int queue_next_idx = 0;
static int next_number = 1;
static volatile bool boot_button_pressed_flag = false;

static led_strip_handle_t led_strip;

void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    float hh = h / 60.0f;
    int i = (int)hh;
    float ff = hh - i;
    float p = v * (1.0f - s / 255.0f);
    float q = v * (1.0f - (s / 255.0f) * ff);
    float t = v * (1.0f - (s / 255.0f) * (1.0f - ff));
    switch (i) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

void wifi_init_softap(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32-AP",
            .ssid_len = 0,
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP started. SSID: %s", ap_config.ap.ssid);
}

static const char *CAPTIVE_PORTAL_HTML =
"<!DOCTYPE html><html><head><title>Captive Portal</title><script>"
"window.onload = function() { "
"function pollServer() { "
"fetch('/poll').then(response => response.text()).then(data => { "
"if (data === 'PROCEED') { window.location.href = '/proceed'; } "
"else if (data === 'IN_QUEUE') { if (window.location.pathname !== '/queue' && window.location.pathname !== '/proceed') { window.location.href = '/queue'; } setTimeout(pollServer, 2000); } "
"else { setTimeout(pollServer, 2000); } "
"}).catch(error => { setTimeout(pollServer, 5000); }); "
"}; pollServer(); };"
"</script></head><body><h1>Welcome to the ESP32 Captive Portal</h1><form action='/join_queue' method='get'><button type='submit'>Join Queue</button></form></body></html>";

static const char *QUEUE_PAGE_HTML_PREFIX =
"<!DOCTYPE html><html><head><title>Queue</title><script>"
"window.onload = function() { "
"function pollServer() { "
"fetch('/poll').then(response => response.text()).then(data => { "
"if (data === 'PROCEED') { window.location.href = '/proceed'; } "
"else { setTimeout(pollServer, 2000); } "
"}).catch(error => { setTimeout(pollServer, 5000); }); "
"}; pollServer(); };"
"</script></head><body><h1>You are on the Queue Page</h1><p>Your number: <b>";

static const char *QUEUE_PAGE_HTML_SUFFIX =
"</b></p><p>Waiting for your turn...</p></body></html>";

static const char *PROCEED_PAGE_HTML =
"<!DOCTYPE html><html><head><title>Proceed</title><script>"
"window.onload = function() { "
"var audioCtx = new (window.AudioContext || window.webkitAudioContext)(); "
"function beep(frequency, duration, volume, type, callback) { "
"var oscillator = audioCtx.createOscillator(); "
"var gainNode = audioCtx.createGain(); "
"oscillator.connect(gainNode); "
"gainNode.connect(audioCtx.destination); "
"if (volume !== undefined){gainNode.gain.value = volume;} "
"if (frequency !== undefined){oscillator.frequency.value = frequency;} "
"if (type !== undefined){oscillator.type = type;} "
"oscillator.start(); "
"if (duration !== undefined){setTimeout(function(){oscillator.stop();}, duration);} "
"if (callback){oscillator.onended = callback;} "
"}; "
"function playBeepLoop() { "
"beep(880, 500, 0.1, 'sine', function() { setTimeout(playBeepLoop, 300); }); "
"} "
"playBeepLoop(); "
"};"
"</script></head><body><h1>It's your turn!</h1><p>You can now proceed.</p></body></html>";

esp_err_t captive_portal_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CAPTIVE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t android_captive_portal_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static int get_queue_index(int number) {
    for (int i = 0; i < queue_count; i++) {
        if (queue_numbers[i] == number) return i;
    }
    return -1;
}

static int get_number_from_cookie(httpd_req_t *req) {
    char cookie[128] = {0};
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len > 0 && cookie_len < sizeof(cookie)) {
        httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie));
        char *num_ptr = strstr(cookie, "queue_number=");
        if (num_ptr) {
            int num = atoi(num_ptr + strlen("queue_number="));
            if (num >= 1 && num <= 99) return num;
        }
    }
    return 0;
}

esp_err_t join_queue_handler(httpd_req_t *req) {
    int client_number = get_number_from_cookie(req);
    if (client_number == 0 || get_queue_index(client_number) == -1) {
        if (queue_count < MAX_QUEUE_USERS) {
            int assigned = next_number;
            queue_numbers[queue_count++] = assigned;
            next_number = (next_number % 99) + 1;
            char set_cookie[32];
            snprintf(set_cookie, sizeof(set_cookie), "queue_number=%02d; Path=/", assigned);
            httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
        }
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t poll_handler(httpd_req_t *req) {
    int client_number = get_number_from_cookie(req);
    httpd_resp_set_type(req, "text/plain");
    if (client_number == 0) {
        httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int idx = get_queue_index(client_number);
    if (idx == -1) {
        httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (boot_button_pressed_flag && queue_next_idx < queue_count) {
        if (queue_numbers[queue_next_idx] == client_number) {
            httpd_resp_send(req, "PROCEED", HTTPD_RESP_USE_STRLEN);
            queue_next_idx++;
            boot_button_pressed_flag = false;
        } else {
            httpd_resp_send(req, "IN_QUEUE", HTTPD_RESP_USE_STRLEN);
        }
    } else {
        httpd_resp_send(req, "IN_QUEUE", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

esp_err_t queue_handler(httpd_req_t *req) {
    int client_number = get_number_from_cookie(req);
    char html[1024];
    if (client_number > 0) {
        snprintf(html, sizeof(html), "%s%02d%s", QUEUE_PAGE_HTML_PREFIX, client_number, QUEUE_PAGE_HTML_SUFFIX);
    } else {
        snprintf(html, sizeof(html), "<!DOCTYPE html><html><body><h1>No queue number assigned.</h1></body></html>");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t proceed_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROCEED_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void start_captive_portal_httpd(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_generate_204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = android_captive_portal_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_generate_204);

        httpd_uri_t uri_join_queue = { .uri = "/join_queue", .method = HTTP_GET, .handler = join_queue_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_join_queue);

        httpd_uri_t uri_poll = { .uri = "/poll", .method = HTTP_GET, .handler = poll_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_poll);

        httpd_uri_t uri_queue = { .uri = "/queue", .method = HTTP_GET, .handler = queue_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_queue);

        httpd_uri_t uri_proceed = { .uri = "/proceed", .method = HTTP_GET, .handler = proceed_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_proceed);

        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = captive_portal_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_wildcard = { .uri = "/*", .method = HTTP_GET, .handler = captive_portal_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_wildcard);
    }
}

void captive_portal_dns_task(void *pvParameters) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr, client_addr;
    uint8_t buf[512];
    socklen_t addr_len = sizeof(client_addr);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len > 0) {
            buf[2] |= 0x80;
            buf[3] |= 0x80;
            buf[7] = 1;
            int qlen = len;
            uint8_t answer[16] = {0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 192,168,4,1};
            memcpy(buf + qlen, answer, 16);
            sendto(sock, buf, qlen + 16, 0, (struct sockaddr *)&client_addr, addr_len);
        }
    }
}

void button_poll_task(void *pvParameter) {
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);

    bool last_button_state = true;
    while (1) {
        bool current_button_state = gpio_get_level(BOOT_BUTTON_GPIO);
        if (last_button_state && !current_button_state) {
            vTaskDelay(20 / portTICK_PERIOD_MS);
            current_button_state = gpio_get_level(BOOT_BUTTON_GPIO);
            if (!current_button_state) {
                boot_button_pressed_flag = true;
                while(gpio_get_level(BOOT_BUTTON_GPIO) == false) {
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                }
                last_button_state = true;
                continue;
            }
        }
        last_button_state = current_button_state;
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_softap();
    start_captive_portal_httpd();
    xTaskCreate(captive_portal_dns_task, "dns_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_poll_task, "button_task", 2048, NULL, 10, NULL);

    led_strip_config_t strip_config = { .strip_gpio_num = LED_GPIO_PIN, .max_leds = 1 };
    led_strip_rmt_config_t rmt_config = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    uint16_t hue = 0;
    while (1) {
        uint8_t r, g, b;
        int button_pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        if (button_pressed) {
            r = 255; g = 0; b = 0;
        } else {
            hsv_to_rgb(hue, 255, 255, &r, &g, &b);
            hue = (hue + 10) % 360;
        }
        led_strip_set_pixel(led_strip, 0, r, g, b);
        led_strip_refresh(led_strip);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}