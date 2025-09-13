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
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
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

// Server Configuration (from Kconfig)
#define SERVER_IP CONFIG_DOOR_SERVER_IP
#define SERVER_PORT CONFIG_DOOR_SERVER_PORT

// ntfy.sh Configuration (from Kconfig)
#define NTFY_URL CONFIG_DOOR_NTFY_URL
#define NTFY_PRIORITY CONFIG_DOOR_NTFY_PRIORITY_VALUE

// WiFi Event Group
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Message Queue Configuration
#define MAX_QUEUED_MESSAGES 50
#define MESSAGE_QUEUE_SIZE 256

// Event Batching Configuration
#define BATCH_TIMEOUT_MS 60000  // 60 seconds
#define MAX_EVENT_BUFFER 10

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
 * Send door status to server via TCP with acknowledgment
 */
bool send_door_status_with_ack(const char* message) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char response[64] = {0};

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ESP_LOGE(TAG, "Socket creation error");
        return false;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        ESP_LOGE(TAG, "Invalid address/ Address not supported");
        close(sock);
        return false;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        ESP_LOGE(TAG, "Connection Failed");
        close(sock);
        return false;
    }

    // Send message
    send(sock, message, strlen(message), 0);
    ESP_LOGI(TAG, "Message sent: %s", message);

    // Wait for acknowledgment
    int bytes_received = recv(sock, response, sizeof(response) - 1, 0);
    close(sock);

    if (bytes_received > 0) {
        response[bytes_received] = '\0';
        if (strstr(response, "ACK") != NULL) {
            ESP_LOGI(TAG, "Server acknowledged message");
            return true;
        }
    }
    
    ESP_LOGW(TAG, "No acknowledgment received from server");
    return false;
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
 * Create notification message from event(s)
 */
void create_notification_message(char* message, size_t max_len, door_event_t* events, int count) {
    if (count == 1) {
        // Single event
        const char* status = (events[0].state == DOOR_OPEN) ? "opened" : "closed";
        struct tm* timeinfo = localtime(&events[0].timestamp);
        snprintf(message, max_len, 
                "🚪 Door %s at %02d:%02d", 
                status, timeinfo->tm_hour, timeinfo->tm_min);
    } else if (count == 2 && events[0].state == DOOR_OPEN && events[1].state == DOOR_CLOSED) {
        // Valid pair: OPEN -> CLOSE
        struct tm* open_time = localtime(&events[0].timestamp);
        struct tm* close_time = localtime(&events[1].timestamp);
        int duration_min = (int)((events[1].timestamp - events[0].timestamp) / 60);
        
        snprintf(message, max_len,
                "🚪 Door opened & closed (%02d:%02d-%02d:%02d) - %d min duration",
                open_time->tm_hour, open_time->tm_min,
                close_time->tm_hour, close_time->tm_min,
                duration_min);
    } else {
        // Complex pattern - fallback to count
        snprintf(message, max_len, "⚠️ Door activity: %d events detected", count);
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
            create_notification_message(message, sizeof(message), pair, 2);
            queue_message_direct(message);
            
            processed += 2;  // Skip both events in the pair
        } else {
            // Single event
            char message[256];
            create_notification_message(message, sizeof(message), &event_buffer[processed], 1);
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
            create_notification_message(message, sizeof(message), pair, 2);
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
 * Send door status to server via TCP (legacy function for immediate send)
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
    // Store main task handle for notifications
    main_task_handle = xTaskGetCurrentTaskHandle();
    
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
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    
    // Initialize and sync time via NTP
    if (wifi_connected) {
        initialize_sntp();
        wait_for_time_sync();
    } else {
        ESP_LOGW(TAG, "WiFi not connected - time sync skipped");
    }
    
    // Print startup message
    ESP_LOGI(TAG, "Door monitoring system with NTP sync and event batching started. Monitoring GPIO %d.", REED_SWITCH_PIN);
    
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