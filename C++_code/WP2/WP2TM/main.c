#include <stdio.h>
#include <stdlib.h>   // ADDED: needed for free() in the publish block
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"   // CHANGED: legacy driver/i2c.h is deprecated on IDF v5.2+
#include "driver/gpio.h"
#include "esp_log.h"
#include "mqtt_client.h"   // ADDED: this node previously published nothing at all
#include "cJSON.h"         // ADDED
#include "esp_eth.h"       // ADDED: W5500 Ethernet driver (same pattern as other WP2.x nodes)
#include "esp_netif.h"     // ADDED
#include "esp_event.h"     // ADDED
#include "esp_mac.h"       // ADDED
#include "driver/spi_master.h"

static const char *TAG = "WP2_TM_PID";

// ==========================================
// MQTT Configuration (ADDED -- this node had none)
// ==========================================
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883"
#define MQTT_PUBLISH_TOPIC "bus/env/wp2tm"

esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_mqtt_connected = false;

// ==========================================
// Ethernet (W5500) Pin Definitions (SPI3 / FSPI)
// Same LilyGO T-ETH-Lite ESP32-S3 pinout used on the other WP2.x nodes.
// ==========================================
#define ETH_SPI_HOST      SPI3_HOST
#define ETH_PIN_SCLK      10
#define ETH_PIN_MISO      11
#define ETH_PIN_MOSI      12
#define ETH_PIN_CS        9
#define ETH_PIN_INT       13
#define ETH_PIN_RST       14
#define ETH_SPI_CLOCK_MHZ 20
#define ETH_PHY_ADDR      1

static esp_eth_handle_t eth_handle = NULL;
static volatile bool is_eth_connected = false;

// ==========================================
// I2C Pin Definitions & Addresses
// ==========================================
#define I2C_MASTER_SCL_IO           40
#define I2C_MASTER_SDA_IO           39
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          400000 // Fast Mode for high-frequency polling

// Peripheral Addresses
#define PCA9685_ADDR                0x40   // Default I2C address for PCA9685
#define ADS1115_ADDR_GND            0x48
#define ADS1115_ADDR_VDD            0x49
#define ADS1115_ADDR_SCL            0x4B
#define ADS1115_ADDR_SDA            0x4A

// ==========================================
// Thermal & PID Constants
// ==========================================
#define TARGET_SKIN_TEMP            34.0f
#define NTC_NOMINAL_RESISTANCE      10000.0f
#define NTC_NOMINAL_TEMP_K          298.15f // 25 C
#define NTC_BETA_VALUE              3950.0f // Standard 10k NTC Beta (Adjust per datasheet)
#define SERIES_RESISTOR             10000.0f

#define PID_KP                      50.0f   // Proportional gain
#define PID_KI                      0.5f    // Integral gain
#define PID_KD                      10.0f   // Derivative gain
#define PWM_MAX_RESOLUTION          4095    // 12-bit PWM

// ==========================================
// Control Structures
// ==========================================
typedef struct {
    const char* zone_name;
    uint8_t pwm_channel;    // PCA9685 Output (0-15)
    uint8_t adc_i2c_addr;   // ADS1115 Hardware Address
    uint8_t adc_channel;    // ADS1115 Input (0-3)

    // ADDED: matches the flat "internal_wp2tm" schema key names exactly,
    // so publishing is a straight lookup rather than a second mapping table.
    const char* schema_temp_key;
    const char* schema_status_key;

    float current_temp;
    float target_temp;

    // PID States
    float integral_sum;
    float previous_error;
    uint16_t current_pwm_output;
    bool fault_active; // ADDED: tracked so it can be published, not just logged
} ThermalZone_t;

// Mapping the 16 independent zones. Order matches the schema's
// internal_wp2tm key order exactly (verified against WP2_payload_schema.json).
ThermalZone_t dummy_zones[16] = {
    {"Head",            0, ADS1115_ADDR_GND, 0, "temp_head",            "on_status_pad_head",            0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Chest Left",      1, ADS1115_ADDR_GND, 1, "temp_chest_left",      "on_status_pad_chest_left",       0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Chest Right",     2, ADS1115_ADDR_GND, 2, "temp_chest_right",     "on_status_pad_chest_right",      0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Abdomen",         3, ADS1115_ADDR_GND, 3, "temp_abdomen",         "on_status_pad_abdomen",          0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Left Thigh",      4, ADS1115_ADDR_VDD, 0, "temp_tight_left",      "on_status_pad_tight_left",       0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Right Thigh",     5, ADS1115_ADDR_VDD, 1, "temp_tight_right",     "on_status_pad_tight_right",      0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Left Upper Arm",  6, ADS1115_ADDR_VDD, 2, "temp_upper_left_arm",  "on_status_pad_upper_left_arm",   0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Right Upper Arm", 7, ADS1115_ADDR_VDD, 3, "temp_upper_right_arm", "on_status_pad_upper_right_arm",  0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Left Lower Arm",  8, ADS1115_ADDR_SCL, 0, "temp_lower_left_arm",  "on_status_pad_lower_left_arm",   0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Right Lower Arm", 9, ADS1115_ADDR_SCL, 1, "temp_lower_right_arm", "on_status_pad_lower_right_arm",  0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Left Hand",       10, ADS1115_ADDR_SCL, 2, "temp_left_hand",      "on_status_pad_left_hand",        0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Right Hand",      11, ADS1115_ADDR_SCL, 3, "temp_right_hand",     "on_status_pad_right_hand",       0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Left Lower Leg",  12, ADS1115_ADDR_SDA, 0, "temp_lower_left_leg", "on_status_pad_lower_left_leg",   0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Right Lower Leg", 13, ADS1115_ADDR_SDA, 1, "temp_lower_right_leg","on_status_pad_lower_right_leg",  0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Left Foot",       14, ADS1115_ADDR_SDA, 2, "temp_left_foot",      "on_status_pad_left_foot",        0, TARGET_SKIN_TEMP, 0, 0, 0, false},
    {"Right Foot",      15, ADS1115_ADDR_SDA, 3, "temp_right_foot",     "on_status_pad_right_foot",       0, TARGET_SKIN_TEMP, 0, 0, 0, false}
};

static i2c_master_bus_handle_t i2c_bus_handle;
static i2c_master_dev_handle_t pca9685_dev;
static i2c_master_dev_handle_t ads_gnd_dev;
static i2c_master_dev_handle_t ads_vdd_dev;
static i2c_master_dev_handle_t ads_scl_dev;
static i2c_master_dev_handle_t ads_sda_dev;

// ==========================================
// Ethernet Event Handlers (same pattern as other WP2.x nodes)
// ==========================================
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet Link Down");
            is_eth_connected = false;
            break;
        default:
            break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    is_eth_connected = true;
}

// ==========================================
// Ethernet Initialization (W5500 over SPI3) -- non-blocking.
// This node's PID control loop MUST NOT depend on network state -- a
// heater safety loop stalling because a cable is unplugged would be far
// worse than a sensor node doing the same. This only affects publish.
// ==========================================
void init_ethernet(void) {
    ESP_LOGI(TAG, "Initializing W5500 Ethernet...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    spi_bus_config_t eth_buscfg = {
        .miso_io_num = ETH_PIN_MISO,
        .mosi_io_num = ETH_PIN_MOSI,
        .sclk_io_num = ETH_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &eth_buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t eth_spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = ETH_PIN_CS,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &eth_spi_devcfg);
    w5500_config.int_gpio_num = ETH_PIN_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PIN_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    uint8_t eth_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(eth_mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Ethernet init complete. Link negotiation continues in background.");
}

// ==========================================
// MQTT Event Handler (ADDED)
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
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
// I2C & Hardware Initialization
// ==========================================
void init_i2c_bus(void) {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_handle));

    i2c_device_config_t pca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9685_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &pca_cfg, &pca9685_dev));

    i2c_device_config_t ads_gnd_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADS1115_ADDR_GND, .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_gnd_cfg, &ads_gnd_dev));

    i2c_device_config_t ads_vdd_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADS1115_ADDR_VDD, .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_vdd_cfg, &ads_vdd_dev));

    i2c_device_config_t ads_scl_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADS1115_ADDR_SCL, .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_scl_cfg, &ads_scl_dev));

    i2c_device_config_t ads_sda_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADS1115_ADDR_SDA, .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_sda_cfg, &ads_sda_dev));
}

// Small helper: resolve a zone's address enum to its device handle.
i2c_master_dev_handle_t resolve_ads_dev(uint8_t i2c_addr) {
    if (i2c_addr == ADS1115_ADDR_GND) return ads_gnd_dev;
    if (i2c_addr == ADS1115_ADDR_VDD) return ads_vdd_dev;
    if (i2c_addr == ADS1115_ADDR_SCL) return ads_scl_dev;
    return ads_sda_dev; // ADS1115_ADDR_SDA
}

void init_pca9685(void) {
    uint8_t mode1_cmd[2] = {0x00, 0x00};
    i2c_master_transmit(pca9685_dev, mode1_cmd, 2, pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t sleep_cmd[2] = {0x00, 0x10};
    i2c_master_transmit(pca9685_dev, sleep_cmd, 2, pdMS_TO_TICKS(50));

    uint8_t prescale_cmd[2] = {0xFE, 0x3D}; // 100Hz Prescaler value
    i2c_master_transmit(pca9685_dev, prescale_cmd, 2, pdMS_TO_TICKS(50));

    uint8_t wake_cmd[2] = {0x00, 0x00};
    i2c_master_transmit(pca9685_dev, wake_cmd, 2, pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t restart_cmd[2] = {0x00, 0xA0};
    i2c_master_transmit(pca9685_dev, restart_cmd, 2, pdMS_TO_TICKS(50));
}

// ==========================================
// Low Level Hardware Functions
// ==========================================
void set_pca9685_pwm(uint8_t channel, uint16_t on_val, uint16_t off_val) {
    uint8_t base_reg = 0x06 + (4 * channel);
    uint8_t data[5] = {
        base_reg,
        (uint8_t)(on_val & 0xFF),
        (uint8_t)(on_val >> 8),
        (uint8_t)(off_val & 0xFF),
        (uint8_t)(off_val >> 8)
    };
    i2c_master_transmit(pca9685_dev, data, 5, pdMS_TO_TICKS(50));
}

float read_ads1115_ntc_temperature(uint8_t i2c_addr, uint8_t channel) {
    i2c_master_dev_handle_t dev = resolve_ads_dev(i2c_addr);

    uint16_t config = 0x8183;
    if (channel == 0) config = 0xC183;
    if (channel == 1) config = 0xD183;
    if (channel == 2) config = 0xE183;
    if (channel == 3) config = 0xF183;

    uint8_t write_buf[3] = {0x01, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF)};
    if (i2c_master_transmit(dev, write_buf, 3, pdMS_TO_TICKS(50)) != ESP_OK) {
        return -99.0f; // I2C fault -- same sentinel as a bad reading
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // ADC Conversion time

    uint8_t read_cmd = 0x00;
    uint8_t read_buf[2] = {0};
    if (i2c_master_transmit_receive(dev, &read_cmd, 1, read_buf, 2, pdMS_TO_TICKS(50)) != ESP_OK) {
        return -99.0f;
    }

    int16_t raw_adc = (read_buf[0] << 8) | read_buf[1];
    float voltage = raw_adc * 0.0001875f;

    if (voltage <= 0.0f || voltage >= 3.3f) return -99.0f; // Fault detection

    float ntc_resistance = SERIES_RESISTOR * ((3.3f / voltage) - 1.0f);

    float temp_k = ntc_resistance / NTC_NOMINAL_RESISTANCE;
    temp_k = log(temp_k);
    temp_k /= NTC_BETA_VALUE;
    temp_k += 1.0f / NTC_NOMINAL_TEMP_K;
    temp_k = 1.0f / temp_k;

    return temp_k - 273.15f;
}

// ==========================================
// PID Control Loop
// ==========================================
void calculate_pid_and_update(ThermalZone_t *zone) {
    float error = zone->target_temp - zone->current_temp;

    float p_out = PID_KP * error;

    zone->integral_sum += (error * PID_KI);
    if (zone->integral_sum > PWM_MAX_RESOLUTION)
        zone->integral_sum = PWM_MAX_RESOLUTION;
    if (zone->integral_sum < 0)
        zone->integral_sum = 0; // Heating only, no active cooling

    float derivative = error - zone->previous_error;
    float d_out = PID_KD * derivative;

    float total_output = p_out + zone->integral_sum + d_out;

    if (total_output > PWM_MAX_RESOLUTION)
        total_output = PWM_MAX_RESOLUTION;
    if (total_output < 0)
        total_output = 0;

    zone->current_pwm_output = (uint16_t)total_output;
    zone->previous_error = error;

    set_pca9685_pwm(zone->pwm_channel, 0, zone->current_pwm_output);
}

// ==========================================
// Main Application Loop
// ==========================================
void app_main(void) {
    ESP_LOGI(TAG, "Starting WP2.TM (Thermal Dummy PID Controller)...");

    init_ethernet();   // Non-blocking -- PID loop below never waits on this
    init_i2c_bus();
    init_pca9685();
    init_mqtt();

    // Ensure all heaters are completely OFF at startup for safety
    for (int i = 0; i < 16; i++) {
        set_pca9685_pwm(i, 0, 0);
    }

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000);

    while (1) {
        ESP_LOGI(TAG, "--- PID Control Cycle Start ---");

        for (int i = 0; i < 16; i++) {
            ThermalZone_t *zone = &dummy_zones[i];

            zone->current_temp = read_ads1115_ntc_temperature(zone->adc_i2c_addr, zone->adc_channel);

            if (zone->current_temp < -20.0f || zone->current_temp > 80.0f) {
                // Logged locally regardless of MQTT state, matching the
                // pattern used on every other WP2.x node.
                ESP_LOGE(TAG, "FAULT detected in zone %s. Shutting down heater.", zone->zone_name);
                zone->integral_sum = 0;
                zone->fault_active = true;
                set_pca9685_pwm(zone->pwm_channel, 0, 0);
                continue;
            }

            zone->fault_active = false;
            calculate_pid_and_update(zone);

            ESP_LOGD(TAG, "Zone: %-15s | Temp: %5.2f C | Target: %5.2f C | PWM: %4d",
                     zone->zone_name, zone->current_temp, zone->target_temp, zone->current_pwm_output);
        }

        // ADDED: publish the internal_wp2tm block. Field names/order match
        // WP2_payload_schema.json's internal_wp2tm exactly.
        if (is_mqtt_connected) {
            cJSON *root = cJSON_CreateObject();
            cJSON *internal = cJSON_AddObjectToObject(root, "internal_wp2tm");

            for (int i = 0; i < 16; i++) {
                ThermalZone_t *zone = &dummy_zones[i];
                cJSON_AddNumberToObject(internal, zone->schema_temp_key, zone->current_temp);
                // on_status reflects whether the heater is actively being
                // driven right now (PWM > 0), not just "not faulted".
                cJSON_AddBoolToObject(internal, zone->schema_status_key,
                                       (!zone->fault_active) && (zone->current_pwm_output > 0));
            }

            cJSON *diag = cJSON_AddObjectToObject(root, "diagnostics");
            cJSON_AddBoolToObject(diag, "mqtt_connected", true);
            cJSON *faults = cJSON_AddArrayToObject(diag, "sensor_faults");
            char fault_msg[64];
            for (int i = 0; i < 16; i++) {
                if (dummy_zones[i].fault_active) {
                    snprintf(fault_msg, sizeof(fault_msg), "%s_NTC_FAULT", dummy_zones[i].zone_name);
                    cJSON_AddItemToArray(faults, cJSON_CreateString(fault_msg));
                }
            }

            char *json_str = cJSON_PrintUnformatted(root);
            esp_mqtt_client_publish(mqtt_client, MQTT_PUBLISH_TOPIC, json_str, 0, 1, 0);

            cJSON_Delete(root);
            free(json_str);
        } else {
            ESP_LOGW(TAG, "MQTT not connected. PID loop still running locally; publish skipped.");
        }

        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}
