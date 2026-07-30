#include <stdio.h>
#include <stdlib.h>   // ADDED: needed for free() in publish_sensor_data()
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_eth.h"      // ADDED: W5500 Ethernet driver
#include "esp_netif.h"    // ADDED: network interface layer (netif/LWIP)
#include "esp_event.h"    // ADDED: default event loop (needed by netif + MQTT)
#include "esp_mac.h"      // ADDED: esp_read_mac()/ESP_MAC_ETH -- no longer pulled in
                          // automatically by esp_system.h as of IDF v5.x
 
static const char *TAG = "WP2_1_i_NODE";
 
// ==========================================
// Out-of-bounds values for Diagnostics
// ==========================================
#define TEMP_MIN_BOUND -20.0f
#define TEMP_MAX_BOUND  60.0f
 
// ==========================================
// FORWARD DECLARATIONS (ADDED)
// These were previously used (in functions below) before being declared,
// causing "implicit declaration" and "conflicting types" build errors.
// Declaring them here, before first use, fixes the compile.
// ==========================================
spi_device_handle_t spi_max31856;
spi_device_handle_t spi_max31865;
 
esp_err_t spi_read_registers(spi_device_handle_t spi, uint8_t reg_addr, uint8_t *data_out, size_t len);
esp_err_t spi_write_register(spi_device_handle_t spi, uint8_t reg_addr, uint8_t data_in);
 
// Helper to check hardware faults on MAX31856 (Register 0x0F)
uint8_t check_thermocouple_hardware_fault(void) {
    uint8_t fault_reg = 0;
    spi_read_registers(spi_max31856, 0x0F, &fault_reg, 1);
    return fault_reg;
    // Returns 0 if OK. Non-zero means Open Circuit, Overvoltage, etc.
}
 
// Helper to check hardware faults on MAX31865 (Register 0x07)
uint8_t check_rtd_hardware_fault(void) {
    uint8_t fault_reg = 0;
    spi_read_registers(spi_max31865, 0x07, &fault_reg, 1);
    return fault_reg;
    // Returns 0 if OK. Non-zero means RTD High/Low Threshold, Short, etc.
}
 
// Helper for logical out-of-bounds check
bool is_temp_out_of_bounds(float temp) {
    return (temp < TEMP_MIN_BOUND || temp > TEMP_MAX_BOUND || isnan(temp));
}
 
// ==========================================
// MQTT Configuration
// ==========================================
 
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883" // Raspberry pi MQTT broker IP-address
// Depending in each case/location, a publish topic must be enabled
#define MQTT_PUBLISH_TOPIC "bus/env/wp211_front"
//#define MQTT_PUBLISH_TOPIC "bus/env/wp211_middle"
//#define MQTT_PUBLISH_TOPIC "bus/env/wp211_rear"
 
esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_mqtt_connected = false;
 
// ==========================================
// Sensor SPI Bus Pin Definitions (SPI2 / HSPI)
// ==========================================
#define PIN_NUM_MISO 6
#define PIN_NUM_MOSI 5
#define PIN_NUM_CLK  15
#define PIN_NUM_CS1  17  // CS for MAX31856 (T-type TC)
#define PIN_NUM_CS2  7   // CS for MAX31865 (RTD 3-wire)
#define PIN_NUM_A0   20  // ADG704 Multiplexer A0
#define PIN_NUM_A1   3   // ADG704 Multiplexer A1
 
// ==========================================
// Ethernet (W5500) Pin Definitions (SPI3 / FSPI)
// Confirmed pinout for LilyGO T-ETH-Lite ESP32-S3.
// Kept on a SEPARATE SPI host from the sensor bus above.
// ==========================================
#define ETH_SPI_HOST      SPI3_HOST
#define ETH_PIN_SCLK      10
#define ETH_PIN_MISO      11
#define ETH_PIN_MOSI      12
#define ETH_PIN_CS        9
#define ETH_PIN_INT       13
#define ETH_PIN_RST       14
#define ETH_SPI_CLOCK_MHZ 20
#define ETH_PHY_ADDR      1   // W5500 fixed PHY address on this board
 
static esp_eth_handle_t eth_handle = NULL;
static volatile bool is_eth_connected = false;
 
// ==========================================
// Sensor Configuration Constants
// ==========================================
#define RTD_NOMINAL_RESISTANCE 100.0f // Standard (100 ohms at 100C)
#define RTD_REFERENCE_RESISTOR 430.0f // Adafruit standard ref resistor (430ohms) for PT100
#define TC_TYPE_T_CR1_VAL      0x07   // T-Type Thermocouple (0111 HEX)
 
// ==========================================
// Ethernet Event Handlers
// ==========================================
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
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
 
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    is_eth_connected = true;
}
 
// ==========================================
// Ethernet Initialization (W5500 over SPI3)
// Sets up the network stack (netif + event loop) that MQTT depends on,
// then starts link negotiation in the BACKGROUND (non-blocking).
// This is what was previously missing entirely, causing the LWIP
// "assert failed: tcpip_send_msg_wait_sem ... Invalid mbox" crash loop:
// MQTT was trying to open a socket before any netif/event loop existed.
// Sensor reads in app_main() proceed immediately after this call returns,
// regardless of whether a physical link/IP has been obtained yet.
// ==========================================
void init_ethernet(void) {
    ESP_LOGI(TAG, "Initializing W5500 Ethernet...");
 
    // 1. Core network stack that LWIP/MQTT depend on
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
 
    // ADDED: required before any GPIO-interrupt-driven peripheral (like the
    // W5500's INT pin) can register its handler. Missing this caused the
    // silent "gpio isr service is not installed" error and permanent link hang.
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
 
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
 
    // 2. Dedicated SPI bus for the W5500 (kept separate from sensor SPI2 bus)
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
 
    // ADDED: derive a real MAC from the chip's factory-programmed base MAC
    // instead of leaving it at the all-zero placeholder (some switches/
    // routers silently drop packets from an all-zero source MAC)
    uint8_t eth_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(eth_mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));
 
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
 
    // 3. Register handlers + bring the link up
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
 
    // CHANGED: No longer blocks here waiting for an IP.
    // Sensor reading and local fault detection must not depend on the
    // network being up (e.g. a field technician bumps the cable loose).
    // The link/IP will arrive asynchronously via the event handlers above;
    // is_eth_connected is checked opportunistically wherever it matters
    // (currently: inside publish_sensor_data(), via is_mqtt_connected).
    ESP_LOGI(TAG, "Ethernet init complete. Link negotiation continues in background.");
}
 
// ==========================================
// MQTT Event Handler
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected to Broker!");
            is_mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected.");
            is_mqtt_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error Occurred.");
            break;
        default:
            break;
    }
}
 
void init_mqtt(void) {
    ESP_LOGI(TAG, "Initializing MQTT Client...");
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}
 
// ==========================================
// Hardware Initialization (Sensors)
// ==========================================
void init_multiplexer(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_NUM_A0) | (1ULL << PIN_NUM_A1),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);
}
 
void init_spi_bus(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
 
    spi_device_interface_config_t devcfg_tc = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 1,
        .spics_io_num = PIN_NUM_CS1,
        .queue_size = 7
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg_tc, &spi_max31856));
 
    spi_device_interface_config_t devcfg_rtd = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 1,
        .spics_io_num = PIN_NUM_CS2,
        .queue_size = 7
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg_rtd, &spi_max31865));
}
 
// ==========================================
// SPI Read Helpers
// ==========================================
esp_err_t spi_read_registers(spi_device_handle_t spi, uint8_t reg_addr, uint8_t *data_out, size_t len) {
    uint8_t tx_buffer[len + 1];
    uint8_t rx_buffer[len + 1];
    memset(tx_buffer, 0, sizeof(tx_buffer));
    memset(rx_buffer, 0, sizeof(rx_buffer));
    tx_buffer[0] = reg_addr & 0x7F;
 
    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx_buffer,
        .rx_buffer = rx_buffer
    };
    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    if (ret == ESP_OK) memcpy(data_out, &rx_buffer[1], len);
    return ret;
}
 
esp_err_t spi_write_register(spi_device_handle_t spi, uint8_t reg_addr, uint8_t data_in) {
    uint8_t tx_buffer[2] = {reg_addr | 0x80, data_in};
    spi_transaction_t t = { .length = 16, .tx_buffer = tx_buffer };
    return spi_device_polling_transmit(spi, &t);
}
 
// ==========================================
// Sensor Read Functions
// ==========================================
void select_thermocouple_channel(uint8_t channel) {
    gpio_set_level(PIN_NUM_A0, channel & 0x01);
    gpio_set_level(PIN_NUM_A1, (channel >> 1) & 0x01);
 
    // Optimized delay for ADG704 switching and signal settling
    vTaskDelay(pdMS_TO_TICKS(5));
}
 
float read_thermocouple_temperature(void) {
    spi_write_register(spi_max31856, 0x00, 0x80);
    spi_write_register(spi_max31856, 0x01, TC_TYPE_T_CR1_VAL);
 
    // Optimized delay for MAX31856 conversion time
    vTaskDelay(pdMS_TO_TICKS(160));
 
    uint8_t rx_data[3];
    spi_read_registers(spi_max31856, 0x0C, rx_data, 3);
 
    int32_t temp_raw = (rx_data[0] << 16) | (rx_data[1] << 8) | rx_data[2];
    temp_raw >>= 5;
    if (temp_raw & 0x40000) temp_raw |= 0xFFF80000;
 
    // 1 bit in the LSB (exp(2;-7)) which is 0,0078125
    return (float)temp_raw * 0.0078125f;
}
 
float read_rtd_temperature(void) {
    spi_write_register(spi_max31865, 0x00, 0xD2);
    vTaskDelay(pdMS_TO_TICKS(100)); // Max conversion time is ~65ms for 50Hz filter
 
    uint8_t rx_data[2];
    spi_read_registers(spi_max31865, 0x01, rx_data, 2);
 
    uint16_t rtd_raw = (rx_data[0] << 8) | rx_data[1];
    rtd_raw >>= 1;
 
    float rtd_resistance = ((float)rtd_raw * RTD_REFERENCE_RESISTOR) / 32768.0f;
 
    // returning the ratiometric relationship
    return (rtd_resistance - RTD_NOMINAL_RESISTANCE) / (RTD_NOMINAL_RESISTANCE * 0.00385f);
}
 
// ==========================================
// JSON Payload Generation & Publish
// ==========================================
void publish_sensor_data(float tc_temps[4], float rtd_temp, uint8_t tc_hw_faults[4], uint8_t rtd_hw_fault) {
    // ADDED: evaluate + log faults locally regardless of network state,
    // so a technician watching the serial console (or a later log pull)
    // still sees problems even while the node is offline. Publishing to
    // MQTT is the only part gated on connectivity below.
    for (int i = 0; i < 4; i++) {
        if (tc_hw_faults[i] != 0) {
            ESP_LOGW(TAG, "T%d HARDWARE FAULT code 0x%02X", i + 1, tc_hw_faults[i]);
        } else if (is_temp_out_of_bounds(tc_temps[i])) {
            ESP_LOGW(TAG, "T%d OUT OF BOUNDS: %.1f C", i + 1, tc_temps[i]);
        }
    }
    if (rtd_hw_fault != 0) {
        ESP_LOGW(TAG, "RTD HARDWARE FAULT code 0x%02X", rtd_hw_fault);
    } else if (is_temp_out_of_bounds(rtd_temp)) {
        ESP_LOGW(TAG, "RTD OUT OF BOUNDS: %.1f C", rtd_temp);
    }
 
    if (!is_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected. Sensors still read locally; publish skipped.");
        return;
    }
 
    cJSON *root = cJSON_CreateObject();
 
    // 1. Data Block
    cJSON_AddNumberToObject(root, "c1_temp_C_head_standing", tc_temps[0]);
    cJSON_AddNumberToObject(root, "c2_temp_C_head_seated", tc_temps[1]);
    cJSON_AddNumberToObject(root, "c3_temp_C_abdomen_seated", tc_temps[2]);
    cJSON_AddNumberToObject(root, "c4_temp_C_ankle_feet", tc_temps[3]);
    cJSON_AddNumberToObject(root, "k1_temp_C_control", rtd_temp);
 
    // 2. Diagnostics Block
    cJSON *diagnostics = cJSON_AddObjectToObject(root, "diagnostics");
    cJSON_AddBoolToObject(diagnostics, "mqtt_connected", true);
    cJSON_AddBoolToObject(diagnostics, "watchdog_triggered", false); // Can be tied to ESP-IDF task watchdog
 
    cJSON *sensor_faults = cJSON_AddArrayToObject(diagnostics, "sensor_faults");
 
    // 3. Evaluate and append faults to the array
    char fault_msg[50];
    for (int i = 0; i < 4; i++) {
        if (tc_hw_faults[i] != 0) {
            snprintf(fault_msg, sizeof(fault_msg), "T%d_HARDWARE_FAULT_CODE_0x%02X", i+1, tc_hw_faults[i]);
            cJSON_AddItemToArray(sensor_faults, cJSON_CreateString(fault_msg));
        } else if (is_temp_out_of_bounds(tc_temps[i])) {
            snprintf(fault_msg, sizeof(fault_msg), "T%d_OUT_OF_BOUNDS_%.1fC", i+1, tc_temps[i]);
            cJSON_AddItemToArray(sensor_faults, cJSON_CreateString(fault_msg));
        }
    }
 
    if (rtd_hw_fault != 0) {
        snprintf(fault_msg, sizeof(fault_msg), "RTD_HARDWARE_FAULT_CODE_0x%02X", rtd_hw_fault);
        cJSON_AddItemToArray(sensor_faults, cJSON_CreateString(fault_msg));
    } else if (is_temp_out_of_bounds(rtd_temp)) {
        snprintf(fault_msg, sizeof(fault_msg), "RTD_OUT_OF_BOUNDS_%.1fC", rtd_temp);
        cJSON_AddItemToArray(sensor_faults, cJSON_CreateString(fault_msg));
    }
 
    // Render and publish
    char *json_string = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(mqtt_client, MQTT_PUBLISH_TOPIC, json_string, 0, 1, 0);
    ESP_LOGD(TAG, "Payload: %s", json_string);
 
    cJSON_Delete(root);
    free(json_string);
}
 
// ==========================================
// Main Application Loop
// ==========================================
void app_main(void) {
    ESP_LOGI(TAG, "Starting WP2.1.i Node with 1Hz MQTT Publish...");
 
    // Network stack must exist (netif + event loop) before MQTT is started,
    // which init_ethernet() sets up. It no longer blocks waiting for a link/
    // IP -- sensor reads below proceed immediately even while offline.
    // Previously this line was commented out entirely, which caused the
    // "Invalid mbox" crash loop -- MQTT had no network stack to attach to.
    init_ethernet();
 
    init_multiplexer();
    init_spi_bus();
    init_mqtt();
 
    float tc_temps[4] = {0};
 
    // Setup for exact 1Hz execution frequency
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000);
 
    while (1) {
        ESP_LOGI(TAG, "--- Sensor Reading Cycle Start ---");
 
        uint8_t tc_hw_faults[4] = {0};
 
        // 1. Read 4 multiplexed T-Type thermocouples + Faults
        for (uint8_t i = 0; i < 4; i++) {
            select_thermocouple_channel(i);
            tc_temps[i] = read_thermocouple_temperature();
            tc_hw_faults[i] = check_thermocouple_hardware_fault();
        }
 
        // 2. Read PT100 RTD + Faults
        float rtd_temp = read_rtd_temperature();
        uint8_t rtd_hw_fault = check_rtd_hardware_fault();
 
        // 3. Construct JSON with Diagnostics and Publish via MQTT
        publish_sensor_data(tc_temps, rtd_temp, tc_hw_faults, rtd_hw_fault);
 
        // 4. Wait for the exact time remaining to complete (1000ms)
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}
