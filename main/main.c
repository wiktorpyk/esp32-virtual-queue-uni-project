#include <stdio.h>
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"
#include "math.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/udp.h"
#include "esp_mac.h"
#include "esp_wifi_types.h"

#define LED_GPIO_PIN  8
#define TAG "BLINK"
#define BOOT_BUTTON_GPIO GPIO_NUM_9


#define MAX_QUEUE_USERS 50
static uint8_t user_mac_queue[MAX_QUEUE_USERS][6];
static int mac_queue_count = 0;
static int mac_queue_next_idx = 0;
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
        case 5:
        default: *r = v; *g = p; *b = q; break;
    }
}

void wifi_init_softap(void)
{
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

// Simple captive portal HTML page
static const char *CAPTIVE_PORTAL_HTML = "<!DOCTYPE html><html><head><title>Captive Portal</title><script>\
window.onload = function() { \
    function pollServer() { \
        fetch('/poll').then(response => response.text()).then(data => { \
            if (data === 'PROCEED') { \
                window.location.href = '/proceed'; \
            } else if (data === 'IN_QUEUE') { \
                /* Optionally, redirect to /queue or update status on page */ \
                /* For now, if already on main page and told 'IN_QUEUE', we might just wait */ \
                /* If we want to force redirect to /queue always when 'IN_QUEUE': */ \
                /* if (window.location.pathname !== '/queue') window.location.href = '/queue'; */ \
                /* For simplicity, let's assume /queue is a distinct page user might navigate to if they get IN_QUEUE */ \
                /* and are not already there. The current setup redirects to /queue from / if 'IN_QUEUE' is first response. */ \
                /* If they are on '/' and get 'IN_QUEUE', they will be redirected by the first condition. */ \
                /* If they are on '/queue' and get 'IN_QUEUE', they stay. */ \
                /* If they are on '/' and get 'PROCEED', they go to '/proceed'. */ \
                /* Let's refine: if 'IN_QUEUE' and not on '/queue', go to '/queue' */ \
                if (window.location.pathname !== '/queue' && window.location.pathname !== '/proceed') { \
                     /* If we are on the main page and told we are in queue, go to queue page */ \
                     /* This logic might need refinement based on desired UX */ \
                     /* A simpler approach: if 'IN_QUEUE', always try to go to /queue if not already there */ \
                     /* window.location.href = '/queue'; */ \
                     /* For now, let's stick to the original: if 'IN_QUEUE', redirect to /queue. */ \
                     /* The user will land on /queue. Subsequent polls from /queue that return 'IN_QUEUE' will do nothing. */ \
                     /* If a poll from /queue returns 'PROCEED', they will go to /proceed. */ \
                     window.location.href = '/queue'; \
                } \
                setTimeout(pollServer, 2000); \
            } else { \
                setTimeout(pollServer, 2000); \
            } \
        }).catch(error => { \
            console.error('Error polling:', error); \
            setTimeout(pollServer, 5000); \
        }); \
    } \
    pollServer(); \
};</script></head><body><h1>Welcome to the ESP32 Captive Portal</h1><form action='/join_queue' method='get'><button type='submit'>Join Queue</button></form></body></html>";


// HTML for the queue page
static const char *QUEUE_PAGE_HTML = "<!DOCTYPE html><html><head><title>Queue</title><script>\
window.onload = function() { \
    function pollServer() { \
        fetch('/poll').then(response => response.text()).then(data => { \
            if (data === 'PROCEED') { \
                window.location.href = '/proceed'; \
            } else if (data === 'IN_QUEUE') { \
                /* Already on queue page, just continue polling */ \
                setTimeout(pollServer, 2000); \
            } else { \
                /* Not in queue or other status, maybe redirect to main or just poll */ \
                /* For now, if on /queue and not told PROCEED or IN_QUEUE, keep polling */ \
                setTimeout(pollServer, 2000); \
            } \
        }).catch(error => { \
            console.error('Error polling from /queue:', error); \
            setTimeout(pollServer, 5000); \
        }); \
    } \
    pollServer(); \
};</script></head><body><h1>You are on the Queue Page</h1><p>Waiting for your turn...</p></body></html>";

// HTML for the proceed page
static const char *PROCEED_PAGE_HTML = "<!DOCTYPE html><html><head><title>Proceed</title><script>\
window.onload = function() { \
    var audioCtx = new (window.AudioContext || window.webkitAudioContext)(); \
    function beep(frequency, duration, volume, type, callback) { \
        var oscillator = audioCtx.createOscillator(); \
        var gainNode = audioCtx.createGain(); \
        oscillator.connect(gainNode); \
        gainNode.connect(audioCtx.destination); \
        if (volume !== undefined){gainNode.gain.value = volume;} \
        if (frequency !== undefined){oscillator.frequency.value = frequency;} \
        if (type !== undefined){oscillator.type = type;} \
        oscillator.start(); \
        if (duration !== undefined){setTimeout(function(){oscillator.stop();}, duration);} \
        if (callback){oscillator.onended = callback;} \
    }; \
    /* Function to play the beep sequence and loop */ \
    function playBeepLoop() { \
        beep(880, 500, 0.1, 'sine', function() { \
            /* After the beep ends, wait 300ms then play again */ \
            setTimeout(playBeepLoop, 300); \
        }); \
    } \
    playBeepLoop(); /* Start the beeping loop */ \
};</script></head><body><h1>It's your turn!</h1><p>You can now proceed.</p></body></html>";


// HTTP server handler: respond with captive portal page to all requests
esp_err_t captive_portal_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "captive_portal_get_handler invoked for URI: %s", req->uri);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CAPTIVE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for Android captive portal check: redirect to captive portal page
esp_err_t android_captive_portal_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "android_captive_portal_handler invoked for URI: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Helper function to check if MAC is in queue and get its index
static int get_mac_queue_index(const uint8_t *mac) {
    for (int i = 0; i < mac_queue_count; i++) {
        if (memcmp(user_mac_queue[i], mac, 6) == 0) {
            return i;
        }
    }
    return -1; // Not found
}

// Handler for joining the queue
esp_err_t join_queue_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "join_queue_handler invoked");
    uint8_t client_mac[6] = {0};
    bool client_mac_obtained = false;

    wifi_sta_list_t sta_list;
    memset(&sta_list, 0, sizeof(sta_list));

    esp_err_t ret_sta_list = esp_wifi_ap_get_sta_list(&sta_list);
    if (ret_sta_list == ESP_OK && sta_list.num > 0) {
        memcpy(client_mac, sta_list.sta[0].mac, 6); // Take the first station's MAC
        client_mac_obtained = true;
        ESP_LOGI(TAG, "Obtained client MAC from esp_wifi_ap_get_sta_list: %02x:%02x:%02x:%02x:%02x:%02x",
                 client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5]);
    } else {
            ESP_LOGW(TAG, "Could not obtain client MAC from station list. List error: %s or no stations connected.", esp_err_to_name(ret_sta_list));
    }

    if (client_mac_obtained) {
        if (mac_queue_count < MAX_QUEUE_USERS) {
            if (get_mac_queue_index(client_mac) == -1) { // Use helper
                memcpy(user_mac_queue[mac_queue_count], client_mac, 6);
                mac_queue_count++;
                ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x added to queue. Queue size: %d",
                         client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5], mac_queue_count);
            } else {
                ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x already in queue.",
                         client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5]);
            }
        } else {
            ESP_LOGW(TAG, "Queue is full. Cannot add MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5]);
        }
    } else {
        ESP_LOGE(TAG, "Failed to get client MAC address for queue.");
        uint8_t ap_mac[6];
        esp_err_t ret_ap_mac = esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
        if (ret_ap_mac == ESP_OK) {
            ESP_LOGI(TAG, "AP MAC Address (not client): %02x:%02x:%02x:%02x:%02x:%02x",
                     ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4], ap_mac[5]);
        }
    }

    // Redirect back to the main page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t poll_handler(httpd_req_t *req) {
    uint8_t current_client_mac[6] = {0};
    bool client_mac_obtained = false;

    wifi_sta_list_t sta_list;
    memset(&sta_list, 0, sizeof(sta_list));
    esp_err_t ret_sta_list = esp_wifi_ap_get_sta_list(&sta_list);

    if (ret_sta_list == ESP_OK && sta_list.num > 0) {
        // Simplification: assume the first station in the list is the one polling.
        // A more robust solution would involve session management or passing client identifier.
        memcpy(current_client_mac, sta_list.sta[0].mac, 6);
        client_mac_obtained = true;
        ESP_LOGV(TAG, "Poll: Assumed client MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 current_client_mac[0], current_client_mac[1], current_client_mac[2],
                 current_client_mac[3], current_client_mac[4], current_client_mac[5]);
    } else {
        ESP_LOGW(TAG, "Poll: Could not obtain station list or no stations connected. Error: %s", esp_err_to_name(ret_sta_list));
        // Cannot determine client, send empty to avoid incorrect state changes
        httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain");

    if (boot_button_pressed_flag && mac_queue_next_idx < mac_queue_count) {
        if (client_mac_obtained && memcmp(current_client_mac, user_mac_queue[mac_queue_next_idx], 6) == 0) {
            ESP_LOGI(TAG, "Poll: Button pressed for user %02x:%02x:%02x:%02x:%02x:%02x (idx %d). Sending PROCEED.",
                     user_mac_queue[mac_queue_next_idx][0], user_mac_queue[mac_queue_next_idx][1],
                     user_mac_queue[mac_queue_next_idx][2], user_mac_queue[mac_queue_next_idx][3],
                     user_mac_queue[mac_queue_next_idx][4], user_mac_queue[mac_queue_next_idx][5],
                     mac_queue_next_idx);
            httpd_resp_send(req, "PROCEED", HTTPD_RESP_USE_STRLEN);
            mac_queue_next_idx++;
            boot_button_pressed_flag = false; // Reset flag after processing one user
        } else {
            // Button pressed, but this client is not the one at the head of the queue
            // or client MAC couldn't be obtained for this specific poll.
            // Check if this client is in queue at all.
            int client_idx = get_mac_queue_index(current_client_mac);
            if (client_idx != -1) {
                 ESP_LOGI(TAG, "Poll: Button pressed, but client %02x:%02x:%02x:%02x:%02x:%02x is not current. Sending IN_QUEUE.",
                     current_client_mac[0], current_client_mac[1], current_client_mac[2],
                     current_client_mac[3], current_client_mac[4], current_client_mac[5]);
                httpd_resp_send(req, "IN_QUEUE", HTTPD_RESP_USE_STRLEN);
            } else {
                 ESP_LOGI(TAG, "Poll: Button pressed, but client %02x:%02x:%02x:%02x:%02x:%02x not in queue. Sending empty.",
                     current_client_mac[0], current_client_mac[1], current_client_mac[2],
                     current_client_mac[3], current_client_mac[4], current_client_mac[5]);
                httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
            }
        }
    } else { // Button not pressed or queue already fully processed by button presses
        int client_idx = get_mac_queue_index(current_client_mac);
        if (client_idx != -1) { // Client is in the queue
             if (client_idx == mac_queue_next_idx && mac_queue_next_idx < mac_queue_count) {
                 ESP_LOGI(TAG, "Poll: Client %02x:%02x:%02x:%02x:%02x:%02x is at head of queue (idx %d), waiting for button. Sending IN_QUEUE.",
                     current_client_mac[0], current_client_mac[1], current_client_mac[2],
                     current_client_mac[3], current_client_mac[4], current_client_mac[5], client_idx);
             } else {
                 ESP_LOGI(TAG, "Poll: Client %02x:%02x:%02x:%02x:%02x:%02x is in queue (idx %d), not at head or head processed. Sending IN_QUEUE.",
                     current_client_mac[0], current_client_mac[1], current_client_mac[2],
                     current_client_mac[3], current_client_mac[4], current_client_mac[5], client_idx);
             }
            httpd_resp_send(req, "IN_QUEUE", HTTPD_RESP_USE_STRLEN);
        } else { // Client not in queue
            ESP_LOGV(TAG, "Poll: Client %02x:%02x:%02x:%02x:%02x:%02x not in queue or queue empty/processed. Sending empty.",
                     current_client_mac[0], current_client_mac[1], current_client_mac[2],
                     current_client_mac[3], current_client_mac[4], current_client_mac[5]);
            httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
        }
    }
    return ESP_OK;
}

// Handler for the queue page
esp_err_t queue_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "queue_handler invoked");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, QUEUE_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for the proceed page
esp_err_t proceed_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "proceed_handler invoked");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROCEED_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


// Start HTTP server for captive portal
void start_captive_portal_httpd(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
        // Handler for Android captive portal detection
        httpd_uri_t uri_generate_204 = {
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = android_captive_portal_handler,
            .user_ctx = NULL
        };
        esp_err_t ret = httpd_register_uri_handler(server, &uri_generate_204);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered URI handler for /generate_204");
        } else {
            ESP_LOGE(TAG, "Failed to register URI handler for /generate_204: %s", esp_err_to_name(ret));
        }

        // Handler for joining queue
        httpd_uri_t uri_join_queue = {
            .uri      = "/join_queue",
            .method   = HTTP_GET,
            .handler  = join_queue_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server, &uri_join_queue);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered URI handler for /join_queue");
        } else {
            ESP_LOGE(TAG, "Failed to register URI handler for /join_queue: %s", esp_err_to_name(ret));
        }

        // Handler for polling
        httpd_uri_t uri_poll = {
            .uri      = "/poll",
            .method   = HTTP_GET,
            .handler  = poll_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server, &uri_poll);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered URI handler for /poll");
        } else {
            ESP_LOGE(TAG, "Failed to register URI handler for /poll: %s", esp_err_to_name(ret));
        }

        // Handler for queue page
        httpd_uri_t uri_queue = {
            .uri      = "/queue",
            .method   = HTTP_GET,
            .handler  = queue_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server, &uri_queue);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered URI handler for /queue");
        } else {
            ESP_LOGE(TAG, "Failed to register URI handler for /queue: %s", esp_err_to_name(ret));
        }

        // Handler for proceed page
        httpd_uri_t uri_proceed = {
            .uri      = "/proceed",
            .method   = HTTP_GET,
            .handler  = proceed_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server, &uri_proceed);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered URI handler for /proceed");
        } else {
            ESP_LOGE(TAG, "Failed to register URI handler for /proceed: %s", esp_err_to_name(ret));
        }

        // Explicit handler for the root path "/"
        httpd_uri_t uri_root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = captive_portal_get_handler, // Use the same handler
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server, &uri_root);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered URI handler for /");
        } else {
            ESP_LOGE(TAG, "Failed to register URI handler for /: %s", esp_err_to_name(ret));
        }

        // Handler for all other requests (captive portal page)
        httpd_uri_t uri_wildcard = { // Renamed from 'uri' to 'uri_wildcard'
            .uri = "/*",
            .method = HTTP_GET,
            .handler = captive_portal_get_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server, &uri_wildcard); // Use renamed variable
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered URI handler for /*");
        } else {
            ESP_LOGE(TAG, "Failed to register URI handler for /*: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

// Simple DNS server task: hijack all DNS queries and reply with ESP32 AP IP
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
            // DNS response: copy query, set response flags, answer with AP IP
            buf[2] |= 0x80; // QR=1 (response)
            buf[3] |= 0x80; // RA=1
            buf[7] = 1;     // ANCOUNT = 1
            int qlen = len;
            // Append answer: name pointer, type A, class IN, TTL, RDLENGTH, RDATA
            uint8_t answer[16] = {0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 192,168,4,1};
            memcpy(buf + qlen, answer, 16);
            sendto(sock, buf, qlen + 16, 0, (struct sockaddr *)&client_addr, addr_len);
        }
    }
}

void button_poll_task(void *pvParameter) {
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY); // ESP32 boot button is active low

    bool last_button_state = true; // Assume button is initially not pressed (high)
    int press_count = 0;

    ESP_LOGI(TAG, "Button poll task started, monitoring GPIO %d", BOOT_BUTTON_GPIO);

    while (1) {
        bool current_button_state = gpio_get_level(BOOT_BUTTON_GPIO);
        if (last_button_state == true && current_button_state == false) { // Falling edge
            // Debounce: wait a bit and check again
            vTaskDelay(20 / portTICK_PERIOD_MS); // 20ms debounce delay
            current_button_state = gpio_get_level(BOOT_BUTTON_GPIO);
            if (current_button_state == false) {
                 press_count++;
                ESP_LOGI(TAG, "Boot button pressed! Count: %d. Setting flag.", press_count);
                boot_button_pressed_flag = true;
                 // Wait for button release to avoid multiple triggers from one long press
                while(gpio_get_level(BOOT_BUTTON_GPIO) == false) {
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                }
                ESP_LOGI(TAG, "Button released.");
                last_button_state = true; // Reset for next press detection
                continue; // Skip updating last_button_state at the end of the loop for this iteration
            }
        }
        last_button_state = current_button_state;
        vTaskDelay(50 / portTICK_PERIOD_MS); // Poll every 50ms
    }
}

void app_main(void) {
    // Initialize NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_softap();

    // Start captive portal HTTP server
    start_captive_portal_httpd();

    // Start DNS hijack task
    xTaskCreate(captive_portal_dns_task, "dns_task", 4096, NULL, 5, NULL);

    // Start button polling task
    xTaskCreate(button_poll_task, "button_task", 2048, NULL, 10, NULL);


    // Configure WS2812B LED    
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO_PIN,
        .max_leds = 1
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    uint16_t hue = 0;
    while (1) {
        uint8_t r, g, b;
        int button_pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        if (button_pressed) {
            r = 255;
            g = 0;
            b = 0;
        } else {
            hsv_to_rgb(hue, 255, 255, &r, &g, &b);
            hue = (hue + 10) % 360;
        }
        led_strip_set_pixel(led_strip, 0, r, g, b);
        led_strip_refresh(led_strip);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}