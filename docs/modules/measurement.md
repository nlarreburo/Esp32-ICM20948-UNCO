# measurement

Gestiona la estructura de datos central del sistema: la medición completa. Provee alloc en PSRAM, conversión de unidades y cálculo de RMS/peak.

---

## Estructuras principales

### `measurement_t` — medición completa

Vive en PSRAM (o RAM interna en ESP32 base). Nunca viaja por ESP-NOW — es demasiado grande. La PC la solicita por TCP con `CMD_GET_DATA`.

```c
typedef struct {
    uint8_t  node_mac[6];       // nodo que capturó la medición
    uint8_t  slot_index;        // posición en g_slots[]
    uint8_t  trigger_source;    // TRIGGER_SOURCE_AUTO o TRIGGER_SOURCE_MANUAL
    uint32_t timestamp_ms;      // ms desde arranque al momento del trigger
    uint16_t pre_trigger_ms;    // ventana pre configurada al grabar
    uint16_t post_trigger_ms;   // ventana post configurada al grabar
    float    threshold_g;       // umbral activo al momento del trigger
    float    rms_trigger;       // RMS que disparó el evento (0 si fue manual)
    uint8_t  acel_range;        // 0=±2g  1=±4g  2=±8g  3=±16g
    uint16_t sample_rate_hz;    // ODR efectivo (nominalmente 225)
    uint16_t num_samples;       // total = pre_samples + post_samples
    uint16_t pre_samples;       // cuántas muestras son pre-trigger
    icm20948_raw_sample_t samples[];  // array flexible — 6 bytes por muestra
} __attribute__((packed)) measurement_t;
```

Tamaño total: `31 bytes fijos + num_samples × 6 bytes`

### `measurement_summary_t` — resumen compacto

24 bytes. Diseñado para viajar por ESP-NOW. Actualmente solo se usa para logging local.

```c
typedef struct {
    uint8_t  node_mac[6];
    uint8_t  slot_index;
    uint8_t  trigger_source;
    uint32_t timestamp_ms;
    uint16_t num_samples;
    float    rms_g;
    float    peak_g;
} __attribute__((packed)) measurement_summary_t;
```

### `measurement_config_t` — parámetros configurables

```c
typedef struct {
    uint16_t pre_trigger_ms;
    uint16_t post_trigger_ms;
    uint16_t manual_duration_ms;
    float    threshold_g;
} __attribute__((packed)) measurement_config_t;
```

Viaja por ESP-NOW en `MSG_CONFIG`. Se almacena en `g_config` protegido por semáforo.

---

## API

```c
measurement_t *measurement_alloc(uint16_t num_samples);
```
Aloca una medición en PSRAM si `CONFIG_SPIRAM` está habilitado, en RAM interna si no. Inicializa todo a cero. Devuelve `NULL` si no hay memoria.

```c
void measurement_free(measurement_t *m);
```
Libera la memoria. Llama a `free()` — funciona tanto para PSRAM como RAM interna.

```c
size_t measurement_size(uint16_t num_samples);
```
Devuelve el tamaño total en bytes de una medición con `num_samples` muestras. Útil para calcular el `length` del paquete TCP antes de enviar.

```c
measurement_summary_t measurement_to_summary(const measurement_t *m);
```
Calcula RMS y peak sobre todas las muestras de la medición:

```
RMS  = sqrt( sum(x² + y² + z²) / n )   — energía promedio
peak = max( sqrt(x² + y² + z²) )        — muestra de mayor magnitud
```

Ambos valores en `g`, usando el `acel_range` del header para la conversión. Actualmente solo se usa para logging — no se envía a la PC al finalizar la grabación.

```c
uint16_t ms_to_samples(uint16_t ms, uint16_t sample_rate_hz);
```
Convierte milisegundos a número de muestras con redondeo hacia arriba:

```
ms_to_samples(500, 225) = ceil(500 × 225 / 1000) = 113
```

---

## Factores de escala

Para convertir `int16` crudo a `g`:

| `acel_range` | Rango | Factor |
|---|---|---|
| 0 | ±2g  | 16384 LSB/g |
| 1 | ±4g  | 8192 LSB/g  |
| 2 | ±8g  | 4096 LSB/g  |
| 3 | ±16g | 2048 LSB/g  |

```
valor_g = muestra_int16 / factor
```

---

## Notas

- El array `samples[]` es un **flexible array member** de C99 — debe estar al final del struct y el struct debe alocarse dinámicamente con espacio suficiente para las muestras. No puede declararse como variable local en el stack.
- `measurement_to_summary()` itera todas las muestras con operaciones de punto flotante. En ESP32 con FPU hardware el tiempo es despreciable (~1 ms para 338 muestras).
