#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// Pin definitions
#define REED_SWITCH_PIN GPIO_NUM_23  // Magnetic reed switch connected to GPIO 23
#define LED_PIN GPIO_NUM_2           // Built-in LED connected to GPIO 2

// Door states
#define DOOR_OPEN 1
#define DOOR_CLOSED 0

// Logging tag
static const char* TAG = "DOOR_SENSOR";

// Global variable to track current door state
static int current_door_state = -1;  // Initialize to invalid state to force initial detection

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
    // Initialize GPIO pins
    configure_gpio();
    
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
            } else {
                // Door closed
                ESP_LOGI(TAG, "Door Closed!");
                blink_led(2);  // Blink LED twice
            }
        }
        
        // Small delay to be efficient and prevent excessive polling
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms delay
    }
}