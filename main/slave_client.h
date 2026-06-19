#ifndef SLAVE_CLIENT_H
#define SLAVE_CLIENT_H

#include <stdint.h>
#include "esp_err.h"
#include "measurement.h"
#include "icm20948.h"

// API

// Inicializa el cliente TCP hacia el master
// Lanza la tarea de conexion con reconexion automatica.
esp_err_t slave_client_init(void);

// Envia una medicion completa al master por TCP.
// Bloquea hasta que se envian todos los bytes o falla la conexion.
esp_err_t slave_client_send_measurement(const measurement_t *meas);

// Envia un chunk de stream al master por TCP.
// Bloquea hasta que se envian todos los bytes o falla la conexion.
esp_err_t slave_client_send_stream_chunk(const icm20948_raw_sample_t *samples,
                                          uint16_t count);

#endif // SLAVE_CLIENT_H