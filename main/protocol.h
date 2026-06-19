#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "measurement.h"

#define WIFI_TCP_PORT       5000
#define SLAVE_TCP_PORT      5001

#define CMD_GET_STATUS      "CMD_GET_STATUS"
#define CMD_SET_CONFIG      "CMD_SET_CONFIG"
#define CMD_START           "CMD_START"
#define CMD_STOP            "CMD_STOP"
#define CMD_GET_DATA        "CMD_GET_DATA"
#define CMD_STREAM_START     "CMD_STREAM_START"
#define CMD_STREAM_STOP      "CMD_STREAM_STOP"
#define CMD_STREAM_START_ALL "CMD_STREAM_START_ALL"
#define CMD_STREAM_STOP_ALL  "CMD_STREAM_STOP_ALL"


#define RESP_OK             "OK"
#define RESP_ERROR          "ERROR"
#define RESP_STATUS         "STATUS"
#define RESP_DATA_START     "DATA_START"
#define RESP_DATA_END       "DATA_END"
#define RESP_STREAM_START   "STREAM_START"
#define RESP_STREAM_END     "STREAM_END"

#define RESP_EVENT_TRIGGER  "EVENT TRIGGER"

#define CMD_SYNC_START   "CMD_SYNC_START"
#define CMD_SYNC_STOP    "CMD_SYNC_STOP"

// TCP
typedef enum {
    SLAVE_PKT_HANDSHAKE         = 0X01, // esclavo → master: identificacion (MAC)
    SLAVE_PKT_HANDSHAKE_ACK     = 0x02,  // master → esclavo: confirmacion
    SLAVE_PKT_MEASUREMENT       = 0x03,  // esclavo → master: medicion completa
    SLAVE_PKT_STREAM_CHUNK      = 0x04,  // esclavo → master: chunk de stream
} slave_pkt_type_t;

typedef struct {
    uint8_t type; // slave_pkt_type_t
    uint32_t length; // bytes del payload
} __attribute__((packed)) slave_pkt_header_t;

typedef struct {
    uint8_t mac[6];
} __attribute__((packed)) slave_pkt_handshake_t;

// ESP-NOW
typedef enum {
    //Control de medicion
    MSG_TRIGGER         = 0x01,  // Master → todos: iniciar grabacion
    MSG_TRIGGER_ACK     = 0x02,  // Nodo   → Master: confirme el trigger
    MSG_TRIGGER_NOTIFY  = 0x03,  // Nodo   → Master: deteccion automatica (Vigilante)
    MSG_STOP            = 0x04,  // Master → todos: detener grabacion activa
    MSG_STOP_ACK        = 0x05,  // Nodo   → Master: detuve la grabacion

    //Configuracion
    MSG_CONFIG          = 0x10,  // Master → todos: nueva configuracion de medicion
    MSG_CONFIG_ACK      = 0x11,  // Nodo   → Master: configuracion aplicada

    //Heartbeat
    MSG_HEARTBEAT       = 0x20,  // Master → todos: ¿estas vivo?
    MSG_HEARTBEAT_ACK   = 0x21,  // Nodo   → Master: si, estoy vivo + estado

    //Solicidar datos
    MSG_DATA_REQUEST = 0x30,

    //Stream
    MSG_STREAM_START = 0x70,  // Master → nodo: activar modo stream
    MSG_STREAM_STOP  = 0x71,  // Master → nodo: desactivar modo stream

    //Gestion de red 
    MSG_NEW_MASTER      = 0x50,  // Nodo   → todos: yo soy el nuevo master
    MSG_NEW_MASTER_ACK  = 0x51,  // Nodo   → nuevo master: te reconozco como master
    
    //Gestion de sincronizacion
    MSG_SYNC_START      = 0X60,  // Master → todos: comenzar a enviar mediciones por TCP
    MSG_SYNC_STOP       = 0X61,  // Master → todos: dejar de enviar mediciones por TCP
} esp_now_msg_type_t;

//MESSAGE FRAMING - header + payload
typedef struct {
    uint8_t msg_type;    // esp_now_msg_type_t
    uint8_t src_mac[6];  // MAC del remitente
    uint8_t reserved;    // padding para alineacion a 8 bytes
} __attribute__((packed)) msg_header_t;

typedef struct {
    msg_header_t header;
    uint8_t      slot_index;      // en que slot debe guardar la medicion
    uint8_t      trigger_source;  // TRIGGER_SOURCE_AUTO o TRIGGER_SOURCE_MANUAL
    uint32_t     timestamp_ms;    // timestamp del master al momento del trigger
                                  // los nodos lo copian en su measurement_t
                                  // para tener referencia temporal comun
} __attribute__((packed)) msg_trigger_t;

typedef struct {
    msg_header_t header;
    uint8_t      slot_index;     // slot que esta grabando (eco del trigger)
    uint32_t     timestamp_ms;   // timestamp local del nodo al recibir el trigger
                                 // la diferencia con trigger.timestamp_ms es
                                 // la latencia de red — util para calibracion
} __attribute__((packed)) msg_trigger_ack_t;

typedef struct {
    msg_header_t header;
    float        rms_g;          // RMS que disparo la deteccion
    uint32_t     timestamp_ms;   // cuando ocurrio la deteccion
} __attribute__((packed)) msg_trigger_notify_t;

typedef struct {
    msg_header_t header;
    // sin payload adicional — el tipo de mensaje es suficiente
} __attribute__((packed)) msg_stop_t;

typedef struct {
    msg_header_t header;
    uint8_t      status;   // 0=grabacion cancelada, 1=ya habia terminado, 2=estaba idle
} __attribute__((packed)) msg_stop_ack_t;

typedef struct {
    msg_header_t         header;
    measurement_config_t config;   // pre_ms, post_ms, manual_ms, threshold_g
} __attribute__((packed)) msg_config_t;

typedef struct {
    msg_header_t header;
    uint8_t      ok;   // 1=config aplicada, 0=error (log en el nodo)
} __attribute__((packed)) msg_config_ack_t;

typedef struct {
    msg_header_t header;
    uint32_t     timestamp_ms;   // timestamp del master — los nodos lo usan
                                 // para detectar si el master se reinicio
} __attribute__((packed)) msg_heartbeat_t;

typedef struct {
    msg_header_t  header;
    node_status_t status;        // IDLE, RECORDING o STREAMING
    uint8_t       current_slot;  // slot activo actualmente
    uint32_t      free_heap;     // bytes libres en RAM/PSRAM — util para debug
} __attribute__((packed)) msg_heartbeat_ack_t;

typedef struct {
    msg_header_t header;
    // src_mac del header ya identifica al nuevo master
} __attribute__((packed)) msg_new_master_t;

typedef struct {
    msg_header_t header;
    // src_mac del header identifica al nodo que reconoce al nuevo master
} __attribute__((packed)) msg_new_master_ack_t;

typedef struct {
    msg_header_t header;
} __attribute__((packed)) msg_sync_start_t;

typedef struct {
    msg_header_t header;
} __attribute__((packed)) msg_sync_stop_t;

typedef struct {
    msg_header_t header;
} __attribute__((packed)) msg_stream_start_t;

typedef struct {
    msg_header_t header;
} __attribute__((packed)) msg_stream_stop_t;

//Header binario para stream TCP master→PC
#define STREAM_PKT_CHUNK  0x01

typedef struct {
    uint8_t  type;      // STREAM_PKT_CHUNK
    uint8_t  mac[6];    // MAC del nodo origen
    uint16_t count;     // numero de muestras
} __attribute__((packed)) stream_chunk_hdr_t;

//Header binario para medidas 
#define MEAS_PKT_DATA  0x02

typedef struct {
    uint8_t  type;      // MEAS_PKT_DATA
    uint8_t  mac[6];    // MAC del nodo origen
    uint8_t  slot;      // slot de la medicion
    uint32_t length;    // bytes del payload que siguen
} __attribute__((packed)) meas_pkt_hdr_t;

//Solicitar datos
typedef struct {
    msg_header_t header;
    uint8_t      slot_index;
} __attribute__((packed)) msg_data_request_t;

#endif // PROTOCOL_H