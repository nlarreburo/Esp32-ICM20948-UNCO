#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "protocol.h"
#include "measurement.h"

//----------------------
// CONFIGURACION
//----------------------

// Tamaño de la cola de recepcion — cuantos mensajes pueden acumularse
// antes de que espnow_rx_task los procese.
// Si la cola se llena, los mensajes nuevos se descartan.
#define ESPNOW_RX_QUEUE_SIZE    16

// Timeout de envio — cuanto esperar confirmacion de entrega (ms)
#define ESPNOW_SEND_TIMEOUT_MS  100

// Canal WiFi — debe coincidir con WIFI_AP_CHANNEL en wifi_manager.h
// ESP-NOW y WiFi AP deben operar en el mismo canal
#define ESPNOW_CHANNEL          6

// Direccion de broadcast — envia a todos los peers registrados
static const uint8_t ESPNOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

//----------------------
// CALLBACKS DE RECEPCION
//----------------------
//
// El modulo que inicializa espnow_manager registra estos callbacks.
// Se invocan desde espnow_rx_task cuando llega un mensaje del tipo correspondiente.
// Cada callback recibe el mensaje ya casteado al tipo correcto.

typedef void (*espnow_cb_trigger_t)        (const msg_trigger_t *msg);
typedef void (*espnow_cb_trigger_ack_t)    (const msg_trigger_ack_t *msg);
typedef void (*espnow_cb_trigger_notify_t) (const msg_trigger_notify_t *msg);
typedef void (*espnow_cb_stop_t)           (const msg_stop_t *msg);
typedef void (*espnow_cb_stop_ack_t)       (const msg_stop_ack_t *msg);
typedef void (*espnow_cb_config_ack_t)     (const msg_config_ack_t *msg);
typedef void (*espnow_cb_config_t)         (const msg_config_t *msg);
typedef void (*espnow_cb_heartbeat_t)      (const msg_heartbeat_t *msg);
typedef void (*espnow_cb_heartbeat_ack_t)  (const msg_heartbeat_ack_t *msg);
typedef void (*espnow_cb_new_master_t)     (const msg_new_master_t *msg);
typedef void (*espnow_cb_new_master_ack_t) (const msg_new_master_ack_t *msg);
typedef void (*espnow_cb_sync_start_t)     (const msg_sync_start_t *msg);
typedef void (*espnow_cb_sync_stop_t)      (const msg_sync_stop_t *msg);
typedef void (*espnow_cb_stream_start_t)   (const msg_stream_start_t *msg);
typedef void (*espnow_cb_stream_stop_t)    (const msg_stream_stop_t *msg);
typedef void (*espnow_cb_data_request_t)   (const msg_data_request_t *msg);

typedef struct {
    espnow_cb_trigger_t         on_trigger;
    espnow_cb_trigger_ack_t     on_trigger_ack;
    espnow_cb_trigger_notify_t  on_trigger_notify;
    espnow_cb_stop_t            on_stop;
    espnow_cb_stop_ack_t        on_stop_ack;
    espnow_cb_config_ack_t      on_config_ack;
    espnow_cb_config_t          on_config;
    espnow_cb_heartbeat_t       on_heartbeat;
    espnow_cb_heartbeat_ack_t   on_heartbeat_ack;
    espnow_cb_new_master_t      on_new_master;
    espnow_cb_new_master_ack_t  on_new_master_ack;
    espnow_cb_sync_start_t      on_sync_start;
    espnow_cb_sync_stop_t       on_sync_stop;
    espnow_cb_stream_start_t    on_stream_start;
    espnow_cb_stream_stop_t     on_stream_stop;
    espnow_cb_data_request_t    on_data_request;
} espnow_callbacks_t;

//----------------------
// API — inicializacion
//----------------------

// Inicializa ESP-NOW, registra callbacks internos y lanza espnow_rx_task
// Debe llamarse despues de esp_wifi_start()
// La MAC propia se obtiene automaticamente con esp_wifi_get_mac()
esp_err_t espnow_manager_init(const espnow_callbacks_t *callbacks);

// Actualiza los callbacks sin reinicializar ESP-NOW
// Usar cuando ESP-NOW ya esta inicializado y solo se necesita
// cambiar las funciones de despacho — por ejemplo despues del
// descubrimiento de rol en app_main
void espnow_manager_update_callbacks(const espnow_callbacks_t *callbacks);

// Obtiene la MAC propia del nodo (cargada en init)
// util para completar el campo src_mac de los mensajes salientes
void espnow_manager_get_own_mac(uint8_t mac_out[6]);

//----------------------
// API — gestion de peers
//----------------------
//
// ESP-NOW requiere registrar cada destinatario como "peer" antes de enviarle
// La direccion de broadcast esta registrada por defecto en init()

// Registra un nodo como peer. Si ya existe, no hace nada
esp_err_t espnow_manager_add_peer(const uint8_t mac[6]);

// Elimina un peer. Usado cuando un nodo se detecta como offline
esp_err_t espnow_manager_remove_peer(const uint8_t mac[6]);

// Devuelve true si la MAC esta registrada como peer
bool espnow_manager_peer_exists(const uint8_t mac[6]);

//----------------------
// API — envio de mensajes
//----------------------

// Cada funcion construye el mensaje, completa src_mac con la MAC propia
// y lo envia por ESP-NOW

// Para broadcast: pasar ESPNOW_BROADCAST_MAC como dst_mac.
// Para unicast:   pasar la MAC del destinatario

// Todas devuelven ESP_OK si el mensaje fue entregado al driver ESP-NOW.
// La confirmacion de entrega al peer se maneja internamente

//Control de medicion

esp_err_t espnow_send_trigger(const uint8_t dst_mac[6],
                               uint8_t slot_index,
                               uint8_t trigger_source,
                               uint32_t timestamp_ms);

esp_err_t espnow_send_trigger_ack(const uint8_t dst_mac[6],
                                   uint8_t slot_index,
                                   uint32_t timestamp_ms);

esp_err_t espnow_send_trigger_notify(const uint8_t dst_mac[6],
                                      float rms_g,
                                      uint32_t timestamp_ms);

esp_err_t espnow_send_stop(const uint8_t dst_mac[6]);

esp_err_t espnow_send_stop_ack(const uint8_t dst_mac[6], uint8_t status);

//Configuracion

esp_err_t espnow_send_config(const uint8_t dst_mac[6],
                              const measurement_config_t *config);

esp_err_t espnow_send_config_ack(const uint8_t dst_mac[6], uint8_t ok);

//Heartbeat

esp_err_t espnow_send_heartbeat(const uint8_t dst_mac[6], uint32_t timestamp_ms, uint8_t current_slot);

esp_err_t espnow_send_heartbeat_ack(const uint8_t dst_mac[6],
                                     node_status_t status,
                                     uint8_t current_slot,
                                     uint32_t free_heap);

//Stream
esp_err_t espnow_send_stream_start(const uint8_t dst_mac[6]);
esp_err_t espnow_send_stream_stop(const uint8_t dst_mac[6]);

//Funcion enviar datos
esp_err_t espnow_send_data_request(const uint8_t dst_mac[6], uint8_t slot_index);

//Gestion de red

esp_err_t espnow_send_new_master(const uint8_t dst_mac[6]);
esp_err_t espnow_send_new_master_ack(const uint8_t dst_mac[6]);

//Sincronizacion
esp_err_t espnow_send_sync_start(const uint8_t dst_mac[6]);
esp_err_t espnow_send_sync_stop(const uint8_t dst_mac[6]);

#endif // ESPNOW_MANAGER_H