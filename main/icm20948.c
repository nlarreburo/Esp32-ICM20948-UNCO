#include "icm20948.h"
#include "esp_log.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ICM20948";


// CAPA DE TRANSPORTE SPI (privada)

//
// Protocolo ICM-20948 (DS-000189 Rev 1.6, seccion 6.5):
//
//   Escritura: CS_LOW → [0|reg][data]         → CS_HIGH
//   Lectura:   CS_LOW → [1|reg][dummy x N]    → CS_HIGH  (sensor responde durante los dummy)
//   Burst:     CS_LOW → [1|0x72][dummy x N]   → CS_HIGH  (FIFO_R_W auto-incrementa)

static esp_err_t icm20948_read_reg(icm20948_handle_t *handle,
                                    uint8_t reg_addr,
                                    uint8_t *data,
                                    size_t len)
{
    uint8_t tx_buf[len + 1];
    uint8_t rx_buf[len + 1];

    memset(tx_buf, 0x00, sizeof(tx_buf));
    tx_buf[0] = ICM20948_SPI_READ_BIT | reg_addr;

    spi_transaction_t t = {
        .length    = (len + 1) * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };

    esp_err_t ret = spi_device_polling_transmit(handle->spi_dev, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error SPI leyendo reg 0x%02X: %s", reg_addr, esp_err_to_name(ret));
        return ret;
    }

    // rx_buf[0] es basura (respuesta al byte de comando), datos utiles desde rx_buf[1]
    memcpy(data, rx_buf + 1, len);
    return ESP_OK;
}

static esp_err_t icm20948_write_reg(icm20948_handle_t *handle,
                                     uint8_t reg_addr,
                                     uint8_t data)
{
    uint8_t tx_buf[2];
    tx_buf[0] = ICM20948_SPI_WRITE_BIT | reg_addr;
    tx_buf[1] = data;

    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx_buf,
        .rx_buffer = NULL,
    };

    esp_err_t ret = spi_device_polling_transmit(handle->spi_dev, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error SPI escribiendo reg 0x%02X: %s", reg_addr, esp_err_to_name(ret));
    }
    return ret;
}


// UTILIDADES (privadas)


static float get_acel_scale_factor(icm20948_acel_range_t range)
{
    switch (range) {
        case ICM20948_ACEL_RANGE_2G:  return 16384.0f;
        case ICM20948_ACEL_RANGE_4G:  return 8192.0f;
        case ICM20948_ACEL_RANGE_8G:  return 4096.0f;
        case ICM20948_ACEL_RANGE_16G: return 2048.0f;
        default:                       return 16384.0f;
    }
}


// API


esp_err_t icm20948_select_bank(icm20948_handle_t *handle, uint8_t bank)
{
    uint8_t bank_value = (bank & 0x03) << 4;
    return icm20948_write_reg(handle, ICM20948_REG_BANK_SEL, bank_value);
}

esp_err_t icm20948_test(icm20948_handle_t *handle)
{
    uint8_t who_am_i = 0;

    esp_err_t ret = icm20948_select_bank(handle, 0);
    if (ret != ESP_OK) return ret;

    ret = icm20948_read_reg(handle, ICM20948_WHO_AM_I, &who_am_i, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer WHO_AM_I");
        return ret;
    }

    if (who_am_i != 0xEA) {
        ESP_LOGE(TAG, "WHO_AM_I = 0x%02X, se esperaba 0xEA", who_am_i);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "ICM-20948 detectado (WHO_AM_I=0xEA)");
    return ESP_OK;
}

esp_err_t icm20948_init(icm20948_config_t *config, icm20948_handle_t *handle)
{
    if (config == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&handle->config, config, sizeof(icm20948_config_t));
    handle->acel_scale = get_acel_scale_factor(config->acel_range);

    // 1. Registrar dispositivo en el bus SPI 
    spi_device_interface_config_t dev_cfg = {
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
        .mode           = ICM20948_SPI_MODE,
        .clock_speed_hz = ICM20948_SPI_CLOCK_HZ,
        .spics_io_num   = config->pin_cs,
        .queue_size     = 1,
        .pre_cb         = NULL,
        .post_cb        = NULL,
    };

    esp_err_t ret = spi_bus_add_device(config->spi_host, &dev_cfg, &handle->spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error registrando dispositivo SPI: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Verificar conexion 
    ret = icm20948_test(handle);
    if (ret != ESP_OK) return ret;

    // 3. Reset completo 
    ret = icm20948_select_bank(handle, 0);
    if (ret != ESP_OK) return ret;

    ret = icm20948_write_reg(handle, ICM20948_PWR_MGMT_1, 0x80);  // DEVICE_RESET
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(100));

    // 4. Salir de sleep, auto-seleccionar clock 
    ret = icm20948_write_reg(handle, ICM20948_PWR_MGMT_1, 0x01);
    if (ret != ESP_OK) return ret;

    // 5. Giroscopio OFF, acelerometro ON 
    // PWR_MGMT_2: bits [5:3]=DISABLE_ACCEL_XYZ, bits [2:0]=DISABLE_GYRO_XYZ
    // 0x07 = 0b00000111 → accel ON, gyro OFF
    ret = icm20948_write_reg(handle, ICM20948_PWR_MGMT_2, ICM20948_GYRO_DISABLE_ALL);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(100));

    // 6. Configurar acelerometro (Banco 2)
    ret = icm20948_select_bank(handle, 2);
    if (ret != ESP_OK) return ret;

    // ACCEL_CONFIG: bits [5:3]=DLPFCFG, bits [2:1]=rango, bit 0=ACCEL_FCHOICE
    // ACCEL_FCHOICE=1 habilita el DLPF
    // DLPFCFG=1 → corte a 111.4 Hz — justo por debajo de Nyquist (225/2 = 112.5 Hz)
    // DLPFCFG=0 (anterior) cortaba a 246 Hz, por encima de Nyquist → aliasing posible
    uint8_t acel_config = (1 << 3) | (config->acel_range << 1) | 0x01;
    ret = icm20948_write_reg(handle, ICM20948_ACCEL_CONFIG, acel_config);
    if (ret != ESP_OK) return ret;

    // ODR = 1125 Hz / (1 + SMPLRT_DIV) → SMPLRT_DIV=4 → 225 Hz
    ret = icm20948_write_reg(handle, ICM20948_ACCEL_SMPLRT_DIV_1, 0x00);
    if (ret != ESP_OK) return ret;
    ret = icm20948_write_reg(handle, ICM20948_ACCEL_SMPLRT_DIV_2, 0x04);
    if (ret != ESP_OK) return ret;

    // 7. Configurar FIFO (Banco 0)
    ret = icm20948_select_bank(handle, 0);
    if (ret != ESP_OK) return ret;

    // Reset del FIFO: 0x1F activa el reset, 0x00 lo libera
    ret = icm20948_write_reg(handle, ICM20948_FIFO_RST, 0x1F);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));
    ret = icm20948_write_reg(handle, ICM20948_FIFO_RST, 0x00);
    if (ret != ESP_OK) return ret;

    // FIFO_MODE = 0x00: modo stream (sobreescribe muestras viejas al llenarse)
    ret = icm20948_write_reg(handle, ICM20948_FIFO_MODE, 0x00);
    if (ret != ESP_OK) return ret;

    // USER_CTRL: FIFO_EN (bit6) + I2C_IF_DIS (bit4) = 0x50
    // I2C_IF_DIS es necesario para forzar SPI exclusivo y evitar datos corruptos
    ret = icm20948_write_reg(handle, ICM20948_USER_CTRL, ICM20948_USER_CTRL_SPI_FIFO);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // FIFO_EN_2 debe escribirse DESPUES de USER_CTRL
    // Si se escribe antes, el reset interno del FIFO al activarse USER_CTRL lo borra
    ret = icm20948_write_reg(handle, ICM20948_FIFO_EN_2, ICM20948_FIFO_EN2_ACCEL);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "ICM-20948 listo | Rango: +/-%dg | ODR: ~225 Hz | Giroscopio: OFF | FIFO: ON",
             (1 << (config->acel_range + 1)));

    return ESP_OK;
}

esp_err_t icm20948_read_acel(icm20948_handle_t *handle, icm20948_acel_data_t *data)
{
    if (handle == NULL || data == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = icm20948_select_bank(handle, 0);
    if (ret != ESP_OK) return ret;

    uint8_t raw[6];
    ret = icm20948_read_reg(handle, ICM20948_ACEL_XOUT_H, raw, 6);
    if (ret != ESP_OK) return ret;

    int16_t rx = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ry = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t rz = (int16_t)((raw[4] << 8) | raw[5]);

    data->x = (float)rx / handle->acel_scale;
    data->y = (float)ry / handle->acel_scale;
    data->z = (float)rz / handle->acel_scale;

    return ESP_OK;
}

esp_err_t icm20948_fifo_reset(icm20948_handle_t *handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = icm20948_select_bank(handle, 0);
    if (ret != ESP_OK) return ret;

    ret = icm20948_write_reg(handle, ICM20948_FIFO_RST, 0x1F);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(5));
    ret = icm20948_write_reg(handle, ICM20948_FIFO_RST, 0x00);

    return ret;
}

esp_err_t icm20948_read_fifo(icm20948_handle_t *handle,
                              icm20948_raw_sample_t *samples_out,
                              uint16_t *samples_read)
{
    if (handle == NULL || samples_out == NULL || samples_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_read = 0;

    //  1. Leer el contador del FIFO 
    // El contador es de 13 bits: [12:8] en FIFO_COUNTH, [7:0] en FIFO_COUNTL
    esp_err_t ret = icm20948_select_bank(handle, 0);
    if (ret != ESP_OK) return ret;

    uint8_t count_bytes[2];
    ret = icm20948_read_reg(handle, ICM20948_FIFO_COUNTH, count_bytes, 2);
    if (ret != ESP_OK) return ret;

    uint16_t fifo_count = ((uint16_t)(count_bytes[0] & 0x1F) << 8) | count_bytes[1];

    if (fifo_count == 0) {
        return ESP_OK;  // FIFO vacio, no es error
    }

    //  2. Verificar alineacion 
    // Si fifo_count no es multiplo de 6 el FIFO esta desalineado (overflow)
    if (fifo_count % ICM20948_SAMPLE_SIZE_BYTES != 0) {
        ESP_LOGW(TAG, "FIFO desalineado (%d bytes). Reseteando...", fifo_count);
        icm20948_fifo_reset(handle);
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t num_samples = fifo_count / ICM20948_SAMPLE_SIZE_BYTES;

    if (num_samples > ICM20948_FIFO_MAX_SAMPLES) {
        ESP_LOGW(TAG, "FIFO: %d muestras, limitando a %d", num_samples, ICM20948_FIFO_MAX_SAMPLES);
        num_samples = ICM20948_FIFO_MAX_SAMPLES;
    }

    // 3. Burst read: leer todo el bloque en una sola transaccion SPI 
    uint8_t raw_buffer[ICM20948_FIFO_MAX_BYTES];
    uint16_t bytes_to_read = num_samples * ICM20948_SAMPLE_SIZE_BYTES;

    ret = icm20948_read_reg(handle, ICM20948_FIFO_R_W, raw_buffer, bytes_to_read);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error en burst read del FIFO (%d bytes): %s",
                 bytes_to_read, esp_err_to_name(ret));
        return ret;
    }

    // 4. Parsear bytes crudos → array de muestras
    // Formato FIFO: [X_H][X_L][Y_H][Y_L][Z_H][Z_L] por muestra, big-endian
    for (uint16_t i = 0; i < num_samples; i++) {
        uint8_t *p = &raw_buffer[i * ICM20948_SAMPLE_SIZE_BYTES];
        samples_out[i].x = (int16_t)((p[0] << 8) | p[1]);
        samples_out[i].y = (int16_t)((p[2] << 8) | p[3]);
        samples_out[i].z = (int16_t)((p[4] << 8) | p[5]);
    }

    *samples_read = num_samples;
    return ESP_OK;
}