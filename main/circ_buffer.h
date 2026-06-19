#ifndef CIRC_BUFFER_H
#define CIRC_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "icm20948.h"

// CONFIGURACION

// 2 segundos a 225 Hz = 450 muestras
// Cada muestra ocupa 6 bytes (int16 x 3 ejes) → 2.700 bytes en RAM
#define CIRC_BUFFER_CAPACITY    900

// TIPOS

typedef struct{
    icm20948_raw_sample_t   data[CIRC_BUFFER_CAPACITY];
    uint16_t                head;   // indice donde se escribe la proxima muestra
    uint16_t                tail;   // indice de la muestra mas antigua
    uint16_t                count;  // muestras validas actualmente en el buffer
    SemaphoreHandle_t       mutex;  // protege acceso concurrente Core0/Core1
} circ_buffer_t;

// Inicializa el buffer y crea el mutex FreeRTOS
// Debe llamarse antes de lanzar las tareas que lo usan
void     circ_buffer_init(circ_buffer_t *buf);

// Escribe N muestras en el buffer
// En modo overwrite: si no hay espacio, avanza tail (descarta muestras viejas)
// Thread-safe: puede llamarse desde cualquier tarea/core
void     circ_buffer_push(circ_buffer_t *buf,
                          const icm20948_raw_sample_t *samples,
                          uint16_t count);

// Lee hasta max_count muestras del buffer y las copia en out
// Devuelve la cantidad de muestras efectivamente leidas (puede ser 0)
// Thread-safe: puede llamarse desde cualquier tarea/core
uint16_t circ_buffer_pop(circ_buffer_t *buf,
                         icm20948_raw_sample_t *out,
                         uint16_t max_count);

// Devuelve cuantas muestras hay disponibles en este momento
// Util para decidir cuando hay suficientes datos para transmitir
uint16_t circ_buffer_count(circ_buffer_t *buf);

// Devuelve true si el buffer esta lleno
bool     circ_buffer_is_full(circ_buffer_t *buf);

// Descarta todas las muestras sin leerlas
void     circ_buffer_clear(circ_buffer_t *buf);

// Lee las ultimas N muestras SIN consumirlas (no mueve tail)
// Util para el Vigilante: inspecciona sin interferir con tx_task
// Devuelve cuantas muestras se copiaron (puede ser menor a N si el buffer tiene menos)
uint16_t circ_buffer_peek_last(circ_buffer_t *buf,
                                icm20948_raw_sample_t *out,
                                uint16_t n);

#endif // CIRC_BUFFER_H