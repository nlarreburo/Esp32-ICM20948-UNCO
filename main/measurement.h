#ifndef MEASUREMENT_H
#define MEASUREMENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "icm20948.h"


// CONFIGURACION POR DEFECTO


#define CONFIG_DEFAULT_PRE_MS       500
#define CONFIG_DEFAULT_POST_MS      1000
#define CONFIG_DEFAULT_MANUAL_MS    2000
#define CONFIG_DEFAULT_THRESHOLD_G  1.5f


// TRIGGER SOURCE


#define TRIGGER_SOURCE_AUTO    0   // el Vigilante detecto vibracion
#define TRIGGER_SOURCE_MANUAL  1   // la PC envio CMD_START


// CONFIGURACION DE MEDICION

// La PC envia esta struct al master via WiFi TCP.
// El master la propaga a todos los nodos via ESP-NOW (MSG_CONFIG).
// Se persiste en NVS para sobrevivir reinicios.

typedef struct {
    uint16_t pre_trigger_ms;     // ms a capturar antes del trigger (del buffer circular)
    uint16_t post_trigger_ms;    // ms a capturar despues del trigger (en tiempo real)
    uint16_t manual_duration_ms; // ms a capturar cuando la PC inicia manualmente
    float    threshold_g;        // umbral del Vigilante en g
} __attribute__((packed)) measurement_config_t;


// RESUMEN DE MEDICION — viaja por ESP-NOW

//
// Generado al finalizar cada medicion. Viaja del nodo al master por ESP-NOW
// y del master a la PC por WiFi como notificacion de evento.
// Tamano: 24 bytes — entra holgadamente en un paquete ESP-NOW (limite 250 bytes)

typedef struct {
    uint8_t  node_mac[6];       // MAC del nodo origen (identidad unica)
    uint8_t  slot_index;        // slot donde quedo guardada la medicion
    uint8_t  trigger_source;    // TRIGGER_SOURCE_AUTO o TRIGGER_SOURCE_MANUAL
    uint32_t timestamp_ms;      // ms desde arranque del sistema cuando ocurrio el trigger
    uint16_t num_samples;       // total de muestras capturadas (pre + post)
    float    rms_g;             // RMS de aceleracion del evento
    float    peak_g;            // valor pico absoluto registrado
} __attribute__((packed)) measurement_summary_t;


// MEDICION COMPLETA — vive en PSRAM

//
// Alocada dinamicamente en PSRAM con heap_caps_malloc (MALLOC_CAP_SPIRAM).
// Nunca viaja por ESP-NOW. La PC la solicita por WiFi TCP con CMD_GET_DATA.
//
// Uso de memoria:
//   Header fijo:   ~30 bytes
//   Por muestra:   6 bytes (int16 x 3 ejes)
//
//   Ejemplo con config por defecto (pre=500ms, post=1000ms, ODR=225Hz):
//     pre:   113 muestras
//     post:  225 muestras
//     total: 338 muestras

typedef struct {
    //Identidad
    uint8_t  node_mac[6];       // MAC del nodo que capturo la medicion
    uint8_t  slot_index;        // posicion en el vector de slots
    uint8_t  trigger_source;    // TRIGGER_SOURCE_AUTO o TRIGGER_SOURCE_MANUAL

    //Tim
    uint32_t timestamp_ms;      // ms desde arranque cuando ocurrio el trigger

    //Configuracion usada en esta medicion
    //Se guarda en el header para que la PC sepa como interpretar los datos
    //aunque la config haya cambiado despues
    uint16_t pre_trigger_ms;
    uint16_t post_trigger_ms;
    float    threshold_g;       // umbral que estaba activo al momento del trigger
    float    rms_trigger;       // RMS que disparo el evento (0 si fue manual)

    //Parametros del sensor
    uint8_t  acel_range;        // icm20948_acel_range_t: 0=2g 1=4g 2=8g 3=16g
    uint16_t sample_rate_hz;    // ODR efectivo en Hz

    //Dimensiones
    uint16_t num_samples;       // total de muestras = pre_samples + post_samples
    uint16_t pre_samples;       // cuantas muestras son pre-trigger

    //Datos crudos
    icm20948_raw_sample_t samples[];

} __attribute__((packed)) measurement_t;


// ESTADO DE UN NODO EN LA RED


typedef enum {
    NODE_ROLE_SLAVE  = 0,
    NODE_ROLE_MASTER = 1,
} node_role_t;

typedef enum {
    NODE_STATUS_IDLE      = 0,  // en espera
    NODE_STATUS_RECORDING = 1,  // grabando medicion activa
    NODE_STATUS_STREAMING = 2,  // stream en vivo hacia la PC
} node_status_t;

typedef struct {
    uint8_t       mac[6];           // identificador unico del nodo
    node_role_t   role;             // SLAVE o MASTER
    node_status_t status;           // IDLE, RECORDING o STREAMING
    uint32_t      last_seen_ms;     // timestamp del ultimo heartbeat recibido
                                    // si (now - last_seen_ms) > 15000 → nodo offline
    uint8_t       current_slot;     // slot activo actualmente en ese nodo
    uint8_t       config_sync;      //1 o 0 para saber si esta sincronizada la configuracion
} node_state_t;


// API


// Aloca una medicion en PSRAM con espacio para num_samples muestras.
// En ESP32 DevKit usa malloc normal (sin PSRAM).
// En ESP32-S3 usa heap_caps_malloc con MALLOC_CAP_SPIRAM.
// Devuelve NULL si no hay memoria suficiente.
measurement_t *measurement_alloc(uint16_t num_samples);

// Libera la memoria alocada por measurement_alloc
void measurement_free(measurement_t *m);

// Calcula el tamano total en bytes de una medicion con n muestras
// Util para saber cuanta PSRAM se va a usar antes de alocar
size_t measurement_size(uint16_t num_samples);

// Genera el resumen para enviar por ESP-NOW a partir de una medicion completa
measurement_summary_t measurement_to_summary(const measurement_t *m);

// Convierte ms a numero de muestras segun el ODR configurado
// Ejemplo: ms_to_samples(500, 225) = 112
uint16_t ms_to_samples(uint16_t ms, uint16_t sample_rate_hz);

#endif // MEASUREMENT_H