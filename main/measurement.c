#include "measurement.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "string.h"
#include "math.h"

static const char *TAG = "MEASUREMENT";


// DETECCION DE PSRAM

//
// En ESP32 DevKit (sin PSRAM): heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0
// En ESP32-S3 N16R8 (con PSRAM): devuelve los bytes disponibles en PSRAM
//
// Esta funcion centraliza la decision de donde alojar para que el resto del
// codigo no necesite saber en que hardware esta corriendo.

static void *measurement_malloc(size_t size)
{
    void *ptr = NULL;

#if CONFIG_SPIRAM
    // ESP32-S3 con PSRAM habilitada en menuconfig
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (ptr == NULL) {
        ESP_LOGW(TAG, "PSRAM insuficiente (%d bytes), intentando RAM interna", size);
        ptr = malloc(size);
    }
#else
    // ESP32 DevKit sin PSRAM — usamos RAM interna
    ptr = malloc(size);
#endif

    return ptr;
}


// API


size_t measurement_size(uint16_t num_samples)
{
    // Header fijo + array de muestras contiguo en memoria
    return sizeof(measurement_t) + num_samples * sizeof(icm20948_raw_sample_t);
}

measurement_t *measurement_alloc(uint16_t num_samples)
{
    if (num_samples == 0) {
        ESP_LOGE(TAG, "num_samples no puede ser 0");
        return NULL;
    }

    size_t size = measurement_size(num_samples);
    measurement_t *m = (measurement_t *)measurement_malloc(size);

    if (m == NULL) {
        ESP_LOGE(TAG, "Sin memoria para medicion de %d muestras (%d bytes)",
                 num_samples, size);
        return NULL;
    }

    // Inicializar todo en cero — evita datos basura en el header
    memset(m, 0, size);
    m->num_samples = num_samples;

    ESP_LOGW(TAG, "Medicion alojadada: %d muestras, %d bytes", num_samples, size);
    return m;
}

void measurement_free(measurement_t *m)
{
    if (m == NULL) return;
    free(m);
}

measurement_summary_t measurement_to_summary(const measurement_t *m)
{
    measurement_summary_t summary = {0};

    if (m == NULL) return summary;

    // Copiar campos de identidad y timing
    memcpy(summary.node_mac, m->node_mac, 6);
    summary.slot_index    = m->slot_index;
    summary.trigger_source = m->trigger_source;
    summary.timestamp_ms  = m->timestamp_ms;
    summary.num_samples   = m->num_samples;

    // Calcular RMS y pico sobre todas las muestras
    // RMS = sqrt( mean(x^2 + y^2 + z^2) ) — energia total de la vibracion
    // peak = max( sqrt(x^2 + y^2 + z^2) ) — peor muestra individual
    //
    // Necesitamos la escala para convertir int16 a g.
    // La guardamos en acel_range dentro del header.
    float scale;
    switch (m->acel_range) {
        case 0:  scale = 16384.0f; break;  // +/-2g
        case 1:  scale =  8192.0f; break;  // +/-4g
        case 2:  scale =  4096.0f; break;  // +/-8g
        case 3:  scale =  2048.0f; break;  // +/-16g
        default: scale =  8192.0f; break;
    }

    float sum_sq = 0.0f;
    float peak   = 0.0f;

    for (uint16_t i = 0; i < m->num_samples; i++) {
        float x = (float)m->samples[i].x / scale;
        float y = (float)m->samples[i].y / scale;
        float z = (float)m->samples[i].z / scale;

        float mag_sq = x*x + y*y + z*z;
        sum_sq += mag_sq;

        float mag = sqrtf(mag_sq);
        if (mag > peak) peak = mag;
    }

    summary.rms_g  = sqrtf(sum_sq / m->num_samples);
    summary.peak_g = peak;

    return summary;
}

uint16_t ms_to_samples(uint16_t ms, uint16_t sample_rate_hz)
{
    if (sample_rate_hz == 0) return 0;
    // Redondeo hacia arriba: si no es multiplo exacto, agregamos una muestra
    // Ejemplo: 500 ms a 225 Hz = 112.5 → 113 muestras
    return (uint16_t)(((uint32_t)ms * sample_rate_hz + 999) / 1000);
}