#include <stdio.h>
#include <stdlib.h>   // ADDED: needed for free() in the publish block
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"   // CHANGED: legacy driver/i2c.h is deprecated on IDF v5.2+
#include "driver/gpio.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_eth.h"      // ADDED: W5500 Ethernet driver (same pattern as other WP2.x nodes)
#include "esp_netif.h"    // ADDED: network interface layer (netif/LWIP)
#include "esp_event.h"    // ADDED: default event loop (needed by netif + MQTT)
#include "esp_mac.h"      // ADDED: esp_read_mac()/ESP_MAC_ETH
#include "driver/spi_master.h"

static const char *TAG = "WP2_3_i_NODE";

// ==========================================
// MQTT Configuration
// ==========================================
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883"
// Depending in each case/location, a publish topic must be enabled
#define MQTT_PUBLISH_TOPIC "bus/env/wp231_front"
//#define MQTT_PUBLISH_TOPIC "bus/env/wp232_middle"
//#define MQTT_PUBLISH_TOPIC "bus/env/wp233_rear"

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
// Hardware Pin Definitions
// ==========================================
#define I2C_MASTER_SCL_IO           40
#define I2C_MASTER_SDA_IO           39
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000

#define PIN_DOOR_SWITCH             15      // Optocoupler PC817 output
// No conflict with the Ethernet pins above -- this GPIO15 is local to this
// node's own board and separate from WP2.1.i's SPI2 sensor bus, which lives
// on a different physical ESP32.

// I2C Device Addresses
#define ADS1115_ADDR                0x48    // ADDR pin connected to GND

// Calibration Constants & OOB Bounds
#define VOLTAGE_DIVIDER_RATIO       1.6666f // Compensates for 10k/15k divider
#define ADS1115_VOLTS_PER_BIT       0.0001875f // +/- 6.144V FSR mode

#define GLOBE_TEMP_MIN_BOUND       -20.0f
#define GLOBE_TEMP_MAX_BOUND        70.0f
#define HIH4000_RH_MIN_BOUND        0.01f
#define HIH4000_RH_MAX_BOUND        99.9f

static i2c_master_bus_handle_t i2c_bus_handle;
static i2c_master_dev_handle_t ads1115_dev;

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
// Without this, MQTT init crashes with "Invalid mbox" because no
// netif/event loop/tcpip task exists yet for it to attach to.
// Sensor reads must NOT depend on link/IP being up (see app_main loop).
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
// MQTT Event Handler
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    // FIXED: removed the unused 'event' variable (-Werror=unused-variable)
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

    i2c_device_config_t ads_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_cfg, &ads1115_dev));
}

void init_digital_inputs(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_DOOR_SWITCH),
        .pull_down_en = 0, // Using external 10k pull-down shown in schematic
        .pull_up_en = 0
    };
    gpio_config(&io_conf);
}

// ==========================================
// Sensor Read Functions
// ==========================================
float read_ads1115_voltage(uint8_t mux_channel, uint8_t *hw_fault) {
    uint16_t config = 0x8183;
    if (mux_channel == 0) config = 0xC183; // AIN0 to GND (TP-RS-BB)
    if (mux_channel == 1) config = 0xD183; // AIN1 to GND (HIH-4000)

    uint8_t write_buf[3] = {0x01, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF)};
    esp_err_t err = i2c_master_transmit(ads1115_dev, write_buf, 3, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        *hw_fault = 1;
        return 0.0f;
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // Conversion delay

    uint8_t read_cmd = 0x00;
    uint8_t read_buf[2] = {0};
    err = i2c_master_transmit_receive(ads1115_dev, &read_cmd, 1, read_buf, 2, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        *hw_fault = 1;
        return 0.0f;
    }

    int16_t raw_adc = (read_buf[0] << 8) | read_buf[1];
    return raw_adc * ADS1115_VOLTS_PER_BIT;
}

// ==========================================
// Main Application Loop
// ==========================================
void app_main(void) {
    ESP_LOGI(TAG, "Starting WP2.3.i Node (Globe Temp, RH & Door State)...");

    init_ethernet();   // Must run first: brings up netif/event loop that MQTT needs
    init_i2c_bus();
    init_digital_inputs();
    init_mqtt();

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000); // 1 Hz Sync

    while (1) {
        ESP_LOGI(TAG, "--- Sensor Reading Cycle Start ---");

        uint8_t ads_fault = 0;

        // 1. Read Analog Sensors via ADS1115

        // TP-RS-BB (Globe Temp) on AIN0
        float v_tprs = read_ads1115_voltage(0, &ads_fault);
        v_tprs *= VOLTAGE_DIVIDER_RATIO;
        float globe_temp_c = v_tprs * 10.0f; // Placeholder linear conversion

        // HIH-4000 (RH) on AIN1
        float v_hih4000 = read_ads1115_voltage(1, &ads_fault);
        v_hih4000 *= VOLTAGE_DIVIDER_RATIO;
        float rh_percent = (v_hih4000 - 0.8f) / 0.031f;

        // Hard limits for RH physical reality
        if (rh_percent < 0.0f) rh_percent = 0.0f;
        if (rh_percent > 100.0f) rh_percent = 100.0f;

        // 2. Read Digital Door State (MC-31B via PC817)
        int door_pin_level = gpio_get_level(PIN_DOOR_SWITCH);
        bool open_door_state = (door_pin_level == 0);

        // 3. Evaluate faults + log locally, REGARDLESS of MQTT connectivity
        // (same pattern established on WP2.1.i / WP2.2.i so nothing is
        // silently lost if the node is temporarily offline).
        bool rh_oob = (rh_percent <= HIH4000_RH_MIN_BOUND || rh_percent >= HIH4000_RH_MAX_BOUND);
        bool globe_oob = (globe_temp_c <= GLOBE_TEMP_MIN_BOUND || globe_temp_c >= GLOBE_TEMP_MAX_BOUND);

        if (ads_fault) ESP_LOGW(TAG, "ADS1115 I2C fault/timeout");
        if (rh_oob) ESP_LOGW(TAG, "HIH4000 RH out of bounds: %.1f %%", rh_percent);
        if (globe_oob) ESP_LOGW(TAG, "TP-RS-BB globe temp out of bounds: %.1f C", globe_temp_c);

        // 4. Generate and Publish JSON Payload
        if (is_mqtt_connected) {
            cJSON *root = cJSON_CreateObject();

            cJSON_AddNumberToObject(root, "b1_RH_percent", rh_percent);
            cJSON_AddNumberToObject(root, "d1_globe_temp_C", globe_temp_c);
            cJSON_AddBoolToObject(root, "b2_open_door_state", open_door_state);

            // Diagnostics & Watchdog
            cJSON *diag = cJSON_AddObjectToObject(root, "diagnostics");
            cJSON_AddBoolToObject(diag, "mqtt_connected", true);
            cJSON *faults = cJSON_AddArrayToObject(diag, "sensor_faults");

            if (ads_fault) cJSON_AddItemToArray(faults, cJSON_CreateString("ADS1115_I2C_TIMEOUT"));
            if (rh_oob) cJSON_AddItemToArray(faults, cJSON_CreateString("HIH4000_OUT_OF_BOUNDS"));
            if (globe_oob) cJSON_AddItemToArray(faults, cJSON_CreateString("TPRSBB_OUT_OF_BOUNDS"));

            char *json_str = cJSON_PrintUnformatted(root);
            esp_mqtt_client_publish(mqtt_client, MQTT_PUBLISH_TOPIC, json_str, 0, 1, 0);
            ESP_LOGD(TAG, "Published: %s", json_str);

            cJSON_Delete(root);
            free(json_str);
        } else {
            ESP_LOGW(TAG, "MQTT not connected. Sensors still read locally; publish skipped.");
        }

        // 5. Wait exact remaining time to maintain 1 Hz
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}
