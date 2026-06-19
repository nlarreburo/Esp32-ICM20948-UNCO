#ifndef ICM20948_H
#define ICM20948_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

// SPI — configuracion del bus

// Modo SPI 0 (CPOL=0, CPHA=0): verificado en hardware con este modulo GY-ICM20948V2
// Datasheet indica soporte para Mode 0 y Mode 3. Mode 3 no funciono en pruebas.
#define ICM20948_SPI_CLOCK_HZ       1000000     // 1 MHz — conservador para cables jumper
#define ICM20948_SPI_MODE           0           // CPOL=0, CPHA=0

// Convenciones del protocolo SPI del ICM-20948 (DS-000189 Rev 1.6, seccion 6.5):
// Bit 7 del primer byte = 1 → lectura, 0 → escritura
#define ICM20948_SPI_READ_BIT       (1 << 7)
#define ICM20948_SPI_WRITE_BIT      (0 << 7)

// REGISTROS — USER BANK 0 (DS-000189 Rev 1.6, seccion 7.1)

#define ICM20948_WHO_AM_I           0x00    // Debe devolver 0xEA
#define ICM20948_USER_CTRL          0x03    // Control general
#define ICM20948_LP_CONFIG          0x05    // Configuracion bajo consumo
#define ICM20948_PWR_MGMT_1         0x06    // Gestion de energia 1
#define ICM20948_PWR_MGMT_2         0x07    // Gestion de energia 2
#define ICM20948_INT_PIN_CFG        0x0F
#define ICM20948_INT_ENABLE         0x10
#define ICM20948_INT_ENABLE_1       0x11
#define ICM20948_INT_ENABLE_2       0x12
#define ICM20948_INT_ENABLE_3       0x13

// Datos del acelerometro (Bank 0)
#define ICM20948_ACEL_XOUT_H        0x2D
#define ICM20948_ACEL_XOUT_L        0x2E
#define ICM20948_ACEL_YOUT_H        0x2F
#define ICM20948_ACEL_YOUT_L        0x30
#define ICM20948_ACEL_ZOUT_H        0x31
#define ICM20948_ACEL_ZOUT_L        0x32

// Temperatura (Bank 0)
#define ICM20948_TEMP_OUT_H         0x39
#define ICM20948_TEMP_OUT_L         0x3A

// FIFO (Bank 0) — direcciones verificadas contra datasheet tabla 7.1
#define ICM20948_FIFO_EN_1          0x66    // Habilita slaves I2C en el FIFO (no usado)
#define ICM20948_FIFO_EN_2          0x67    // Bit4=ACCEL, Bit3=GYRO_Z, Bit2=GYRO_Y, Bit1=GYRO_X
#define ICM20948_FIFO_RST           0x68    // Reset del FIFO (escribir 0x1F luego 0x00)
#define ICM20948_FIFO_MODE          0x69    // Modo FIFO: 0=stream, 1=snapshot
#define ICM20948_FIFO_COUNTH        0x70    // Contador de bytes disponibles [bits 12:8]
#define ICM20948_FIFO_COUNTL        0x71    // Contador de bytes disponibles [bits  7:0]
#define ICM20948_FIFO_R_W           0x72    // Puerto de lectura del FIFO

// Selector de banco (comun a todos los bancos)
#define ICM20948_REG_BANK_SEL       0x7F


// REGISTROS — USER BANK 2 (DS-000189 Rev 1.6, seccion 7.3)

#define ICM20948_GYRO_SMPLRT_DIV    0x00
#define ICM20948_GYRO_CONFIG_1      0x01
#define ICM20948_GYRO_CONFIG_2      0x02
#define ICM20948_ACCEL_SMPLRT_DIV_1 0x10    // Divisor ODR acelerometro [bits 11:8]
#define ICM20948_ACCEL_SMPLRT_DIV_2 0x11    // Divisor ODR acelerometro [bits  7:0]
#define ICM20948_ACCEL_CONFIG       0x14    // Rango y DLPF del acelerometro


// MASCARAS DE BITS

// USER_CTRL (0x03)
#define ICM20948_USER_CTRL_FIFO_EN  (1 << 6)  // Bit 6: habilita el FIFO
#define ICM20948_USER_CTRL_I2C_DIS  (1 << 4)  // Bit 4: deshabilita I2C, fuerza SPI exclusivo
#define ICM20948_USER_CTRL_SPI_FIFO (ICM20948_USER_CTRL_FIFO_EN | ICM20948_USER_CTRL_I2C_DIS) // 0x50

// FIFO_EN_2 (0x67)
#define ICM20948_FIFO_EN2_ACCEL     (1 << 4)  // Bit 4: datos del acelerometro al FIFO

// PWR_MGMT_2 (0x07)
// Bits [5:3] = DISABLE_ACCEL_XYZ | Bits [2:0] = DISABLE_GYRO_XYZ
// 0x07 = 0b00000111: acelerometro ON (bits 5:3 = 0), giroscopio OFF (bits 2:0 = 1)
#define ICM20948_GYRO_DISABLE_ALL   0x07

// CONSTANTES DEL FIFO

// FIFO de 512 bytes. Con giroscopio OFF cada muestra son 6 bytes (X, Y, Z en int16)
// 512 / 6 = 85 muestras completas como maximo
#define ICM20948_FIFO_MAX_BYTES         512
#define ICM20948_SAMPLE_SIZE_BYTES      6
#define ICM20948_FIFO_MAX_SAMPLES       (ICM20948_FIFO_MAX_BYTES / ICM20948_SAMPLE_SIZE_BYTES)  // 85

// TIPOS DE DATOS

typedef enum {
    ICM20948_ACEL_RANGE_2G  = 0,    // +/-2g
    ICM20948_ACEL_RANGE_4G  = 1,    // +/-4g — recomendado para estructuras civiles
    ICM20948_ACEL_RANGE_8G  = 2,    // +/-8g
    ICM20948_ACEL_RANGE_16G = 3     // +/-16g
} icm20948_acel_range_t;

typedef struct {
    spi_host_device_t       spi_host;
    int                     pin_cs;
    icm20948_acel_range_t   acel_range;
} icm20948_config_t;

// Muestra en float (unidades g) — para lectura individual
typedef struct {
    float x;
    float y;
    float z;
} icm20948_acel_data_t;

// Muestra cruda int16 — para el FIFO y el buffer circular
// 6 bytes por muestra vs 12 del float: reduce a la mitad el uso de RAM
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} icm20948_raw_sample_t;

// Handle interno del driver
typedef struct {
    icm20948_config_t   config;
    float               acel_scale;
    spi_device_handle_t spi_dev;
} icm20948_handle_t;

// API

// Inicializa el sensor: SPI, reset, clock, giroscopio OFF, ODR 225 Hz, FIFO ON
esp_err_t icm20948_init(icm20948_config_t *config, icm20948_handle_t *handle);

// Verifica conexion leyendo WHO_AM_I (debe devolver 0xEA)
esp_err_t icm20948_test(icm20948_handle_t *handle);

// Cambia el banco de registros activo (0-3)
esp_err_t icm20948_select_bank(icm20948_handle_t *handle, uint8_t bank);

// Lee una sola muestra directo de los registros, convertida a float en g
// Solo para verificacion puntual. En produccion usar icm20948_read_fifo()
esp_err_t icm20948_read_acel(icm20948_handle_t *handle, icm20948_acel_data_t *data);

// Lee todas las muestras disponibles del FIFO en una sola rafaga SPI
// samples_out : buffer destino, espacio para ICM20948_FIFO_MAX_SAMPLES elementos
// samples_read: cantidad de muestras leidas (0 si el FIFO estaba vacio)
esp_err_t icm20948_read_fifo(icm20948_handle_t *handle,
                              icm20948_raw_sample_t *samples_out,
                              uint16_t *samples_read);

// Descarta todos los datos pendientes del FIFO
esp_err_t icm20948_fifo_reset(icm20948_handle_t *handle);

#endif // ICM20948_H