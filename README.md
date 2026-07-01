# Sistema de Medición de Vibraciones — ESP32

Sistema distribuido de captura de vibraciones basado en ESP32. Permite coordinar múltiples nodos sensores (esclavos) bajo un nodo central (master) que expone una interfaz TCP hacia una PC de monitoreo.

---

## Características

- Detección automática de vibraciones por umbral RMS configurable
- Captura sincronizada de pre-trigger y post-trigger en todos los nodos
- Hasta 8 nodos en red simultáneos
- Stream en vivo de muestras hacia la PC
- Almacenamiento de mediciones en tarjeta SD
- Rol master/esclavo autodescubierto al arranque
- Cambio de master en caliente mediante pulsador

---

## Hardware requerido

| Componente | Descripción |
|---|---|
| ESP32 / ESP32-S3 N16R8 | Microcontrolador (con PSRAM para mayor capacidad de slots) |
| ICM-20948 | Acelerómetro/giroscopio 9 ejes por SPI |
| Tarjeta microSD | Almacenamiento local de mediciones (opcional) |

### Pinout

#### ICM-20948 — SPI2 (bus del sensor)

| Señal | GPIO |
|---|---|
| MOSI | 23 |
| MISO | 19 |
| CLK | 18 |
| CS | 5 |

#### Tarjeta SD — SPI3

| Señal | GPIO |
|---|---|
| MOSI | 13 |
| MISO | 27 |
| CLK | 14 |
| CS | 15 |

#### General

| Señal | GPIO |
|---|---|
| LED (master) | 2 |
| Pulsador (cambio de master) | 0 |

---

## Arquitectura del sistema

```
PC ──── TCP/5000 ────► MASTER ◄──── ESP-NOW ────► ESCLAVO(s)
                          ▲                             │
                     TCP/5001 ───────────────────────────┘
                     (datos pesados)
```

- **ESP-NOW** — señalización entre nodos (triggers, heartbeats, configuración). Máximo 250 bytes por paquete.
- **TCP/5000** — comandos de la PC al master (texto) y datos binarios del master a la PC.
- **TCP/5001** — transferencia de mediciones completas y stream del esclavo al master.

### Roles

| Rol | Comportamiento |
|---|---|
| **Master** | Levanta AP WiFi `BRIDGE_MONITOR`. Expone TCP/5000 a la PC y TCP/5001 para esclavos. Coordina triggers y heartbeats. |
| **Esclavo** | Se conecta al AP del master. Captura muestras y las envía al master por TCP/5001. |

El rol se autodescubre al arranque: el nodo espera 10 segundos buscando heartbeats del master. Si no encuentra ninguno, asume el rol master.

### Tareas FreeRTOS

| Tarea | Core | Prioridad | Función |
|---|---|---|---|
| `sensor_task` | 1 | 5 | Lee el FIFO del ICM-20948 cada 50ms y llena el buffer circular |
| `vigilante_task` | 1 | 4 | Calcula RMS cada 100ms y dispara trigger si supera el umbral |
| `tx_task` | 0 | 3 | Procesa trigger: arma medición (pre+post) o envía stream |
| `heartbeat_task` | 0 | 2 | Master envía ping cada 5s y detecta nodos offline |
| `button_task` | 0 | 1 | Detecta pulsación para cambio de master |
| `espnow_rx_task` | 0 | 2 | Despacha mensajes ESP-NOW entrantes |
| `server_task` / `client_task` | 0 | 3 | Servidor TCP/5000 (master) |
| `slave_server_task` | 0 | 3 | Servidor TCP/5001 (master) |
| `slave_client_task` | 0 | 3 | Cliente TCP/5001 (esclavo) |

---

## Estructura del repositorio

```
├── main/
│   ├── main.c              — punto de entrada, lógica central, callbacks
│   ├── protocol.h          — definición de todos los mensajes ESP-NOW y TCP
│   ├── measurement.h/.c    — struct de medición, alloc en PSRAM, cálculo de RMS/peak
│   ├── circ_buffer.h/.c    — buffer circular thread-safe para muestras pre-trigger
│   ├── icm20948.h/.c       — driver del sensor ICM-20948 por SPI
│   ├── espnow_manager.h/.c — capa ESP-NOW: envío, recepción, gestión de peers
│   ├── wifi_manager.h/.c   — AP WiFi, servidor TCP/5000, comandos de la PC
│   ├── slave_client.h/.c   — cliente TCP/5001 (corre en esclavo)
│   └── slave_server.h/.c   — servidor TCP/5001 (corre en master)
├── unity-app/              — tests unitarios
├── CMakeLists.txt
└── sdkconfig               — configuración de ESP-IDF (generado por menuconfig)
```

---

## Configuración

Los parámetros principales están en `main/main.c`:

```c
// Sensor
#define SENSOR_SAMPLE_RATE_HZ   225     // ODR del ICM-20948

// Vigilante (detección automática)
#define VIGILANTE_WINDOW        22      // muestras analizadas por ciclo
#define VIGILANTE_THRESHOLD_G   1.5f   // umbral en g
#define VIGILANTE_COOLDOWN_MS   2000   // tiempo mínimo entre triggers
#define VIGILANTE_INTERVAL_MS   100    // intervalo de análisis

// Red
#define MASTER_DISCOVERY_MS     10000  // tiempo buscando master al arranque
#define HEARTBEAT_INTERVAL_MS   5000   // intervalo de heartbeat del master
#define NODE_OFFLINE_TIMEOUT_MS 15000  // tiempo para marcar nodo como offline
#define MAX_NODES               8      // máximo de esclavos simultáneos
#define MEASUREMENT_SLOTS       5      // slots de medición en PSRAM/RAM

// Captura
#define POST_TRIGGER_MAX_SAMPLES 900   // máximo de muestras post-trigger (~4s)
#define STREAM_CHUNK_SAMPLES    45     // muestras por chunk en modo stream
```

Los parámetros de medición configurables desde la PC (por defecto):

| Parámetro | Valor | Descripción |
|---|---|---|
| `pre_trigger_ms` | 500 ms | Ventana antes del trigger (del buffer circular) |
| `post_trigger_ms` | 1000 ms | Ventana después del trigger |
| `manual_duration_ms` | 2000 ms | Duración al disparar manualmente |
| `threshold_g` | 1.5 g | Umbral del vigilante (en RMS, ver abajo) |

> **RMS (Root Mean Square):** magnitud que representa la energía promedio de la vibración. Se calcula como la raíz cuadrada del promedio de (x²+y²+z²) sobre un conjunto de muestras. A diferencia de leer un solo eje, el RMS vectorial captura el movimiento en las tres direcciones simultáneamente. Un valor alto indica vibración intensa.
>
> **Peak:** valor pico — la muestra individual con mayor magnitud vectorial dentro de toda la medición. Indica el instante de mayor intensidad del golpe.

### Frecuencia de muestreo y filtro anti-aliasing

El sensor toma **225 muestras por segundo** (225 Hz). Esto significa que el sistema puede capturar correctamente vibraciones de hasta **112.5 Hz** — la mitad de la frecuencia de muestreo. Este límite se llama frecuencia de Nyquist.

**¿Qué pasa si hay vibraciones por encima de 112.5 Hz?**
Si el sensor recibe una vibración más rápida de lo que puede muestrear, no la ignora — la registra como una frecuencia incorrecta más baja (efecto llamado *aliasing*). Esto contaminaría los datos con valores falsos.

Para evitarlo, el sensor tiene un **filtro paso-bajo** interno que atenúa todo lo que supere los 111.4 Hz antes de pasar las muestras al sistema. Las vibraciones de interés en aplicaciones típicas (impactos, maquinaria) están por debajo de 50 Hz, bien dentro del rango capturable.

| | Valor |
|---|---|
| Frecuencia de muestreo (ODR) | 225 Hz |
| Frecuencia máxima capturable (Nyquist) | 112.5 Hz |
| Corte del filtro interno (DLPF) | 111.4 Hz |

### WiFi

| Parámetro | Valor |
|---|---|
| SSID | `BRIDGE_MONITOR` |
| Contraseña | `monitor123` |
| IP del master | `192.168.4.1` |
| Puerto PC↔Master | `5000` |
| Puerto Esclavo↔Master | `5001` |
| Canal ESP-NOW | `6` |

---

## Compilar y flashear

Requiere ESP-IDF instalado. Desde la raíz del proyecto:

```bash
idf.py build
idf.py -p COMx flash monitor
```

Para habilitar PSRAM (ESP32-S3), verificar en `sdkconfig`:
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
```

---

## Protocolo TCP — comandos de la PC

Todos los comandos se envían como texto plano terminado en `\n` al puerto **5000**.

### Comandos de texto (PC → master)

| Comando | Descripción |
|---|---|
| `CMD_GET_STATUS` | Devuelve JSON con rol y estado de todos los nodos |
| `CMD_START` | Dispara grabación manual en todos los nodos |
| `CMD_STOP` | Detiene grabación o stream activo |
| `CMD_SET_CONFIG PRE=ms POST=ms MANUAL=ms THR=g` | Actualiza parámetros de medición |
| `CMD_GET_DATA MAC=xx:xx:xx:xx:xx:xx SLOT=n` | Solicita medición completa de un nodo |
| `CMD_STREAM_START MAC=xx:xx:xx:xx:xx:xx` | Inicia stream en vivo de un nodo |
| `CMD_STREAM_STOP` | Detiene el stream activo |
| `CMD_STREAM_START_ALL` | Inicia stream en vivo en todos los nodos |
| `CMD_STREAM_STOP_ALL` | Detiene todos los streams |
| `CMD_SYNC_START` | Activa envío automático de mediciones al terminar |
| `CMD_SYNC_STOP` | Desactiva envío automático |

### Respuestas de texto (master → PC)

| Respuesta | Descripción |
|---|---|
| `OK` | Comando ejecutado correctamente |
| `ERROR <motivo>` | Error al ejecutar el comando |
| `STATUS <json>` | Respuesta a `CMD_GET_STATUS` |
| `EVENT TRIGGER MAC=... SLOT=n RMS=x.xxx` | Notificación espontánea de trigger automático |

### Datos binarios (master → PC)

La PC detecta el tipo por el primer byte del paquete:

| Byte tipo | Estructura | Descripción |
|---|---|---|
| `0x02` | `meas_pkt_hdr_t` + `measurement_t` | Medición completa (respuesta a `CMD_GET_DATA`) |
| `0x01` | `stream_chunk_hdr_t` + muestras | Chunk de stream en vivo |

Ver `main/protocol.h` para la definición exacta de cada estructura.

---

## Flujo de una medición automática

```
[Esclavo]                         [Master]                    [PC]
vigilante detecta golpe
  │ RMS > umbral
  └─ espnow_trigger_notify ──────► congela buffer master
                                   broadcast trigger ─────────────────┐
  ◄─────────────────────────────── MSG_TRIGGER                        │
congela buffer esclavo                                                 │
espnow_trigger_ack ────────────►  nodes[i].status = RECORDING         │
                                                               "EVENT TRIGGER"
[tx_task esclavo]                [tx_task master]
  pre del buffer circular          pre del buffer circular
  espera post_buf lleno            espera post_buf lleno
  guarda en g_slots[]              guarda en g_slots[]
  guarda en SD                     guarda en SD
  TCP/5001 → master ─────────────► TCP/5000 ──────────────────► binario
```

---

## Almacenamiento SD

Las mediciones se guardan automáticamente en `/sd/mediciones/` con el formato:

```
MMMMNN_TIMESTAMP_slotN.bin
```

Donde `MMMMNN` son los últimos 3 bytes de la MAC del nodo. El archivo es un volcado binario de `measurement_t` con las muestras incluidas al final.

---

## Issues conocidos / TODO

- [ ] `s_pending_data_request` es un booleano — si la PC solicita datos de dos esclavos seguidos antes de que el primero responda, el segundo request pisa al primero.
- [ ] La configuración (`g_config`) no se persiste en NVS — se pierde al reiniciar.
- [ ] `measurement_to_summary()` calcula RMS y peak reales pero solo se usa para el log local; no se envía a la PC al finalizar la grabación. El `EVENT TRIGGER` reporta el RMS del vigilante (22 muestras) y `PEAK=0.000` hardcodeado.
- [ ] `EVT_NUEVO_MASTER` se setea en el event group pero ninguna tarea lo espera.
- [ ] No hay reconexión automática robusta del `slave_client` si el master se reinicia (cambio de rol por botón).
- [ ] Los ejemplos de diagramas en `circ_buffer.c` tienen errores (el head se dibuja en la posición donde se escribió, no donde escribirá).
