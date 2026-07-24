#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

static const char *TAG = "WP2_4_i_NODE";

// ==========================================
// MQTT Configuration
// ==========================================
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883" 
#define MQTT_PUBLISH_TOPIC "bus/env/wp241"

esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_mqtt_connected = false;

// ==========================================
// Hardware Pin Definitions
// ==========================================
#define TXD_PIN (GPIO_NUM_5) // ESP32 TX -> GPS RX
#define RXD_PIN (GPIO_NUM_4) // ESP32 RX -> GPS TX

#define UART_NUM UART_NUM_1
#define BUF_SIZE (1024)

// ==========================================
// GPS Global Variables
// ==========================================
float current_lat = 0.0f;
float current_lon = 0.0f;
float current_speed_kmh = 0.0f;
int current_satellites = 0;
bool has_valid_fix = false;

// ==========================================
// MQTT Event Handler
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT Connected");
        is_mqtt_connected = true;
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT Disconnected");
        is_mqtt_connected = false;
    }
}

void init_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URI };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ==========================================
// Hardware Initialization
// ==========================================
void init_uart(void) {
    // NEO-6M default baud rate is usually 9600
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// ==========================================
// Basic NMEA Parser Helpers
// ==========================================
// Converts NMEA format (DDMM.MMMM) to Decimal Degrees
float nmea_to_decimal(float nmea_coord, char direction) {
    int degrees = (int)(nmea_coord / 100);
    float minutes = nmea_coord - (degrees * 100);
    float decimal = degrees + (minutes / 60.0f);
    if (direction == 'S' || direction == 'W') decimal = -decimal;
    return decimal;
}

void parse_nmea_sentence(const char *sentence) {
    // Parse $GPGGA for Satellites and Fix Quality
    if (strncmp(sentence, "$GPGGA", 6) == 0) {
        int fix_quality = 0;
        char *p = strchr(sentence, ',');
        for (int i = 0; i < 5 && p != NULL; i++) p = strchr(p + 1, ','); // Skip to fix quality
        if (p != NULL) sscanf(p + 1, "%d", &fix_quality);
        
        has_valid_fix = (fix_quality > 0);

        if (p != NULL) {
            p = strchr(p + 1, ','); // Skip to satellites
            if (p != NULL) sscanf(p + 1, "%d", &current_satellites);
        }
    }
    // Parse $GPRMC for Lat, Lon, and Speed
    else if (strncmp(sentence, "$GPRMC", 6) == 0) {
        char status;
        float raw_lat, raw_lon, raw_speed_knots;
        char ns, ew;
        
        char *p = strchr(sentence, ',');
        for (int i = 0; i < 1 && p != NULL; i++) p = strchr(p + 1, ','); // Skip time
        if (p != NULL) sscanf(p + 1, "%c", &status);

        if (status == 'A') { // 'A' = Active (Valid Fix)
            p = strchr(p + 1, ',');
            if (p != NULL) sscanf(p + 1, "%f,%c,%f,%c,%f", &raw_lat, &ns, &raw_lon, &ew, &raw_speed_knots);

            current_lat = nmea_to_decimal(raw_lat, ns);
            current_lon = nmea_to_decimal(raw_lon, ew);
            
            // Convert knots to km/h (1 knot = 1.852 km/h)
            current_speed_kmh = raw_speed_knots * 1.852f;

            // Speed filter to eliminate GPS drift when stationary
            if (current_speed_kmh < 3.0f) {
                current_speed_kmh = 0.0f;
            }
        }
    }
}

// ==========================================
// Background Task to read UART
// ==========================================
static void rx_task(void *arg) {
    uint8_t* data = (uint8_t*) malloc(BUF_SIZE);
    char line[128];
    int line_len = 0;

    while (1) {
        const int rxBytes = uart_read_bytes(UART_NUM, data, BUF_SIZE, 20 / portTICK_PERIOD_MS);
        if (rxBytes > 0) {
            for (int i = 0; i < rxBytes; i++) {
                if (data[i] == '\n' || line_len >= sizeof(line) - 1) {
                    line[line_len] = '\0';
                    parse_nmea_sentence(line);
                    line_len = 0;
                } else if (data[i] != '\r') {
                    line[line_len++] = (char)data[i];
                }
            }
        }
    }
    free(data);
}

// ==========================================
// Main Application Loop
// ==========================================
void app_main(void) {
    ESP_LOGI(TAG, "Starting WP2.4.i Node (NEO-6M GPS)...");
    
    init_uart();
    init_mqtt();

    // Start UART reading task
    xTaskCreate(rx_task, "uart_rx_task", 1024*2, NULL, configMAX_PRIORITIES - 1, NULL);

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000); // 1 Hz Sync

    while (1) {
        if (is_mqtt_connected) {
            cJSON *root = cJSON_CreateObject();
            
            cJSON_AddNumberToObject(root, "lat", current_lat);
            cJSON_AddNumberToObject(root, "lon", current_lon);
            cJSON_AddNumberToObject(root, "speed_kmh", current_speed_kmh);

            // Add WP2.4.i specific payload
            cJSON_AddNumberToObject(root, "aps", current_satellites);
            
            // Diagnostics Block
            cJSON *diag = cJSON_AddObjectToObject(root, "diagnostics");
            cJSON_AddBoolToObject(diag, "mqtt_connected", true);
            cJSON *faults = cJSON_AddArrayToObject(diag, "sensor_faults");
            
            if (!has_valid_fix) {
                cJSON_AddItemToArray(faults, cJSON_CreateString("GPS_NO_FIX"));
            } else if (current_satellites < 4) {
                cJSON_AddItemToArray(faults, cJSON_CreateString("GPS_POOR_FIX"));
            }

            char *json_str = cJSON_PrintUnformatted(root);
            esp_mqtt_client_publish(mqtt_client, MQTT_PUBLISH_TOPIC, json_str, 0, 1, 0);
            ESP_LOGD(TAG, "Published: %s", json_str);
            
            cJSON_Delete(root);
            free(json_str);
        }

        vTaskDelayUntil(&xLastWakeTime, xFrequency); 
    }
}
