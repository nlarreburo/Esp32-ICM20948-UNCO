#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

#include "icm20948.h"
#include "circ_buffer.h"
#include "measurement.h"
#include "protocol.h"
#include "wifi_manager.h"
#include "espnow_manager.h"
#include "slave_client.h"
#include "slave_server.h"

//----------------------
// PINOUT
//----------------------
//ICM-20948 en SPI2
#define ICM_SPI_HOST    SPI2_HOST
#define PIN_MOSI        23
#define PIN_MISO        19
#define PIN_CLK         18
#define PIN_CS_ICM      5

//SD en SPI3
#define PIN_SD_MOSI     13
#define PIN_SD_MISO     27
#define PIN_SD_CLK      14
#define PIN_CS_SD       15

#define LED_PIN         2
#define PIN_BTN_MASTER  0              //Pulsador cambio de master, luego analizar q boton sera
//----------------------
// Frecuencia de muestreo del sensor (ODR)
//----------------------
#define SENSOR_SAMPLE_RATE_HZ   225

//----------------------
// CONFIGURACION DEL VIGILANTE
//----------------------

#define VIGILANTE_WINDOW        22      //Cantidad de muestras que analiza el interruptor
#define VIGILANTE_THRESHOLD_G   1.5f   //Humbral de deteccion 5%
#define VIGILANTE_COOLDOWN_MS   2000    
#define VIGILANTE_INTERVAL_MS   100     //Cada 100ms analizamos la muestras
#define TX_THRESHOLD            112     //Minimos de muestras para que tx_task las procese 225hz / 2

//----------------------
// CONFIGURACION DE RED
//----------------------

#define MASTER_DISCOVERY_MS     10000    //Tiempo de espera buscando un master
#define HEARTBEAT_INTERVAL_MS   5000    //Tiempo del master para realizar un heartbeat (buscar esclavos)
#define NODE_OFFLINE_TIMEOUT_MS 15000   //Tiempo de espera hasta considerar nodo perdido
#define MAX_NODES               8       //Cantidad de nodos maximo en la red (chequear comportamiento de ram)
#define MEASUREMENT_SLOTS       5       //Cantidad de mediones almacenadas en PSRAM/RAM

//NVS CONFIG

#define NVS_NAMESPACE   "config"
#define NVS_KEY_PRE     "pre_ms"
#define NVS_KEY_POST    "post_ms"
#define NVS_KEY_MANUAL  "manual_ms"
#define NVS_KEY_THR     "thr_g"
//----------------------
// CONFIGURACION STEAM
//----------------------

#define STREAM_CHUNK_SAMPLES 45

static const char *TAG = "MAIN";

//----------------------
// ESTADO GLOBAL
//----------------------

static icm20948_handle_t  g_sensor;
static circ_buffer_t      g_buffer;

static measurement_config_t g_config = {                //g_config guarda los parametros activos de medicion
    .pre_trigger_ms     = CONFIG_DEFAULT_PRE_MS,
    .post_trigger_ms    = CONFIG_DEFAULT_POST_MS,
    .manual_duration_ms = CONFIG_DEFAULT_MANUAL_MS,
    .threshold_g        = CONFIG_DEFAULT_THRESHOLD_G,
};  
static SemaphoreHandle_t g_config_mutex = NULL;         //Mutex para g_config
static SemaphoreHandle_t g_trigger_mutex = NULL;        //Mutex para trigger

static node_role_t   g_role              = NODE_ROLE_SLAVE;  //Inicializamos los nodos como esclavos
static node_status_t g_status            = NODE_STATUS_IDLE; //Estados: IDLE, RECORDING O STREAMING
static uint8_t       g_master_mac[6]     = {0};              //MAC del master
static uint8_t       g_own_mac[6]        = {0};              //MAC del nodo
static uint8_t       g_stream_target[6]  = {0};              //MAC del nodo en stream activo
static uint8_t       g_current_slot      = 0;                //Indice del slot donde se guarda la medicion

static node_state_t      g_nodes[MAX_NODES] = {0};      //Vector con MACS de los nodos
static uint8_t           g_node_count = 0;              //Cantidad de nodos conectados
static SemaphoreHandle_t g_nodes_mutex = NULL;          //Mutex para g_nodes

static measurement_t    *g_slots[MEASUREMENT_SLOTS] = {0};
static SemaphoreHandle_t g_slots_mutex = NULL;          //Mutex para g_slots

#define EVT_VIBRACION       (1 << 0)                    // bit 0 = 0b00000001
#define EVT_TRIGGER         (1 << 1)                    // bit 1 = 0b00000010
#define EVT_STOP            (1 << 2)                    // bit 2 = 0b00000100
#define EVT_NUEVO_MASTER    (1 << 3)                    // bit 3 = 0b00001000
static EventGroupHandle_t g_eventos = NULL;

static uint8_t   g_trigger_slot   = 0;
static uint8_t   g_trigger_source = TRIGGER_SOURCE_AUTO;
static uint32_t  g_trigger_ts     = 0;                  //Timestamp del master en el momento del trigger

static int     g_stream_fd = -1;                        //Descriptor del socket de la PC durante un stream activo. -1 -> no hay stream
static uint8_t g_stream_node_mac[6] = {0};
static uint16_t g_trigger_head = 0;

// Flag de congelamiento del buffer circular
// true  → sensor_task NO escribe en el buffer circular
// false → sensor_task escribe normalmente
static volatile bool g_buffer_frozen = false;
static volatile bool s_pending_data_request = false;

// Buffer post-trigger — sensor_task escribe aca mientras el circular esta congelado
// sensor_task llena este buffer con las muestras nuevas despues del trigger
#define POST_TRIGGER_MAX_SAMPLES  13500   // 60 segundos a 225Hz
static icm20948_raw_sample_t g_post_buf[POST_TRIGGER_MAX_SAMPLES];
static volatile uint16_t g_post_count  = 0;    // muestras acumuladas
static volatile bool     g_post_active = false; // true = sensor_task llenando post
static volatile bool g_auto_sync = false;

//----------------------
// HELPERS
//----------------------

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])   //Comparar 2 MACS
{
    return memcmp(a, b, 6) == 0;
}

static bool mac_less(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) < 0;
}

static int find_or_add_node(const uint8_t mac[6])               //Busca un nodo en g_nodes por su MAC, devuelve su indice, si no, deuvuelve uno nuevo, si esta full un -1
{
    xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);  
    for (int i = 0; i < g_node_count; i++) {
        if (mac_equal(g_nodes[i].mac, mac)) {
            xSemaphoreGive(g_nodes_mutex);
            return i;
        }
    }
    if (g_node_count < MAX_NODES) {
        int idx = g_node_count++;
        memset(&g_nodes[idx], 0, sizeof(node_state_t));
        memcpy(g_nodes[idx].mac, mac, 6);
        g_nodes[idx].role         = NODE_ROLE_SLAVE;
        g_nodes[idx].status       = NODE_STATUS_IDLE;
        g_nodes[idx].last_seen_ms = now_ms();
        espnow_manager_add_peer(mac);
        ESP_LOGI(TAG, "Nodo nuevo: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        xSemaphoreGive(g_nodes_mutex);
        return idx;
    }
    xSemaphoreGive(g_nodes_mutex);
    return -1;
}

//nvs config

static void config_load(void)
{
    nvs_handle_t h;
    if (nvs_open("config", NVS_READONLY, &h) != ESP_OK) return;

    uint16_t val_u;
    uint32_t val_raw;  // float se guarda como uint32_t (mismo tamaño en memoria)

    if (nvs_get_u16(h, "pre_ms",    &val_u) == ESP_OK) g_config.pre_trigger_ms     = val_u;
    if (nvs_get_u16(h, "post_ms",   &val_u) == ESP_OK) g_config.post_trigger_ms    = val_u;
    if (nvs_get_u16(h, "manual_ms", &val_u) == ESP_OK) g_config.manual_duration_ms = val_u;
    if (nvs_get_u32(h, "thr_g",     &val_raw) == ESP_OK) memcpy(&g_config.threshold_g, &val_raw, 4);

    nvs_close(h);
    ESP_LOGI(TAG, "Config cargada desde NVS");
}

static void config_save(const measurement_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("config", NVS_READWRITE, &h) != ESP_OK) return;

    uint32_t val_raw;
    memcpy(&val_raw, &cfg->threshold_g, 4);

    nvs_set_u16(h, "pre_ms",    cfg->pre_trigger_ms);
    nvs_set_u16(h, "post_ms",   cfg->post_trigger_ms);
    nvs_set_u16(h, "manual_ms", cfg->manual_duration_ms);
    nvs_set_u32(h, "thr_g",     val_raw);
    nvs_commit(h);
    nvs_close(h);
}

static esp_err_t apply_config(const measurement_config_t *cfg)   //Actualizar g_config con la nueva cfg
{
    if (cfg->manual_duration_ms<= 0) return ESP_ERR_INVALID_ARG;
    if (cfg->post_trigger_ms<= 0) return ESP_ERR_INVALID_ARG;
    if (cfg->threshold_g<= 0.0f) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    memcpy(&g_config, cfg, sizeof(measurement_config_t));
    xSemaphoreGive(g_config_mutex);
    ESP_LOGI(TAG, "Config: PRE=%dms POST=%dms THR=%.2fg",
             cfg->pre_trigger_ms, cfg->post_trigger_ms, cfg->threshold_g);
    config_save(cfg);
    return ESP_OK;
}

static void on_espnow_config(const msg_config_t *msg)
{
    esp_err_t ret = apply_config(&msg->config);
    espnow_send_config_ack(g_master_mac, ret == ESP_OK ? 1 : 0);
}

//----------------------
// ALMACENAMIENTO SD
//----------------------

static sdmmc_card_t *s_sd_card = NULL;
static bool s_sd_ready = false;

static esp_err_t sd_init(void)
{
    // Inicializar SPI3 para la SD
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_SD_MOSI,
        .miso_io_num     = PIN_SD_MISO,
        .sclk_io_num     = PIN_SD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD: error inicializando bus SPI3: %s", esp_err_to_name(ret));
        s_sd_ready = false;
        return ret;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 4000; // 4 MHz — evita CRC errors durante init

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = PIN_CS_SD;
    slot_cfg.host_id = SPI3_HOST;

    ret = esp_vfs_fat_sdspi_mount("/sd", &host, &slot_cfg, &mount_cfg, &s_sd_card);

    if (ret != ESP_OK){
        ESP_LOGW(TAG, "SD no montada: %s", esp_err_to_name(ret));
        s_sd_ready = false;
        return ret;
    }

    int r = mkdir("/sd/mediciones", 0775);
    if (r != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "SD: mkdir /sd/mediciones fallo (errno=%d)", errno);
    }

    s_sd_ready = true;
    ESP_LOGI(TAG,"SD montada correctamente");
    sdmmc_card_print_info(stdout, s_sd_card);
    return ESP_OK;
}

static void sd_save_measurement(measurement_t *m)
{
    if (!s_sd_ready || m == NULL) return;

    // nombre: mediciones/MAC_TIMESTAMP_slotN.bin
    char path[80];
    snprintf(path, sizeof(path),
            "/sd/mediciones/%02x%02x%02x_%"PRIu32"_slot%d.bin",
             m->node_mac[3], m->node_mac[4], m->node_mac[5],
             m->timestamp_ms, m->slot_index);

    size_t total = measurement_size(m->num_samples);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        // intento crear el directorio por si no existe aun y reintento
        mkdir("/sd/mediciones", 0775);
        f = fopen(path, "wb");
    }
    if (f == NULL) {
        ESP_LOGW(TAG, "SD: error creando %s (errno=%d)", path, errno);
        return;
    }

    size_t written = fwrite(m, 1, total, f);
    fclose(f);

    if (written == total) {
        ESP_LOGI(TAG, "SD: guardado %s (%d bytes)", path, (int)total);
    } else {
        ESP_LOGW(TAG, "SD: escritura incompleta %d/%d", (int)written, (int)total);
    }
}

//----------------------
// CALLBACKS ESP-NOW
//----------------------

static void on_espnow_trigger_notify(const msg_trigger_notify_t *msg) //Funcion de 
{
    if (g_role != NODE_ROLE_MASTER) return;         //Chequeo de master
    xSemaphoreTake(g_trigger_mutex, portMAX_DELAY);
    if (g_status != NODE_STATUS_IDLE){ //Chequeo de no estar ya grabando
        xSemaphoreGive(g_trigger_mutex);
        return;
    }  
    g_status = NODE_STATUS_RECORDING;
    xSemaphoreGive(g_trigger_mutex);
    //Definimos los parametros de trigger
    g_trigger_slot   = g_current_slot;
    g_trigger_source = TRIGGER_SOURCE_AUTO;
    g_trigger_ts     = now_ms();                    //Ts en el momento en que el master recibio notif

    // Activar sistema de captura
    g_buffer_frozen = true;
    g_post_count    = 0;
    g_post_active   = true;

    espnow_send_trigger(ESPNOW_BROADCAST_MAC,       //Broadcast a toda la red para comenzar a grabar -> on_espnow_trigger
                        g_trigger_slot, TRIGGER_SOURCE_AUTO, g_trigger_ts);
        
    char event[80];
    snprintf(event, sizeof(event),
             RESP_EVENT_TRIGGER " MAC=%02x:%02x:%02x:%02x:%02x:%02x"
             " SLOT=%d RMS=%.3f PEAK=0.000\n",
             msg->header.src_mac[0], msg->header.src_mac[1],
             msg->header.src_mac[2], msg->header.src_mac[3],
             msg->header.src_mac[4], msg->header.src_mac[5],
             g_trigger_slot, msg->rms_g);
    wifi_manager_send_event(event); //Aviso a la pc del evento

    xEventGroupSetBits(g_eventos, EVT_TRIGGER);     //Master comienza a grabar
}

static void on_espnow_trigger(const msg_trigger_t *msg) //Esclavo recibe el broadcast del master
{
    if (g_role != NODE_ROLE_SLAVE) return;          //Chequeo q sea esclavo
    //Tomos los parametros del master
    g_trigger_slot   = msg->slot_index;             
    g_trigger_source = msg->trigger_source;
    g_trigger_ts     = msg->timestamp_ms;           //Tomamos el ts del master

    // Activar el sistema de captura igual que el master
    g_buffer_frozen = true;
    g_post_count    = 0;
    g_post_active   = true;

    espnow_send_trigger_ack(g_master_mac, msg->slot_index, now_ms());   //Le envio al master el ts del esclavo, deberiamos usar estos valores para la calibracion, ni idea todavia
    xEventGroupSetBits(g_eventos, EVT_TRIGGER);     //Despertamos a tx_task para iniciar la grabacion
}

static void on_espnow_trigger_ack(const msg_trigger_ack_t *msg) //Master recibe la confirmacion de grabacion
{
    if (g_role != NODE_ROLE_MASTER) return;
    int idx = find_or_add_node(msg->header.src_mac);    
    if (idx < 0) return;
    //Si el nodo existe, modifico el status
    xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
    g_nodes[idx].status       = NODE_STATUS_RECORDING;
    g_nodes[idx].current_slot = msg->slot_index;
    g_nodes[idx].last_seen_ms = now_ms();
    xSemaphoreGive(g_nodes_mutex);

    int32_t desfase = (int32_t)(msg->timestamp_ms - g_trigger_ts);  //Realizamos el calculo de desfase
    ESP_LOGI(TAG, "ACK %02x:...:%02x desfase=%ldms",
             msg->header.src_mac[0], msg->header.src_mac[5], desfase);
}

static void on_espnow_stop(const msg_stop_t *msg)
{
    xEventGroupSetBits(g_eventos, EVT_STOP);
    espnow_send_stop_ack(g_master_mac, 0);
}

static void on_espnow_config_ack(const msg_config_ack_t *msg)       //ACK Esclavo confirma nueva config
{
    int idx = find_or_add_node(msg->header.src_mac);
    if (idx >= 0){
        xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
        g_nodes[idx].config_sync = msg->ok;
        xSemaphoreGive(g_nodes_mutex);
    }
    ESP_LOGI(TAG, "CONFIG_ACK de %02x:...:%02x ok=%d",
             msg->header.src_mac[0], msg->header.src_mac[5], msg->ok);
    char event[64];
    snprintf(event, sizeof(event),
             "EVENT CONFIG_ACK %02x:%02x:%02x:%02x:%02x:%02x ok=%d\n",
             msg->header.src_mac[0], msg->header.src_mac[1],
             msg->header.src_mac[2], msg->header.src_mac[3],
             msg->header.src_mac[4], msg->header.src_mac[5],
             msg->ok);
    wifi_manager_send_event(event);
}

static void on_espnow_heartbeat(const msg_heartbeat_t *msg)         //Esclavo recibe heartbeat del master
{
    if (g_role != NODE_ROLE_SLAVE) return;
    memcpy(g_master_mac, msg->header.src_mac, 6);
    g_current_slot = msg->current_slot; //Asignamos el slot al esclavo segundo lo indica el master
    espnow_manager_add_peer(g_master_mac); //Agregamos la mac del master como peer
    espnow_send_heartbeat_ack(g_master_mac, g_status,       //Contesta al heartbeat
                               g_current_slot, esp_get_free_heap_size());
}

static void on_espnow_data_request(const msg_data_request_t *msg)
{
    if (g_role != NODE_ROLE_SLAVE) return;

    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    measurement_t *m = (msg->slot_index < MEASUREMENT_SLOTS)
                        ? g_slots[msg->slot_index] : NULL;
    xSemaphoreGive(g_slots_mutex);

    if (m == NULL) {
        ESP_LOGW(TAG, "Slot %d vacio", msg->slot_index);
        return;
    }

    esp_err_t ret = slave_client_send_measurement(m);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Error enviando medicion al master");
}

static void on_espnow_heartbeat_ack(const msg_heartbeat_ack_t *msg) //Master recibe respuesta de heartbeat
{
    if (g_role != NODE_ROLE_MASTER) return;
    int idx = find_or_add_node(msg->header.src_mac);
    if (idx < 0) return;

    xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
    g_nodes[idx].status       = msg->status;
    g_nodes[idx].current_slot = msg->current_slot;
    g_nodes[idx].last_seen_ms = now_ms();
    uint8_t needs_config = !g_nodes[idx].config_sync;
    xSemaphoreGive(g_nodes_mutex);

    if (needs_config){
        xSemaphoreTake(g_config_mutex, portMAX_DELAY);
        measurement_config_t cfg = g_config;
        xSemaphoreGive(g_config_mutex);
        espnow_send_config(msg->header.src_mac, &cfg);
    }
}


static void on_espnow_new_master(const msg_new_master_t *msg)   
{
    ESP_LOGI(TAG, "MSG_NEW_MASTER recibido de %02x:%02x:%02x:%02x:%02x:%02x",
             msg->header.src_mac[0], msg->header.src_mac[1],
             msg->header.src_mac[2], msg->header.src_mac[3],
             msg->header.src_mac[4], msg->header.src_mac[5]);
    const uint8_t *candidate = msg->header.src_mac;
    if (g_role == NODE_ROLE_MASTER) {
        // Perdemos el rol — reiniciar para arrancar como esclavo
        ESP_LOGW(TAG, "Cediendo rol MASTER a %02x:%02x:%02x:%02x:%02x:%02x — reiniciando",
                 candidate[0], candidate[1], candidate[2],
                 candidate[3], candidate[4], candidate[5]);
        vTaskDelay(pdMS_TO_TICKS(500));  // dar tiempo al log de imprimirse
        esp_restart();                   // reinicio del sistema
    }
    memcpy(g_master_mac, candidate, 6);
    espnow_send_new_master_ack(candidate);
    xEventGroupSetBits(g_eventos, EVT_NUEVO_MASTER);
    ESP_LOGI(TAG, "Nuevo master: %02x:%02x:%02x:%02x:%02x:%02x",
             candidate[0], candidate[1], candidate[2],
             candidate[3], candidate[4], candidate[5]);
}

static void on_espnow_sync_start(const msg_sync_start_t *msg)
{
    if (g_role != NODE_ROLE_SLAVE) return;
    g_auto_sync = true;
    ESP_LOGI(TAG, "Sincronizacion activada");
}

static void on_espnow_sync_stop(const msg_sync_stop_t *msg)
{
    if (g_role != NODE_ROLE_SLAVE) return;
    g_auto_sync = false;
    ESP_LOGI(TAG, "Sincronizacion desactivada");
}

static void on_espnow_stream_start(const msg_stream_start_t *msg)
{
    g_status        = NODE_STATUS_STREAMING;
    g_buffer_frozen = true;   // congela el circular para que tx_task lea el pre sin solapamiento
    g_post_count    = 0;
    g_post_active   = false;
    xEventGroupSetBits(g_eventos, EVT_TRIGGER);  // despierta tx_task
    ESP_LOGI(TAG, "Modo stream activado");
}

static void on_espnow_stream_stop(const msg_stream_stop_t *msg)
{
    xEventGroupSetBits(g_eventos, EVT_STOP);
    ESP_LOGI(TAG, "Modo stream desactivado");
}
//----------------------
// CALLBACKS WIFI
//----------------------

static esp_err_t on_wifi_start(void)    //CMD_START
{
    if (g_role != NODE_ROLE_MASTER) return ESP_FAIL;
    xSemaphoreTake(g_trigger_mutex, portMAX_DELAY);
    if (g_status != NODE_STATUS_IDLE){
        xSemaphoreGive(g_trigger_mutex);
        return ESP_FAIL;
    }
    g_status = NODE_STATUS_RECORDING;
    xSemaphoreGive(g_trigger_mutex);
    g_trigger_slot   = g_current_slot;
    g_trigger_source = TRIGGER_SOURCE_MANUAL;
    g_trigger_ts     = now_ms();

    // Activar sistema de captura igual que vigilante_task
    g_buffer_frozen = true;
    g_post_count    = 0;
    g_post_active   = true;

    espnow_send_trigger(ESPNOW_BROADCAST_MAC, g_trigger_slot,
                        TRIGGER_SOURCE_MANUAL, g_trigger_ts);
    xEventGroupSetBits(g_eventos, EVT_TRIGGER);
    return ESP_OK;
}

static esp_err_t on_wifi_stop(void)     //CMD_STOP
{
    if (g_status == NODE_STATUS_IDLE) return ESP_FAIL;
    espnow_send_stop(ESPNOW_BROADCAST_MAC);
    xEventGroupSetBits(g_eventos, EVT_STOP);
    return ESP_OK;
}

static esp_err_t on_wifi_get_config(measurement_config_t *out)
{
    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    *out = g_config;
    xSemaphoreGive(g_config_mutex);
    return ESP_OK;
}

static esp_err_t on_wifi_get_status(char *out_json, size_t max_len)     //CMD_GET_STATUS
{
    int pos = 0;
    pos += snprintf(out_json + pos, max_len - pos,
                    "{\"role\":\"%s\",",
                    g_role == NODE_ROLE_MASTER ? "master" : "slave");

    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    measurement_config_t cfg = g_config;
    xSemaphoreGive(g_config_mutex);

    pos += snprintf(out_json + pos, max_len - pos,
                    "\"config\":{\"pre_ms\":%d,\"post_ms\":%d,"
                    "\"manual_ms\":%d,\"thr_g\":%.2f},",
                    cfg.pre_trigger_ms, cfg.post_trigger_ms,
                    cfg.manual_duration_ms, cfg.threshold_g);

    pos += snprintf(out_json + pos, max_len - pos, "\"nodes\":[");

    // Master propio — siempre el primer nodo, siempre online
    const char *own_st;
    switch (g_status) {
        case NODE_STATUS_RECORDING: own_st = "recording"; break;
        case NODE_STATUS_STREAMING: own_st = "streaming"; break;
        default:                    own_st = "idle";      break;
    }
    pos += snprintf(out_json + pos, max_len - pos,
                    "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                    "\"status\":\"%s\",\"slot\":%d,\"online\":true,\"role\":\"master\"}",
                    g_own_mac[0], g_own_mac[1], g_own_mac[2],
                    g_own_mac[3], g_own_mac[4], g_own_mac[5],
                    own_st, g_current_slot);

    xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < g_node_count && pos < (int)max_len - 60; i++) {
        const char *st;
        switch (g_nodes[i].status) {
            case NODE_STATUS_RECORDING: st = "recording"; break;
            case NODE_STATUS_STREAMING: st = "streaming"; break;
            default:                    st = "idle";      break;
        }
        bool offline = (now_ms() - g_nodes[i].last_seen_ms) > NODE_OFFLINE_TIMEOUT_MS;
        pos += snprintf(out_json + pos, max_len - pos,
                        ",{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                        "\"status\":\"%s\",\"slot\":%d,\"online\":%s}",
                        g_nodes[i].mac[0], g_nodes[i].mac[1],
                        g_nodes[i].mac[2], g_nodes[i].mac[3],
                        g_nodes[i].mac[4], g_nodes[i].mac[5],
                        st, g_nodes[i].current_slot,
                        offline ? "false" : "true");
    }
    xSemaphoreGive(g_nodes_mutex);
    snprintf(out_json + pos, max_len - pos, "]}");
    return ESP_OK;
}

static esp_err_t on_wifi_set_config(const measurement_config_t *cfg)    //CMD_SET_CONFIG
{
    esp_err_t ret = apply_config(cfg);  //actualiza
    if (ret != ESP_OK) return ret; 
    espnow_send_config(ESPNOW_BROADCAST_MAC, cfg);  //propaga a esclavos
    xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
        for (int i = 0; i < g_node_count; i++)
            g_nodes[i].config_sync = 0;
    xSemaphoreGive(g_nodes_mutex);
    return ESP_OK;
}

static esp_err_t on_wifi_stream_start(const uint8_t *mac)
{
    memcpy(g_stream_target, mac, 6);
    if (mac_equal(mac, g_own_mac)) {
        // Stream local del master — misma logica que on_espnow_stream_start
        g_status        = NODE_STATUS_STREAMING;
        g_buffer_frozen = true;
        g_post_count    = 0;
        g_post_active   = false;
        xEventGroupSetBits(g_eventos, EVT_TRIGGER);
    } else {
        espnow_send_stream_start(mac);
    }
    return ESP_OK;
}

static esp_err_t on_wifi_stream_stop_all(void)
{
    xEventGroupSetBits(g_eventos, EVT_STOP);
    xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < g_node_count; i++) {
        espnow_send_stream_stop(g_nodes[i].mac);
    }
    xSemaphoreGive(g_nodes_mutex);
    return ESP_OK;
}

static esp_err_t on_wifi_stream_stop(const uint8_t *mac)
{
    // wifi_manager pasa NULL — usar el MAC guardado en stream_start
    const uint8_t *target = (mac != NULL) ? mac : g_stream_target;

    // g_stream_target == {0} indica modo "all" (seteado por stream_start_all)
    static const uint8_t zero_mac[6] = {0};
    if (memcmp(target, zero_mac, 6) == 0)
        return on_wifi_stream_stop_all();

    if (mac_equal(target, g_own_mac)) {
        xEventGroupSetBits(g_eventos, EVT_STOP);
    } else {
        espnow_send_stream_stop(target);
    }
    return ESP_OK;
}

static esp_err_t on_wifi_stream_start_all(void)
{
    memset(g_stream_target, 0, 6);
    g_status        = NODE_STATUS_STREAMING;
    g_buffer_frozen = true;
    g_post_count    = 0;
    g_post_active   = false;
    xEventGroupSetBits(g_eventos, EVT_TRIGGER);
    xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
    for (int i = 0; i < g_node_count; i++) {
        espnow_send_stream_start(g_nodes[i].mac);
    }
    xSemaphoreGive(g_nodes_mutex);
    return ESP_OK;
}


static esp_err_t on_wifi_sync_start(void)
{
    g_auto_sync = true;
    ESP_LOGI(TAG, "Sincronizacion automatica activada");
    return ESP_OK;
}

static esp_err_t on_wifi_sync_stop(void)
{
    g_auto_sync = false;
    ESP_LOGI(TAG, "Sincronizacion automatica desactivada");
    return ESP_OK;
}

static esp_err_t on_wifi_get_data(const uint8_t *mac, uint8_t slot, int fd)
{
    // Datos del master local
    if (mac_equal(mac, g_own_mac)) {
        xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
        measurement_t *m = (slot < MEASUREMENT_SLOTS) ? g_slots[slot] : NULL;
        xSemaphoreGive(g_slots_mutex);

        if (m == NULL) return ESP_FAIL;

        size_t total = measurement_size(m->num_samples);
        meas_pkt_hdr_t hdr = {
            .type   = MEAS_PKT_DATA,
            .slot   = slot,
            .length = total,
        };
        memcpy(hdr.mac, mac, 6);

        size_t buf_size = sizeof(hdr) + total;
        uint8_t *buf = malloc(buf_size);
        if (buf == NULL) return ESP_FAIL;

        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), m, total);
        wifi_manager_send_raw(buf, buf_size);
        free(buf);
        return ESP_OK;
    }

    // Datos de un esclavo — pedir por ESP-NOW
    s_pending_data_request = true;
    espnow_send_data_request(mac, slot);
    return ESP_OK;
}


//----------------------
// CALLBACK TCP SERVER SLAVE
//----------------------

static void on_slave_measurement(const uint8_t *mac, uint8_t slot, const measurement_t *meas)
{
    if (!wifi_manager_is_connected()) return;
    if (!g_auto_sync && !s_pending_data_request) return;
    s_pending_data_request = false;

    size_t meas_size = measurement_size(meas->num_samples);
    size_t total     = sizeof(meas_pkt_hdr_t) + meas_size;

    uint8_t *buf = malloc(total);
    if (buf == NULL) return;

    meas_pkt_hdr_t hdr = {
        .type   = MEAS_PKT_DATA,
        .slot   = slot,
        .length = meas_size,
    };
    memcpy(hdr.mac, mac, 6);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), meas, meas_size);

    wifi_manager_send_raw(buf, total);
    free(buf);
}

static void on_slave_stream_chunk(const uint8_t *mac, const icm20948_raw_sample_t *samples, uint16_t count)
{
    if (!wifi_manager_is_connected()) return;

    size_t payload  = count * sizeof(icm20948_raw_sample_t);
    size_t total    = sizeof(stream_chunk_hdr_t) + payload;

    uint8_t *buf    = malloc(total);
    if (buf == NULL) return;

    stream_chunk_hdr_t hdr = {
        .type   =   STREAM_PKT_CHUNK,
        .count  =   count,
    };

    memcpy(hdr.mac, mac, 6);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), samples, payload);

    wifi_manager_send_raw(buf, total);
    free(buf);
}

//----------------------
// INICIALIZACION SPI
//----------------------

static esp_err_t spi_bus_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 512,
    };
    return spi_bus_initialize(ICM_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
}



//----------------------
// TAREAS
//----------------------

static icm20948_raw_sample_t s_fifo_buf[ICM20948_FIFO_MAX_SAMPLES];     //Buffer muestras crudas del FIFO del sensor

static void sensor_task(void *pv)
{
    uint16_t n = 0;
    while (1) {
        if (icm20948_read_fifo(&g_sensor, s_fifo_buf, &n) == ESP_OK && n > 0) {
            if (!g_buffer_frozen) {
                // Modo normal — escribir en el buffer circular
                circ_buffer_push(&g_buffer, s_fifo_buf, n);
            } else if (g_post_active) {
                // Buffer circular congelado — escribir en el buffer post-trigger
                // El buffer circular queda exactamente como estaba al momento del trigger
                if (g_post_count < POST_TRIGGER_MAX_SAMPLES){
                    uint16_t espacio = POST_TRIGGER_MAX_SAMPLES - g_post_count; 
                    uint16_t copiar  = (n < espacio) ? n : espacio; 
                    memcpy((void*)(g_post_buf + g_post_count), s_fifo_buf,
                           copiar * sizeof(icm20948_raw_sample_t));
                    g_post_count += copiar;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); //tiempo de espera del 50ms es lo q tarda el FIFO tarda en acumular 11 muestras
    }
}

static icm20948_raw_sample_t s_vigilante_buf[VIGILANTE_WINDOW]; //Buffer estatico de 22 muestras donde el Vigilante copia las ultimas muestras del buffer circular para analizarlas.

static void vigilante_task(void *pv)
{
    TickType_t last_trigger = 0;
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        if (g_status != NODE_STATUS_IDLE) { //Suspendemos el vigilante si
            vTaskDelay(pdMS_TO_TICKS(VIGILANTE_INTERVAL_MS));
            continue;
        }

        uint16_t n = circ_buffer_peek_last(&g_buffer, s_vigilante_buf,
                                            VIGILANTE_WINDOW);
        if (n == 0) { vTaskDelay(pdMS_TO_TICKS(VIGILANTE_INTERVAL_MS)); continue; }

        xSemaphoreTake(g_config_mutex, portMAX_DELAY);
        float threshold = g_config.threshold_g;
        xSemaphoreGive(g_config_mutex);

        float sum_sq = 0.0f;
        for (uint16_t i = 0; i < n; i++) {
            float x = (float)s_vigilante_buf[i].x / g_sensor.acel_scale;
            float y = (float)s_vigilante_buf[i].y / g_sensor.acel_scale;
            float z = (float)s_vigilante_buf[i].z / g_sensor.acel_scale;
            sum_sq += x*x + y*y + z*z;
        }
        float rms = sqrtf(sum_sq / n);

        if (rms > threshold) {  //Comparo la ultima medicion con el humbral configurado, si lo supera, entro
            TickType_t now_t = xTaskGetTickCount(); //Devuelve los ticks del sistema
            uint32_t ms_diff = (now_t - last_trigger) * portTICK_PERIOD_MS; //portTICK_PERIOD_MS ticks -> mseg || Calculo cuantos ms pasaron del utlimo trigger
            if (last_trigger == 0 || ms_diff >= VIGILANTE_COOLDOWN_MS) { 
                last_trigger = now_t;
                uint16_t head_at_trigger = g_buffer.head;
                ESP_LOGW(TAG, "VIBRACION | RMS=%.4fg | head=%d", rms, head_at_trigger);
                if (g_role == NODE_ROLE_SLAVE) {
                    espnow_send_trigger_notify(g_master_mac, rms, now_ms());
                } else {
                    xSemaphoreTake(g_trigger_mutex, portMAX_DELAY);
                    if (g_status != NODE_STATUS_IDLE) {
                        xSemaphoreGive(g_trigger_mutex);
                    } else {
                        g_status = NODE_STATUS_RECORDING;
                        xSemaphoreGive(g_trigger_mutex);
                        g_trigger_slot   = g_current_slot;
                        g_trigger_source = TRIGGER_SOURCE_AUTO;
                        g_trigger_ts     = now_ms();
                        g_trigger_head   = g_buffer.head;
    
                        // Congelar el buffer circular — preserva el pre-trigger exacto
                        // Las ultimas 22 muestras del buffer son el golpe (posiciones 91..112)
                        // Activar post-trigger — sensor_task empieza a llenar g_post_buf
                        g_buffer_frozen = true;
                        g_post_count    = 0;
                        g_post_active   = true;
                    }

                    xEventGroupSetBits(g_eventos, EVT_VIBRACION);              //Activa el bit de tx_task
                    espnow_send_trigger(ESPNOW_BROADCAST_MAC, g_trigger_slot,  //Mando el broadcast
                                        TRIGGER_SOURCE_AUTO, g_trigger_ts);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(VIGILANTE_INTERVAL_MS));
    }
}

static void tx_task(void *pv)
{
    while (1) {
        // Esperar trigger
        EventBits_t bits = xEventGroupWaitBits(g_eventos,
                                                EVT_TRIGGER | EVT_VIBRACION,
                                                pdTRUE, pdFALSE,
                                                pdMS_TO_TICKS(100));
        if (!(bits & (EVT_TRIGGER | EVT_VIBRACION))) continue;

        xSemaphoreTake(g_config_mutex, portMAX_DELAY);
        measurement_config_t cfg = g_config;
        xSemaphoreGive(g_config_mutex);

        uint16_t pre  = 0;
        uint16_t post = 0;
        uint16_t post_ms = 0;
        
        if (g_trigger_source == TRIGGER_SOURCE_MANUAL){
            pre = 0;
            post = ms_to_samples(cfg.manual_duration_ms, SENSOR_SAMPLE_RATE_HZ);
            post_ms = cfg.manual_duration_ms;
        } else {
            pre  = ms_to_samples(cfg.pre_trigger_ms,  SENSOR_SAMPLE_RATE_HZ);
            post = ms_to_samples(cfg.post_trigger_ms, SENSOR_SAMPLE_RATE_HZ);
            post_ms = cfg.post_trigger_ms;
        }

        icm20948_raw_sample_t chunk[STREAM_CHUNK_SAMPLES];

        //MODO STREAM
        if (g_status == NODE_STATUS_STREAMING) {

            // Paso 1 — enviar pre-trigger de a chunks
            uint16_t remaining = pre;
            while (remaining > 0) {
                uint16_t to_read = (remaining < STREAM_CHUNK_SAMPLES)
                                    ? remaining : STREAM_CHUNK_SAMPLES;
                uint16_t got = circ_buffer_pop(&g_buffer, chunk, to_read);
                if (got == 0) break;  // buffer drenado (congelado), no hay mas pre disponible
                if (g_role == NODE_ROLE_SLAVE)
                    slave_client_send_stream_chunk(chunk, got);
                else
                    wifi_manager_send_stream_chunk(chunk, got);
                remaining -= got;
            }

            // Descongelar — sensor_task vuelve a escribir en g_buffer
            g_buffer_frozen = false;
            g_post_active   = false;
            g_post_count    = 0;

            // Paso 2 — post-trigger continuo hasta STOP
            while (1) {
                if (xEventGroupGetBits(g_eventos) & EVT_STOP) {
                    xEventGroupClearBits(g_eventos, EVT_STOP);
                    ESP_LOGI(TAG, "Stream detenido por STOP");
                    break;
                }

                uint16_t got = circ_buffer_pop(&g_buffer, chunk, STREAM_CHUNK_SAMPLES);
                if (got > 0) {
                    if (g_role == NODE_ROLE_SLAVE)
                        slave_client_send_stream_chunk(chunk, got);
                    else
                        wifi_manager_send_stream_chunk(chunk, got);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            g_status = NODE_STATUS_IDLE;

        //MODO GRABACION
        } else {

            g_status = NODE_STATUS_RECORDING;

            measurement_t *m = measurement_alloc(pre + post);
            if (m == NULL) {
                g_post_active   = false;
                g_buffer_frozen = false;
                g_post_count    = 0;
                g_status = NODE_STATUS_IDLE;
                continue;
            }

            memcpy(m->node_mac, g_own_mac, 6);
            m->slot_index     = g_trigger_slot;
            m->trigger_source = g_trigger_source;
            m->timestamp_ms   = g_trigger_ts;
            if (g_trigger_source == TRIGGER_SOURCE_MANUAL) {
                m->pre_trigger_ms  = 0;
                m->post_trigger_ms = cfg.manual_duration_ms;
            } else {
                m->pre_trigger_ms  = cfg.pre_trigger_ms;
                m->post_trigger_ms = cfg.post_trigger_ms;
            }
            m->threshold_g    = cfg.threshold_g;
            m->acel_range     = 1;
            m->sample_rate_hz = SENSOR_SAMPLE_RATE_HZ;
            m->num_samples    = pre + post;
            m->pre_samples    = pre;

            icm20948_raw_sample_t *samples_ptr =
                (icm20948_raw_sample_t *)((uint8_t *)m + offsetof(measurement_t, samples));

            uint16_t pre_got = circ_buffer_pop(&g_buffer, samples_ptr, pre);

            uint32_t deadline = now_ms() + post_ms + 200;
            while (g_post_count < post) {
                if (xEventGroupGetBits(g_eventos) & EVT_STOP) {
                    xEventGroupClearBits(g_eventos, EVT_STOP);
                    ESP_LOGI(TAG, "Grabacion cancelada por STOP");
                    break;
                }
                if (now_ms() > deadline) {
                    ESP_LOGW(TAG, "Post-trigger timeout (%d/%d muestras)", g_post_count, post);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            uint16_t post_got = (g_post_count < post) ? g_post_count : post;
            memcpy(samples_ptr + pre_got, g_post_buf,
                   post_got * sizeof(icm20948_raw_sample_t));

            m->num_samples = pre_got + post_got;

            g_post_active   = false;
            g_buffer_frozen = false;
            g_post_count    = 0;

            xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
            if (g_slots[g_trigger_slot]) measurement_free(g_slots[g_trigger_slot]);
            g_slots[g_trigger_slot] = m;
            xSemaphoreGive(g_slots_mutex);

            sd_save_measurement(m);

            if (g_role == NODE_ROLE_SLAVE && g_auto_sync) {
                esp_err_t ret = slave_client_send_measurement(m);
                if (ret != ESP_OK)
                    ESP_LOGW(TAG, "No se pudo enviar medicion al master");
            }

            g_current_slot = (g_trigger_slot + 1) % MEASUREMENT_SLOTS;

            measurement_summary_t sum = measurement_to_summary(m); //ACTUALEMTE SOLO SIRVE PARA LOG, SE DEBE BORRAR
            ESP_LOGI(TAG, "Grabado slot=%d n=%d RMS=%.3fg PEAK=%.3fg",
                    g_trigger_slot, m->num_samples, sum.rms_g, sum.peak_g);
            
            ESP_LOGI(TAG, "--- MUESTRAS slot=%d ---", g_trigger_slot);
            ESP_LOGI(TAG, "i,tipo,x_g,y_g,z_g");
            icm20948_raw_sample_t *sptr =
                (icm20948_raw_sample_t *)((uint8_t *)m + offsetof(measurement_t, samples));
            // snap = ultimas 22 muestras del pre-trigger = el golpe detectado por el Vigilante
            uint16_t snap_start = (m->pre_samples >= VIGILANTE_WINDOW)
                                ? m->pre_samples - VIGILANTE_WINDOW : 0;
            for (uint16_t i = 0; i < m->num_samples; i++) {
                float x = (float)sptr[i].x / g_sensor.acel_scale;
                float y = (float)sptr[i].y / g_sensor.acel_scale;
                float z = (float)sptr[i].z / g_sensor.acel_scale;
                const char *tipo;
                if (i >= snap_start && i < m->pre_samples)
                    tipo = "snap";
                else if (i < m->pre_samples)
                    tipo = "pre ";
                else
                    tipo = "post";
                ESP_LOGI(TAG, "%d,%s,%.4f,%.4f,%.4f", i, tipo, x, y, z);
            }
        }
        ESP_LOGI(TAG, "--- FIN ---");
        g_status = NODE_STATUS_IDLE; //nodo termino de grabar y volvio a estado de espera
    }
}

static void heartbeat_task(void *pv)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));                        //Espera HEARTBEAT_INTERVAL_MS
        if (g_role != NODE_ROLE_MASTER) continue;                                //Si no es master, no envia heartbeat

        espnow_send_heartbeat(ESPNOW_BROADCAST_MAC, now_ms(), g_current_slot);                   //Manda heartbeat a todos los nodos

        xSemaphoreTake(g_nodes_mutex, portMAX_DELAY);
        for (int i = 0; i < g_node_count; i++) {
            if (now_ms() - g_nodes[i].last_seen_ms > NODE_OFFLINE_TIMEOUT_MS)
                ESP_LOGW(TAG, "Nodo offline: %02x:...:%02x",
                         g_nodes[i].mac[0], g_nodes[i].mac[5]);
        }
        xSemaphoreGive(g_nodes_mutex);
    }   //Despues de enviar el heartbeat, recorre todos los nodos conocidos y verifica cuando fue la ultima vez que respondieron. Si pasaron mas de 15 segundos (`NODE_OFFLINE_TIMEOUT_MS`) sin respuesta, loguea una advertencia.
}

static void button_task(void *pv)
{
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BTN_MASTER),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
    bool last = true;

    while (1) {                                                                //Mientras no se pulsa el boton MASTER
        bool cur = gpio_get_level(PIN_BTN_MASTER);
        if (last && !cur) {
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!gpio_get_level(PIN_BTN_MASTER)) {
                ESP_LOGI(TAG, "Pulsador MASTER");
                g_role = NODE_ROLE_MASTER;
                espnow_send_new_master(ESPNOW_BROADCAST_MAC);
                // Levantar AP WiFi si este nodo era esclavo
                wifi_callbacks_t wcbs = {
                    .on_start      = on_wifi_start,
                    .on_stop       = on_wifi_stop,
                    .on_get_status = on_wifi_get_status,
                    .on_set_config = on_wifi_set_config,
                    .on_get_config = on_wifi_get_config,
                    .on_sync_start = on_wifi_sync_start,
                    .on_sync_stop  = on_wifi_sync_stop,
                    .on_stream_start     = on_wifi_stream_start,
                    .on_stream_stop      = on_wifi_stream_stop,
                    .on_stream_start_all = on_wifi_stream_start_all,
                    .on_stream_stop_all  = on_wifi_stream_stop_all,
                    .on_get_data         = on_wifi_get_data,
                };
                wifi_manager_init(&wcbs);

                slave_server_callbacks_t scbs = {
                    .on_measurement  = on_slave_measurement,
                    .on_stream_chunk = on_slave_stream_chunk,
                };
                slave_server_init(&scbs);

                gpio_set_level(LED_PIN, 1);
            }
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

//----------------------
// DESCUBRIMIENTO DE ROL
//----------------------

static volatile bool s_master_found = false;

static void discovery_hb_cb(const msg_heartbeat_t *msg)
{
    memcpy(g_master_mac, msg->header.src_mac, 6);
    espnow_manager_add_peer(g_master_mac);  // registrar antes del primer trigger_ack
    s_master_found = true;
}

static node_role_t discover_role(void)                                         //Busca el master y devuelve su rol
{
    s_master_found = false;
    ESP_LOGI(TAG, "Buscando master (%dms)...", MASTER_DISCOVERY_MS);
    vTaskDelay(pdMS_TO_TICKS(MASTER_DISCOVERY_MS));

    if (s_master_found) {                                                      
        ESP_LOGI(TAG, "Master: %02x:%02x:%02x:%02x:%02x:%02x",
                 g_master_mac[0], g_master_mac[1], g_master_mac[2],
                 g_master_mac[3], g_master_mac[4], g_master_mac[5]);
        return NODE_ROLE_SLAVE;                                                 //Si se ha encontrado un master, devuelve su rol
    }
    ESP_LOGI(TAG, "Sin master — asumiendo MASTER");
    memcpy(g_master_mac, g_own_mac, 6);                                         //Si no se ha encontrado un master, asume MASTER
    gpio_set_level(LED_PIN, 1);
    return NODE_ROLE_MASTER;
}

//----------------------
// ENTRY POINT
//----------------------

void app_main(void)
{
    // 0. LED
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);
    // 1. NVS
    esp_err_t ret = nvs_flash_init();                                           //Inicializa NVS
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    config_load();
    // 2. Event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());//ni idea - WiFi necesita un sistema de eventos para notificar conexiones, desconexiones, etc. Tiene que crearse antes que el WiFi.
            
    // 3. CS alto antes de SPI
    gpio_config_t cs = {
        .pin_bit_mask = (1ULL << PIN_CS_ICM) | (1ULL << PIN_CS_SD),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs);
    gpio_set_level(PIN_CS_ICM, 1);
    gpio_set_level(PIN_CS_SD,  1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 4. SPI + sensor
    ESP_ERROR_CHECK(spi_bus_init());
    vTaskDelay(pdMS_TO_TICKS(100));
    icm20948_config_t scfg = {
        .spi_host   = ICM_SPI_HOST,
        .pin_cs     = PIN_CS_ICM,
        .acel_range = ICM20948_ACEL_RANGE_4G,
    };
    ESP_ERROR_CHECK(icm20948_init(&scfg, &g_sensor));

    // 4b. Init del SD — delay para que la tarjeta complete el power-up
    vTaskDelay(pdMS_TO_TICKS(100));
    sd_init();

    // 5. Buffer y primitivas de sincronizacion
    circ_buffer_init(&g_buffer);
    g_config_mutex = xSemaphoreCreateMutex();
    g_nodes_mutex  = xSemaphoreCreateMutex();
    g_slots_mutex  = xSemaphoreCreateMutex();
    g_trigger_mutex = xSemaphoreCreateMutex();
    g_eventos      = xEventGroupCreate();

    // 6. WiFi base — ambos nodos lo necesitan para ESP-NOW
    ESP_ERROR_CHECK(wifi_base_init());

    // 7. ESP-NOW — primera pasada solo para descubrimiento
    espnow_callbacks_t ecbs_disc = { .on_heartbeat = discovery_hb_cb };
    ESP_ERROR_CHECK(espnow_manager_init(&ecbs_disc));
    espnow_manager_get_own_mac(g_own_mac);
    
    // 8. Descubrir rol
    g_role = discover_role();
    
    // 9. AP WiFi + servidor TCP — solo el master lo necesita
    if (g_role == NODE_ROLE_MASTER) {
        wifi_callbacks_t wcbs = {
            .on_start      = on_wifi_start,
            .on_stop       = on_wifi_stop,
            .on_get_status = on_wifi_get_status,
            .on_get_config = on_wifi_get_config,
            .on_set_config = on_wifi_set_config,
            .on_sync_start = on_wifi_sync_start,
            .on_sync_stop  = on_wifi_sync_stop,
            .on_stream_start     = on_wifi_stream_start,
            .on_stream_stop      = on_wifi_stream_stop,
            .on_stream_start_all = on_wifi_stream_start_all,
            .on_stream_stop_all  = on_wifi_stream_stop_all,
            .on_get_data         = on_wifi_get_data,
        };
        ESP_ERROR_CHECK(wifi_manager_init(&wcbs));

        slave_server_callbacks_t scbs = {
            .on_measurement = on_slave_measurement, // 
            .on_stream_chunk = on_slave_stream_chunk,
        };
        ESP_ERROR_CHECK(slave_server_init(&scbs));
    } else { // g_role = slave
        ESP_ERROR_CHECK(wifi_slave_connect());
        ESP_ERROR_CHECK(slave_client_init());
    }

    // 10. Registrar callbacks completos
    espnow_callbacks_t ecbs = {
        .on_trigger        = on_espnow_trigger,
        .on_trigger_ack    = on_espnow_trigger_ack,
        .on_trigger_notify = on_espnow_trigger_notify,
        .on_stop           = on_espnow_stop,
        .on_config_ack     = on_espnow_config_ack,
        .on_config         = on_espnow_config,
        .on_heartbeat      = on_espnow_heartbeat,
        .on_heartbeat_ack  = on_espnow_heartbeat_ack,
        .on_new_master     = on_espnow_new_master,
        .on_sync_start     = on_espnow_sync_start,
        .on_sync_stop      = on_espnow_sync_stop,
        .on_stream_start   = on_espnow_stream_start,
        .on_stream_stop    = on_espnow_stream_stop,
        .on_data_request   = on_espnow_data_request,
    };
    espnow_manager_update_callbacks(&ecbs);

    // 11. Lanzar tareas
    xTaskCreatePinnedToCore(sensor_task,    "sensor",    4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(vigilante_task, "vigilante", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(tx_task,        "tx",        8192, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(button_task,    "button",    2048, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "━━━ Sistema iniciado ━━━");
    ESP_LOGI(TAG, "MAC:  %02x:%02x:%02x:%02x:%02x:%02x",
             g_own_mac[0], g_own_mac[1], g_own_mac[2],
             g_own_mac[3], g_own_mac[4], g_own_mac[5]);
    ESP_LOGI(TAG, "Rol:  %s", g_role == NODE_ROLE_MASTER ? "MASTER" : "SLAVE");
    ESP_LOGI(TAG, "AP:   %s  Puerto: %d", WIFI_AP_SSID, WIFI_TCP_PORT);
    ESP_LOGI(TAG, "Cfg:  umbral=%.2fg pre=%dms post=%dms",
             g_config.threshold_g,
             g_config.pre_trigger_ms,
             g_config.post_trigger_ms);
    ESP_LOGI(TAG, "SD:   %s", s_sd_ready ? "montada" : "no disponible");
}