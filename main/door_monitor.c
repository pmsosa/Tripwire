#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include <time.h>
#include <sys/time.h>

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

// Bluetooth Configuration (from Kconfig)
#define PHONE_BT_MAC CONFIG_DOOR_PHONE_BT_MAC

// ntfy.sh Configuration (from Kconfig)
#define NTFY_URL CONFIG_DOOR_NTFY_URL
#define NTFY_PRIORITY CONFIG_DOOR_NTFY_PRIORITY_VALUE

// WiFi Event Group
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Message Queue Configuration
#define MAX_QUEUED_MESSAGES 20
#define MESSAGE_QUEUE_SIZE 128

// Event Batching Configuration
#define BATCH_TIMEOUT_MS 60000  // 60 seconds
#define MAX_EVENT_BUFFER 5

// NTP Configuration
#define NTP_SERVER "pool.ntp.org"
#define TIMEZONE "PST8PDT,M3.2.0/2,M11.1.0"  // Pacific Time - change as needed

// Logging tag
static const char* TAG = "DOOR_SENSOR";

// Door event structure
typedef struct {
    int state;          // DOOR_OPEN or DOOR_CLOSED
    time_t timestamp;
    bool processed;
} door_event_t;

// Message queue structure
typedef struct {
    char message[MESSAGE_QUEUE_SIZE];
    time_t timestamp;
} door_message_t;

// Global variables
static int current_door_state = -1;  // Initialize to invalid state to force initial detection
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool wifi_connected = false;
static door_message_t message_queue[MAX_QUEUED_MESSAGES];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

// Event batching variables
static door_event_t event_buffer[MAX_EVENT_BUFFER];
static int event_count = 0;
static TimerHandle_t batch_timer = NULL;
static bool batch_timer_active = false;

// NTP variables
static bool time_synced = false;

// Bluetooth SPP variables
static bool bt_initialized = false;
static bool spp_connected = false;
static esp_bd_addr_t phone_mac_addr;
static uint32_t spp_handle = 0;
#define SPP_CONNECTION_TIMEOUT_MS 10000  // 10 seconds as requested

// Task notification for batch processing
static TaskHandle_t main_task_handle = NULL;
#define BATCH_TIMEOUT_NOTIFICATION (1UL << 0)

// Forward declarations
void queue_message_direct(const char* message);
void process_accumulated_events(void);
void batch_timer_callback(TimerHandle_t xTimer);
void initialize_sntp(void);
void wait_for_time_sync(void);
void sync_time_on_wake(void);
bool send_ntfy_notification(const char* message);
void format_time_12h(struct tm* timeinfo, char* buffer, size_t size);
void init_bluetooth_spp(void);
bool try_connect_to_phone(void);
void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
void parse_mac_address(const char* mac_str, esp_bd_addr_t mac_addr);

/**
 * Function to blink the LED a specified number of times
 * @param blink_count Number of times to blink the LED
 */
static void blink_led(int blink_count) {
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
        wifi_connected = false;
        ESP_LOGI(TAG, "WiFi disconnected - will retry connection");
        esp_wifi_connect();
        s_retry_num++;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Sync time if this is a reconnection (SNTP already initialized)
        if (esp_sntp_enabled()) {
            sync_time_on_wake();
        }
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
 * SNTP sync notification callback
 */
void sntp_sync_time_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Time synchronized with NTP server");
    time_synced = true;
}

/**
 * Initialize SNTP and sync time
 */
void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    
    // Set timezone
    setenv("TZ", TIMEZONE, 1);
    tzset();
    
    // Initialize SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb);
    esp_sntp_init();
    
    ESP_LOGI(TAG, "SNTP initialized, waiting for time sync...");
}

/**
 * Wait for time synchronization with timeout
 */
void wait_for_time_sync(void) {
    int retry = 0;
    const int retry_count = 30; // 30 seconds timeout
    
    while (!time_synced && retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for time sync... (%d/%d)", retry + 1, retry_count);
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    
    if (time_synced) {
        time_t now;
        time(&now);
        struct tm *timeinfo = localtime(&now);
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%c", timeinfo);
        ESP_LOGI(TAG, "Time synced successfully: %s", strftime_buf);
    } else {
        ESP_LOGW(TAG, "Time sync timeout - continuing with system time");
    }
}

/**
 * Sync time on demand (for wake from deep sleep)
 */
void sync_time_on_wake(void) {
    if (!wifi_connected) {
        ESP_LOGW(TAG, "Cannot sync time - WiFi not connected");
        return;
    }
    
    ESP_LOGI(TAG, "Syncing time after wake...");
    time_synced = false;
    
    // Reset SNTP to force immediate sync
    esp_sntp_stop();
    esp_sntp_init();
    
    // Wait for sync with shorter timeout for battery efficiency
    int retry = 0;
    const int retry_count = 10; // 10 seconds timeout
    
    while (!time_synced && retry < retry_count) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    
    if (time_synced) {
        ESP_LOGI(TAG, "Time re-synced successfully");
    } else {
        ESP_LOGW(TAG, "Time re-sync timeout - using previous time");
    }
}

/**
 * HTTP event handler for ntfy.sh requests
 */
esp_err_t ntfy_http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP Error");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "Connected to ntfy.sh");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP headers sent");
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP request finished");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from ntfy.sh");
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * Send notification via ntfy.sh
 */
bool send_ntfy_notification(const char* message) {
    if (!wifi_connected) {
        ESP_LOGW(TAG, "Cannot send ntfy notification - WiFi not connected");
        return false;
    }

    ESP_LOGI(TAG, "Sending ntfy notification: %s", message);
    
    esp_http_client_config_t config = {
        .url = NTFY_URL,
        .event_handler = ntfy_http_event_handler,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,  // 10 second timeout
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use certificate bundle for HTTPS
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }
    
    // Set headers
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    esp_http_client_set_header(client, "Priority", NTFY_PRIORITY);
    esp_http_client_set_header(client, "Title", "Door Monitor");
    esp_http_client_set_header(client, "Tags", "door,security");
    
    // Set the message as POST data
    esp_http_client_set_post_field(client, message, strlen(message));
    
    // Perform the request
    esp_err_t err = esp_http_client_perform(client);
    bool success = false;
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            ESP_LOGI(TAG, "ntfy notification sent successfully");
            success = true;
        } else {
            ESP_LOGW(TAG, "ntfy request failed with status: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "ntfy HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return success;
}

/**
 * Add message to queue
 */
void queue_message(const char* status) {
    if (queue_count >= MAX_QUEUED_MESSAGES) {
        ESP_LOGW(TAG, "Message queue full, dropping oldest message");
        queue_head = (queue_head + 1) % MAX_QUEUED_MESSAGES;
        queue_count--;
    }
    
    time_t now;
    time(&now);
    snprintf(message_queue[queue_tail].message, MESSAGE_QUEUE_SIZE, 
             "{\"STATUS\":\"%s\",\"TIMESTAMP\":%lld}", status, (long long)now);
    message_queue[queue_tail].timestamp = now;
    
    queue_tail = (queue_tail + 1) % MAX_QUEUED_MESSAGES;
    queue_count++;
    
    ESP_LOGI(TAG, "Queued message: %s (Queue size: %d)", message_queue[(queue_tail - 1 + MAX_QUEUED_MESSAGES) % MAX_QUEUED_MESSAGES].message, queue_count);
}

/**
 * Remove message from queue head
 */
void dequeue_message() {
    if (queue_count > 0) {
        queue_head = (queue_head + 1) % MAX_QUEUED_MESSAGES;
        queue_count--;
    }
}


/**
 * Process message queue - send all queued messages via ntfy.sh
 */
void process_message_queue() {
    if (!wifi_connected || queue_count == 0) {
        return;
    }
    
    ESP_LOGI(TAG, "Processing %d queued messages via ntfy.sh", queue_count);
    
    while (queue_count > 0) {
        char* message = message_queue[queue_head].message;
        
        if (send_ntfy_notification(message)) {
            ESP_LOGI(TAG, "Queued notification sent successfully via ntfy.sh");
            dequeue_message();
        } else {
            ESP_LOGW(TAG, "Failed to send queued notification, will retry later");
            break;  // Stop processing if connection fails
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));  // Small delay between messages to avoid rate limiting
    }
}

/**
 * Format time in 12-hour format with AM/PM
 */
void format_time_12h(struct tm* timeinfo, char* buffer, size_t size) {
    int hour = timeinfo->tm_hour;
    const char* ampm = (hour >= 12) ? "PM" : "AM";
    
    if (hour == 0) hour = 12;       // 12 AM
    else if (hour > 12) hour -= 12; // PM hours
    
    snprintf(buffer, size, "%d:%02d %s", hour, timeinfo->tm_min, ampm);
}

/**
 * Parse MAC address string into esp_bd_addr_t
 */
void parse_mac_address(const char* mac_str, esp_bd_addr_t mac_addr) {
    int values[6];
    int result = sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                        &values[0], &values[1], &values[2],
                        &values[3], &values[4], &values[5]);

    if (result == 6) {
        for (int i = 0; i < 6; i++) {
            mac_addr[i] = (uint8_t)values[i];
        }
        ESP_LOGI(TAG, "MAC parsing successful: %d fields parsed", result);
    } else {
        ESP_LOGW(TAG, "MAC parsing failed: only %d fields parsed", result);
        // Set to all zeros on failure
        memset(mac_addr, 0, 6);
    }
}

/**
 * SPP callback function
 */
void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    switch (event) {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI(TAG, "SPP initialized");
            break;
        case ESP_SPP_OPEN_EVT:
            if (param->open.status == ESP_SPP_SUCCESS) {
                ESP_LOGI(TAG, "SPP connection opened successfully - phone authenticated");
                spp_connected = true;
                spp_handle = param->open.handle;
            } else {
                ESP_LOGI(TAG, "SPP connection failed but phone responded: %d - phone authenticated", param->open.status);
                spp_connected = true;  // Any response means phone is present
            }
            break;
        case ESP_SPP_CLOSE_EVT:
            ESP_LOGI(TAG, "SPP connection closed (handle: %d) - phone responded", param->close.handle);
            // Only count as authenticated if we had a real connection attempt
            if (param->close.handle != 0) {
                spp_connected = true;  // Connection attempt got a response, phone is present
            }
            spp_handle = 0;
            break;
        case ESP_SPP_CONG_EVT:
            ESP_LOGD(TAG, "SPP congestion status: %d", param->cong.cong);
            break;
        default:
            break;
    }
}

/**
 * Initialize Bluetooth SPP
 */
void init_bluetooth_spp(void) {
    if (bt_initialized) return;

    ESP_LOGI(TAG, "Initializing Bluetooth SPP for phone authentication");

    // Parse the MAC address from config
    ESP_LOGI(TAG, "Parsing MAC address: '%s'", PHONE_BT_MAC);
    parse_mac_address(PHONE_BT_MAC, phone_mac_addr);
    ESP_LOGI(TAG, "Parsed MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             phone_mac_addr[0], phone_mac_addr[1], phone_mac_addr[2],
             phone_mac_addr[3], phone_mac_addr[4], phone_mac_addr[5]);

    // Check current controller status
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    ESP_LOGI(TAG, "BT controller status: %d", status);

    // Release BLE memory to save RAM (only if controller is in IDLE state)
    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGI(TAG, "Releasing BLE memory...");
        esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "BT controller BLE mem release failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "BLE memory released successfully");
        }
    } else {
        ESP_LOGW(TAG, "Skipping BLE memory release - controller not in IDLE state");
    }

    // Initialize BT controller
    ESP_LOGI(TAG, "Initializing BT controller...");
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    // Try absolute defaults first to isolate the issue
    ESP_LOGI(TAG, "Using default BT controller configuration (no modifications)");
    // bt_cfg.controller_task_stack_size = 3072;  // Commented out for testing
    // bt_cfg.controller_task_prio = 20;          // Commented out for testing

    // Log the configuration parameters for debugging
    ESP_LOGI(TAG, "BT controller config:");
    ESP_LOGI(TAG, "  task_stack_size: %d", bt_cfg.controller_task_stack_size);
    ESP_LOGI(TAG, "  task_prio: %d", bt_cfg.controller_task_prio);
    ESP_LOGI(TAG, "  hci_uart_no: %d", bt_cfg.hci_uart_no);
    ESP_LOGI(TAG, "  hci_uart_baudrate: %d", bt_cfg.hci_uart_baudrate);
    ESP_LOGI(TAG, "  scan_duplicate_mode: %d", bt_cfg.scan_duplicate_mode);
    ESP_LOGI(TAG, "  scan_duplicate_type: %d", bt_cfg.scan_duplicate_type);
    ESP_LOGI(TAG, "  normal_adv_size: %d", bt_cfg.normal_adv_size);
    ESP_LOGI(TAG, "  mesh_adv_size: %d", bt_cfg.mesh_adv_size);
    ESP_LOGI(TAG, "  send_adv_reserved_size: %d", bt_cfg.send_adv_reserved_size);
    ESP_LOGI(TAG, "  controller_debug_flag: %d", bt_cfg.controller_debug_flag);
    ESP_LOGI(TAG, "  mode: %d", bt_cfg.mode);

    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "BT controller initialized successfully");

    ESP_LOGI(TAG, "Enabling BT controller for Classic BT...");
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "BT controller enabled successfully");

    // Initialize Bluedroid stack
    ESP_LOGI(TAG, "Initializing Bluedroid stack...");
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Bluedroid initialized successfully");

    ESP_LOGI(TAG, "Enabling Bluedroid stack...");
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Bluedroid enabled successfully");

    // Initialize SPP (now that Bluedroid is ready)
    ESP_LOGI(TAG, "Registering SPP callback...");
    ret = esp_spp_register_callback(spp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP callback register failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPP callback registered successfully");

    ESP_LOGI(TAG, "Initializing SPP with legacy API...");
    ret = esp_spp_init(ESP_SPP_MODE_CB);  // Use older, stable API instead of enhanced
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPP initialized successfully with legacy API");

    bt_initialized = true;
    ESP_LOGI(TAG, "Bluetooth SPP initialization completed successfully!");
}

/**
 * Try to connect to phone via SPP with 10-second timeout
 */
bool try_connect_to_phone(void) {
    if (!bt_initialized) {
        ESP_LOGW(TAG, "Bluetooth not initialized");
        return false;
    }

    ESP_LOGI(TAG, "Attempting SPP connection to phone MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             phone_mac_addr[0], phone_mac_addr[1], phone_mac_addr[2],
             phone_mac_addr[3], phone_mac_addr[4], phone_mac_addr[5]);
    spp_connected = false;

    // Start SPP connection attempt
    esp_err_t ret = esp_spp_connect(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER, 1, phone_mac_addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPP connect failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "SPP connect initiated, waiting for connection...");

    // Wait for connection with shorter timeout to avoid stack issues
    uint32_t timeout_ms = 3000;  // Reduce to 3 seconds
    uint32_t start_time = esp_timer_get_time() / 1000;

    while (!spp_connected && ((esp_timer_get_time() / 1000) - start_time) < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));  // Longer delay, fewer iterations
    }

    if (spp_connected) {
        ESP_LOGI(TAG, "Phone authenticated - device responded to connection attempt");
        // Try to disconnect if we have a handle, but don't wait
        if (spp_handle != 0) {
            esp_spp_disconnect(spp_handle);
        }
        return true;
    } else {
        ESP_LOGW(TAG, "Phone authentication timeout - device not found after %lu ms", timeout_ms);
        return false;
    }
}

/**
 * Create notification message from event(s)
 */
void create_notification_message(char* message, size_t max_len, door_event_t* events, int count, bool authenticated) {
    const char* auth_status = authenticated ? "" : " ⚠️ (Unauthenticated)";

    if (count == 1) {
        // Single event - use exclamation emoji for open doors (security concern)
        struct tm* timeinfo = localtime(&events[0].timestamp);
        char time_str[16];
        format_time_12h(timeinfo, time_str, sizeof(time_str));

        if (events[0].state == DOOR_OPEN) {
            snprintf(message, max_len, "❗ Door opened at %s%s", time_str, auth_status);
        } else {
            snprintf(message, max_len, "🚪 Door closed at %s%s", time_str, auth_status);
        }
    } else if (count == 2 && events[0].state == DOOR_OPEN && events[1].state == DOOR_CLOSED) {
        // Valid pair: OPEN -> CLOSE - simplified format
        struct tm* open_time = localtime(&events[0].timestamp);
        char time_str[16];
        format_time_12h(open_time, time_str, sizeof(time_str));

        snprintf(message, max_len, "🚪 Door Open/Close (%s)%s", time_str, auth_status);
    } else {
        // Complex pattern - fallback to count
        snprintf(message, max_len, "⚠️ Door activity: %d events detected%s", count, auth_status);
    }
}

/**
 * Process and send accumulated events
 */
void process_accumulated_events() {
    if (event_count == 0) return;
    
    ESP_LOGI(TAG, "Processing %d accumulated events", event_count);
    
    int processed = 0;
    while (processed < event_count) {
        // Look for OPEN->CLOSE pairs
        if (processed + 1 < event_count && 
            event_buffer[processed].state == DOOR_OPEN && 
            event_buffer[processed + 1].state == DOOR_CLOSED) {
            
            // Found a pair
            char message[256];
            door_event_t pair[2] = {event_buffer[processed], event_buffer[processed + 1]};
            bool authenticated = try_connect_to_phone();
            create_notification_message(message, sizeof(message), pair, 2, authenticated);
            queue_message_direct(message);
            
            processed += 2;  // Skip both events in the pair
        } else {
            // Single event
            char message[256];
            bool authenticated = try_connect_to_phone();
            create_notification_message(message, sizeof(message), &event_buffer[processed], 1, authenticated);
            queue_message_direct(message);
            
            processed += 1;
        }
    }
    
    // Clear the event buffer
    event_count = 0;
    ESP_LOGI(TAG, "Event buffer cleared");
}

/**
 * Timer callback for batch timeout - lightweight, just notify main task
 */
void batch_timer_callback(TimerHandle_t xTimer) {
    batch_timer_active = false;
    // Notify main task to process events (don't do heavy work in timer callback)
    if (main_task_handle != NULL) {
        xTaskNotify(main_task_handle, BATCH_TIMEOUT_NOTIFICATION, eSetBits);
    }
}

/**
 * Add event to batch buffer
 */
void add_event_to_batch(int door_state, time_t timestamp) {
    if (event_count >= MAX_EVENT_BUFFER) {
        ESP_LOGW(TAG, "Event buffer full, processing immediately");
        process_accumulated_events();
    }
    
    // Add new event
    event_buffer[event_count].state = door_state;
    event_buffer[event_count].timestamp = timestamp;
    event_buffer[event_count].processed = false;
    event_count++;
    
    ESP_LOGI(TAG, "Added event to batch: %s (buffer size: %d)", 
             (door_state == DOOR_OPEN) ? "OPEN" : "CLOSE", event_count);
    
    // Check for immediate pair completion
    if (event_count >= 2) {
        int last = event_count - 1;
        int prev = event_count - 2;
        
        // If we have OPEN->CLOSE pair, process it immediately
        if (event_buffer[prev].state == DOOR_OPEN && 
            event_buffer[last].state == DOOR_CLOSED) {
            
            ESP_LOGI(TAG, "Complete pair detected, processing immediately");
            
            // Create pair message
            char message[256];
            door_event_t pair[2] = {event_buffer[prev], event_buffer[last]};
            bool authenticated = try_connect_to_phone();
            create_notification_message(message, sizeof(message), pair, 2, authenticated);
            queue_message_direct(message);
            
            // Remove the pair from buffer
            event_count -= 2;
            
            // Shift remaining events (if any)
            for (int i = 0; i < event_count; i++) {
                event_buffer[i] = event_buffer[i + 2];
            }
            
            // If buffer is empty, stop timer
            if (event_count == 0 && batch_timer_active) {
                xTimerStop(batch_timer, 0);
                batch_timer_active = false;
                ESP_LOGI(TAG, "Buffer empty, stopping batch timer");
            }
            
            return;  // Don't start/restart timer
        }
    }
    
    // Start or restart timer for remaining events
    if (batch_timer_active) {
        xTimerReset(batch_timer, 0);
        ESP_LOGI(TAG, "Batch timer reset");
    } else {
        xTimerStart(batch_timer, 0);
        batch_timer_active = true;
        ESP_LOGI(TAG, "Batch timer started");
    }
}

/**
 * Direct message sending (try ntfy immediately, queue if failed)
 */
void queue_message_direct(const char* message) {
    // Try to send immediately if WiFi connected
    if (wifi_connected && send_ntfy_notification(message)) {
        ESP_LOGI(TAG, "Notification sent immediately via ntfy.sh");
        return;
    }
    
    // If sending failed or WiFi not connected, queue the message
    if (queue_count >= MAX_QUEUED_MESSAGES) {
        ESP_LOGW(TAG, "Message queue full, dropping oldest message");
        queue_head = (queue_head + 1) % MAX_QUEUED_MESSAGES;
        queue_count--;
    }
    
    time_t now;
    time(&now);
    strncpy(message_queue[queue_tail].message, message, MESSAGE_QUEUE_SIZE - 1);
    message_queue[queue_tail].message[MESSAGE_QUEUE_SIZE - 1] = '\0';
    message_queue[queue_tail].timestamp = now;
    
    queue_tail = (queue_tail + 1) % MAX_QUEUED_MESSAGES;
    queue_count++;
    
    ESP_LOGI(TAG, "Queued notification for retry: %s (Queue size: %d)", message, queue_count);
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
    // Store main task handle for notifications
    main_task_handle = xTaskGetCurrentTaskHandle();

    // Echo configuration values for debugging
    ESP_LOGI(TAG, "=== DOOR MONITOR CONFIGURATION ===");
    ESP_LOGI(TAG, "WiFi SSID: '%s'", WIFI_SSID);
    ESP_LOGI(TAG, "WiFi Password: '%s'", WIFI_PASS);
    ESP_LOGI(TAG, "Phone BT MAC: '%s'", PHONE_BT_MAC);
    ESP_LOGI(TAG, "NTFY URL: '%s'", NTFY_URL);
    ESP_LOGI(TAG, "NTFY Priority: '%s'", NTFY_PRIORITY);
    ESP_LOGI(TAG, "===================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize GPIO pins
    configure_gpio();
    
    // Create batch timer (but don't start it yet)
    batch_timer = xTimerCreate("BatchTimer", 
                              pdMS_TO_TICKS(BATCH_TIMEOUT_MS),
                              pdFALSE,  // One-shot timer
                              (void*)0,
                              batch_timer_callback);
    
    if (batch_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create batch timer");
        return;
    }
    
    // Initialize WiFi
    ESP_LOGI(TAG, "Starting WiFi initialization in STA mode...");
    wifi_init_sta();
    ESP_LOGI(TAG, "WiFi initialization completed");
    
    // Initialize and sync time via NTP
    if (wifi_connected) {
        initialize_sntp();
        wait_for_time_sync();
    } else {
        ESP_LOGW(TAG, "WiFi not connected - time sync skipped");
    }

    // Initialize Bluetooth SPP for phone authentication
    ESP_LOGI(TAG, "Starting Bluetooth SPP initialization...");
    init_bluetooth_spp();

    // Print startup message
    ESP_LOGI(TAG, "Door monitoring system with SPP authentication, NTP sync and event batching started. Monitoring GPIO %d for phone %s", REED_SWITCH_PIN, PHONE_BT_MAC);
    
    // Main monitoring loop
    while (1) {
        // Check for batch timer notification (non-blocking)
        uint32_t notification_value;
        if (xTaskNotifyWait(0, ULONG_MAX, &notification_value, 0) == pdTRUE) {
            if (notification_value & BATCH_TIMEOUT_NOTIFICATION) {
                ESP_LOGI(TAG, "Batch timer expired, processing events");
                process_accumulated_events();
            }
        }
        
        // Process any queued messages if WiFi is connected
        process_message_queue();
        
        // Read the current state of the reed switch
        int door_state = gpio_get_level(REED_SWITCH_PIN);
        
        // Check if the door state has changed
        if (door_state != current_door_state) {
            // Update the current state
            current_door_state = door_state;
            
            // Get current time for event
            time_t now;
            time(&now);
            
            // Perform actions based on door state
            if (door_state == DOOR_OPEN) {
                // Door opened
                ESP_LOGI(TAG, "Door Opened!");
                blink_led(1);  // Blink LED once
                
                // Add to batch processing
                add_event_to_batch(DOOR_OPEN, now);
                
            } else {
                // Door closed
                ESP_LOGI(TAG, "Door Closed!");
                blink_led(2);  // Blink LED twice
                
                // Add to batch processing
                add_event_to_batch(DOOR_CLOSED, now);
            }
        }
        
        // Small delay to be efficient and prevent excessive polling
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms delay
    }
}