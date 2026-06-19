#ifndef SLAVE_SERVER_H
#define SLAVE_SERVER_H

#include <stdint.h>
#include "esp_err.h"
#include "measurement.h"
#include "icm20948.h"

//CALLBACKS - notificaciones hacia main.c

//Llamado cuando se recibe una medicion de un esclavo
typedef void (*slave_cb_measurement_t)( const uint8_t *mac, 
                                        uint8_t slot, 
                                        const measurement_t *meas);
//Llamado cuando se recibe un chunk de stream de un esclavo
typedef void (*slave_cb_stream_chunk_t)(const uint8_t *mac, 
                                        const icm20948_raw_sample_t *samples, 
                                        uint16_t count);

typedef struct {
    slave_cb_measurement_t on_measurement;
    slave_cb_stream_chunk_t on_stream_chunk;
} slave_server_callbacks_t;

//API
esp_err_t slave_server_init(const slave_server_callbacks_t *callbacks);

#endif