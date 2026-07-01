# Hardware
---

## Componentes

| Componente | Descripción | Notas |
|---|---|---|
| **ESP32 / ESP32-S3 N16R8** | Microcontrolador principal | S3 recomendado por PSRAM y mayor velocidad de CPU |
| **ICM-20948** | IMU 9 ejes (acelerómetro + giroscopio + magnetómetro) por SPI | Solo se usa el acelerómetro en este proyecto |
| **Tarjeta microSD** | Almacenamiento local de mediciones | Opcional — el sistema funciona sin ella |

---

## Pinout

### ICM-20948 — SPI2 (bus del sensor)

| Señal ESP32 | GPIO | Señal ICM-20948 |
|---|---|---|
| MOSI | 23 | SDA |
| MISO | 19 | SDO |
| CLK  | 18 | SCL |
| CS   | 5  | CS  |
| 3.3V | —  | VCC |
| GND  | —  | GND |

### Tarjeta SD — SPI3

| Señal ESP32 | GPIO | Señal SD |
|---|---|---|
| MOSI | 13 | DI  |
| MISO | 27 | DO  |
| CLK  | 14 | CLK |
| CS   | 15 | CS  |
| 3.3V | —  | VCC |
| GND  | —  | GND |

### General

| Función | GPIO |
|---|---|
| LED indicador de rol master | 2 |
| Pulsador cambio de master | 0 |

> El GPIO 0 es el pin de boot del ESP32. Al pulsar durante el arranque se entra en modo flash. En operación normal no hay conflicto porque `button_task` solo detecta pulsaciones largas, no el flanco inicial.

---

## Diagrama de conexión

```
                    ┌─────────────┐
                    │   ESP32     │
                    │             │
          GPIO 23 ──┤ MOSI  ──────┼──► ICM-20948 SDA
          GPIO 19 ──┤ MISO  ◄─────┼─── ICM-20948 SDO
          GPIO 18 ──┤ CLK   ──────┼──► ICM-20948 SCL
          GPIO  5 ──┤ CS1   ──────┼──► ICM-20948 CS
                    │             │
          GPIO 13 ──┤ MOSI  ──────┼──► SD DI
          GPIO 27 ──┤ MISO  ◄─────┼─── SD DO
          GPIO 14 ──┤ CLK   ──────┼──► SD CLK
          GPIO 15 ──┤ CS2   ──────┼──► SD CS
                    │             │
          GPIO  2 ──┤ LED         │
          GPIO  0 ──┤ BTN  ───────┼─── Pulsador → GND
                    └─────────────┘
```

Los dos buses SPI son independientes (SPI2 para el sensor, SPI3 para la SD). Pueden operar simultáneamente sin conflicto.

---

## Configuración del ICM-20948

| Parámetro | Valor configurado |
|---|---|
| ODR (Output Data Rate) | 225 Hz |
| Rango del acelerómetro | Configurable: ±2g / ±4g / ±8g / ±16g |
| Interfaz | SPI modo 0 |
| Lectura | Por FIFO — se lee en ráfaga cada 50 ms |
| Ejes usados | X, Y, Z (acelerómetro) |

El driver lee el FIFO en bloques cada 50 ms (`sensor_task`). A 225 Hz en 50 ms se acumulan ~11 muestras por lectura. Se usa FIFO en lugar de lectura individual para evitar perder muestras si la tarea se retrasa ligeramente.

---

## ESP32 vs ESP32-S3

| Característica | ESP32 (base) | ESP32-S3 N16R8 |
|---|---|---|
| Flash | 4 MB (típico) | 16 MB |
| PSRAM | No | 8 MB (Octal SPI) |
| CPU | Xtensa LX6 240 MHz | Xtensa LX7 240 MHz (~40% más rápido/ciclo) |
| Slots de medición | En RAM interna (ajustado) | En PSRAM (sin impacto en RAM interna) |
| Recomendado | Pruebas / nodo simple | Producción / master con muchos nodos |

Con ESP32 base sin PSRAM, `measurement_alloc()` usa `malloc()` normal sobre la RAM interna. Con 5 slots de ~2 KB cada uno son ~10 KB adicionales compitiendo con el heap del sistema. Es funcional pero el margen es más estrecho.

---

## Consumo estimado

| Estado | Consumo aproximado |
|---|---|
| Idle (WiFi AP activo) | ~100 mA |
| Grabación activa | ~120 mA |
| Stream activo | ~130 mA |

> Valores orientativos a 3.3V. No se han medido con instrumentación — son estimaciones basadas en datasheets de Espressif para modo APSTA con WiFi activo.
