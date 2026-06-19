#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "protocol.h"

//----------------------
// CONFIGURACION DEL AP WIFI
//----------------------

#define WIFI_AP_SSID        "BRIDGE_MONITOR"   // nombre de la red WiFi
#define WIFI_AP_PASSWORD    "monitor123"        // contraseña (min 8 chars)
#define WIFI_AP_CHANNEL     6                   // canal WiFi (1-13)
#define WIFI_AP_MAX_CONN    5                   // solo una PC conectada a la vez
#define WIFI_AP_IP          "192.168.4.1"       // IP fija del master en el AP

//----------------------
// CALLBACKS — el wifi_manager llama a estas funciones cuando recibe comandos
//----------------------
//
// El modulo que inicializa wifi_manager registra estos callbacks para
// manejar cada comando sin que wifi_manager necesite saber nada del
// resto del sistema.
//
// Cada callback recibe los parametros del comando ya parseados y
// devuelve esp_err_t. Si devuelve ESP_OK, wifi_manager responde "OK\n".
// Si devuelve ESP_FAIL, wifi_manager responde "ERROR <motivo>\n".

typedef esp_err_t (*wifi_cb_start_t)(void);
typedef esp_err_t (*wifi_cb_stop_t)(void);
typedef esp_err_t (*wifi_cb_get_status_t)(char *out_json, size_t max_len);
typedef esp_err_t (*wifi_cb_set_config_t)(const measurement_config_t *config);
typedef esp_err_t (*wifi_cb_sync_start_t)(void);
typedef esp_err_t (*wifi_cb_sync_stop_t)(void);
typedef esp_err_t (*wifi_cb_stream_start_t)(const uint8_t *mac);
typedef esp_err_t (*wifi_cb_stream_stop_t)(const uint8_t *mac);
typedef esp_err_t (*wifi_cb_stream_start_all_t)(void);
typedef esp_err_t (*wifi_cb_stream_stop_all_t)(void);
typedef esp_err_t (*wifi_cb_get_data_t)(const uint8_t *mac, uint8_t slot, int fd);

typedef struct {
    wifi_cb_start_t          on_start;
    wifi_cb_stop_t           on_stop;
    wifi_cb_get_status_t     on_get_status;
    wifi_cb_set_config_t     on_set_config;
    wifi_cb_sync_start_t     on_sync_start;
    wifi_cb_sync_stop_t      on_sync_stop;
    wifi_cb_stream_start_t   on_stream_start;
    wifi_cb_stream_stop_t    on_stream_stop;
    wifi_cb_stream_start_all_t on_stream_start_all;
    wifi_cb_stream_stop_all_t  on_stream_stop_all;
    wifi_cb_get_data_t       on_get_data;
} wifi_callbacks_t;

//----------------------
// API
//----------------------

// Inicializa el driver WiFi en modo AP sin arrancar el servidor TCP.
// Necesario en AMBOS nodos (master y esclavo) porque ESP-NOW requiere
// que el WiFi este inicializado antes de poder usarse.
// Debe llamarse despues de esp_event_loop_create_default().
esp_err_t wifi_base_init(void);

// Inicializa el AP WiFi y arranca la tarea del servidor TCP socket.
// Solo debe llamarse en el MASTER despues de wifi_base_init().
// Los callbacks deben estar configurados antes de llamar a esta funcion.
esp_err_t wifi_manager_init(const wifi_callbacks_t *callbacks);

// Envia un mensaje espontaneo a la PC si hay una conexion activa.
// Usado por el master para notificar EVENT TRIGGER sin que la PC lo pida.
// Thread-safe — puede llamarse desde cualquier tarea.
// Si no hay PC conectada, el mensaje se descarta silenciosamente.
esp_err_t wifi_manager_send_event(const char *event_str);

// Devuelve true si hay una PC conectada al socket TCP actualmente.
bool wifi_manager_is_connected(void);

esp_err_t wifi_manager_send_raw(const void *data, size_t len);
esp_err_t wifi_manager_send_stream_chunk(const icm20948_raw_sample_t *samples, uint16_t count);

// Conecta el esclavo al AP del master en modo STA.
// Solo debe llamarse en esclavos, despues de wifi_base_init().
esp_err_t wifi_slave_connect(void);

#endif // WIFI_MANAGER_H