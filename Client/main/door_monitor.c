#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

// Pin definitions
#define REED_SWITCH_PIN GPIO_NUM_23  // Magnetic reed switch connected to GPIO 23
#define LED_PIN GPIO_NUM_2           // Built-in LED connected to GPIO 2

// Door states
#define DOOR_OPEN 1
#define DOOR_CLOSED 0

// WiFi Configuration (from Kconfig)
#define WIFI_SSID CONFIG_DOOR_WIFI_SSID
#define WIFI_PASS CONFIG_DOOR_WIFI_PASSWORD
#define WIFI_MAXIMUM_RETRY 5

// Server Configuration (from Kconfig)
#define SERVER_IP CONFIG_DOOR_SERVER_IP
#define SERVER_PORT CONFIG_DOOR_SERVER_PORT

// WiFi Event Group
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Logging tag
static const char* TAG = "DOOR_SENSOR";

// Global variables
static int current_door_state = -1;  // Initialize to invalid state to force initial detection
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/**
 * Function to blink the LED a specified number of times
 * @param blink_count Number of times to blink the LED
 */
void blink_led(int blink_count) {
    for (int i = 0; i < blink_count; i++) {
        gpio_set_level(LED_PIN, 1);  // Turn LED on
        vTaskDelay(pdMS_TO_TICKS(200));  // Wait 200ms
        gpio_set_level(LED_PIN, 0);  // Turn LED off
        vTaskDelay(pdMS_TO_TICKS(200));  // Wait 200ms
    }
}

/**
 * WiFi event handler
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * Initialize WiFi
 */
void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

/**
 * Send door status to server via TCP
 */
void send_door_status(const char* status) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char json_message[100];

    snprintf(json_message, sizeof(json_message), "{\"STATUS\":\"%s\"}", status);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ESP_LOGE(TAG, "Socket creation error");
        return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        ESP_LOGE(TAG, "Invalid address/ Address not supported");
        close(sock);
        return;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        ESP_LOGE(TAG, "Connection Failed");
        close(sock);
        return;
    }

    send(sock, json_message, strlen(json_message), 0);
    ESP_LOGI(TAG, "Message sent: %s", json_message);

    close(sock);
}

/**
 * Function to configure GPIO pins
 */
void configure_gpio(void) {
    // Configure reed switch pin as input with pull-up resistor
    gpio_config_t reed_switch_config = {
        .pin_bit_mask = (1ULL << REED_SWITCH_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&reed_switch_config);

    // Configure LED pin as output
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_config);

    // Initialize LED to off state
    gpio_set_level(LED_PIN, 0);
}

/**
 * Main application entry point
 */
void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize GPIO pins
    configure_gpio();
    
    // Initialize WiFi
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    
    // Print startup message
    ESP_LOGI(TAG, "Door monitoring system started. Monitoring GPIO %d for door state changes.", REED_SWITCH_PIN);
    
    // Main monitoring loop
    while (1) {
        // Read the current state of the reed switch
        int door_state = gpio_get_level(REED_SWITCH_PIN);
        
        // Check if the door state has changed
        if (door_state != current_door_state) {
            // Update the current state
            current_door_state = door_state;
            
            // Perform actions based on door state
            if (door_state == DOOR_OPEN) {
                // Door opened
                ESP_LOGI(TAG, "Door Opened!");
                blink_led(1);  // Blink LED once
                send_door_status("OPENED");  // Send TCP message
            } else {
                // Door closed
                ESP_LOGI(TAG, "Door Closed!");
                blink_led(2);  // Blink LED twice
                send_door_status("CLOSED");  // Send TCP message
            }
        }
        
        // Small delay to be efficient and prevent excessive polling
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms delay
    }
}