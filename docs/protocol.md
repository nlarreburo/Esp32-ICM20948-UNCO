# Protocolo de comunicación

El sistema usa dos protocolos en paralelo sobre la misma interfaz WiFi:

| Protocolo | Entre | Uso |
|---|---|---|
| **ESP-NOW** | Nodo ↔ Nodo | Señalización liviana: triggers, heartbeats, configuración |
| **TCP** | PC ↔ Master (5000) | Comandos de control y datos hacia la PC |
| **TCP** | Esclavo ↔ Master (5001) | Transferencia de mediciones completas y stream |

---

## 1. Protocolo TCP — PC ↔ Master (puerto 5000)

### Formato general

```
PC → Master:   texto plano terminado en \n
Master → PC:   texto para respuestas de control
               binario para datos pesados (sin \n)
```

> **¿Por qué texto y no binario para los comandos?**
> Los comandos de la PC al master son eventos únicos de baja frecuencia (uno cada varios segundos como mínimo). El overhead de parsear texto (`strcmp`, `sscanf`) es despreciable a esa cadencia. La ventaja es práctica: se puede operar el sistema con cualquier cliente TCP (`telnet`, `nc`, Python `socket`) sin necesidad de un serializador binario en la PC. El flujo de datos pesados (mediciones, stream) sí usa binario porque allí el volumen importa.

Solo se permite **una conexión activa a la vez**. Si llega una segunda conexión mientras hay un cliente, el master la rechaza con `ERROR ya hay un cliente conectado`.

---

### 1.1 Comandos (PC → Master)

#### `CMD_GET_STATUS`

Devuelve el estado de todos los nodos conocidos.

```
Envío:    CMD_GET_STATUS\n
Respuesta: STATUS <json>\n
```

Estructura del JSON:
```json
{
  "role": "master",
  "nodes": [
    {
      "mac": "aa:bb:cc:dd:ee:ff",
      "status": "idle",
      "slot": 0,
      "online": true,
      "role": "master"
    },
    {
      "mac": "11:22:33:44:55:66",
      "status": "recording",
      "slot": 1,
      "online": true
    }
  ]
}
```

Valores posibles de `status`: `idle`, `recording`, `streaming`.
Un nodo se marca `online: false` si no respondió un heartbeat en los últimos 15 segundos.

> **¿Por qué JSON y no binario para la respuesta de status?**
> La respuesta de status es puntual (se pide manualmente, no en bucle) y su tamaño es pequeño (~200 bytes con 8 nodos). JSON permite que la PC lo procese con cualquier librería estándar y que sea legible en un terminal. El coste de serializar el JSON en el master es una sola llamada a `snprintf` y no afecta ningún flujo en tiempo real.

---

#### `CMD_START`

Dispara una grabación manual en todos los nodos simultáneamente.

```
Envío:    CMD_START\n
Respuesta: OK\n  |  ERROR <motivo>\n
```

Solo funciona si el master está en estado `idle`. El trigger se propaga a todos los esclavos por ESP-NOW.

---

#### `CMD_STOP`

Detiene una grabación o stream activo en todos los nodos.

```
Envío:    CMD_STOP\n
Respuesta: OK\n
```

La grabación se guarda con las muestras acumuladas hasta ese momento (puede ser incompleta).

---

#### `CMD_SET_CONFIG`

Actualiza los parámetros de medición en todos los nodos.

```
Envío:    CMD_SET_CONFIG PRE=<ms> POST=<ms> MANUAL=<ms> THR=<g>\n
Respuesta: OK\n  |  ERROR <motivo>\n
```

Todos los parámetros son opcionales — los que se omitan mantienen su valor actual. El master aplica la configuración localmente y la propaga a los esclavos por ESP-NOW (`MSG_CONFIG`).

Ejemplo:
```
CMD_SET_CONFIG PRE=1000 POST=2000 THR=2.0\n
```

---

#### `CMD_GET_DATA`

Solicita la medición completa almacenada en un slot de un nodo.

```
Envío:    CMD_GET_DATA MAC=<xx:xx:xx:xx:xx:xx> SLOT=<n>\n
Respuesta: [binario — ver sección 1.3]
```

- Si `MAC` corresponde al master, los datos se leen directamente de PSRAM.
- Si `MAC` corresponde a un esclavo, el master solicita los datos por ESP-NOW (`MSG_DATA_REQUEST`) y los reenvía a la PC cuando llegan por TCP/5001.

> **Limitación actual:** si se envían dos `CMD_GET_DATA` de esclavos distintos antes de que llegue la respuesta del primero, el segundo request pisa al primero.

---

#### `CMD_STREAM_START`

Inicia stream en vivo de muestras desde un nodo específico.

```
Envío:    CMD_STREAM_START MAC=<xx:xx:xx:xx:xx:xx>\n
Respuesta: OK\n  |  ERROR <motivo>\n
           [chunks binarios continuos — ver sección 1.3]
```

El stream incluye primero las muestras del pre-trigger (buffer circular congelado) y luego muestras en tiempo real hasta recibir `CMD_STREAM_STOP`.

---

#### `CMD_STREAM_START_ALL`

Inicia stream en vivo de todos los nodos simultáneamente.

```
Envío:    CMD_STREAM_START_ALL\n
Respuesta: OK\n
           [chunks binarios de todos los nodos entrelazados]
```

La PC distingue el origen de cada chunk por el campo `mac` del header binario.

---

#### `CMD_STREAM_STOP` / `CMD_STREAM_STOP_ALL`

Detiene el stream activo.

```
Envío:    CMD_STREAM_STOP\n   |   CMD_STREAM_STOP_ALL\n
Respuesta: OK\n
```

---

#### `CMD_SYNC_START` / `CMD_SYNC_STOP`

Activa o desactiva el envío automático de mediciones al finalizar cada grabación.

```
Envío:    CMD_SYNC_START\n   |   CMD_SYNC_STOP\n
Respuesta: OK\n
```

Con sync activo, cada esclavo envía su medición al master por TCP/5001 al terminar `tx_task`, sin que la PC lo solicite.

---

### 1.2 Eventos espontáneos (Master → PC)

El master puede enviar mensajes sin que la PC los haya solicitado:

#### `EVENT TRIGGER`

Notifica que se detectó una vibración automática.

```
EVENT TRIGGER MAC=<xx:xx:xx:xx:xx:xx> SLOT=<n> RMS=<x.xxx> PEAK=0.000\n
```

- `MAC` — nodo que detectó la vibración
- `SLOT` — slot donde se guardará la medición
- `RMS` — magnitud calculada por el vigilante al momento de la detección (sobre 22 muestras)
- `PEAK` — actualmente hardcodeado en `0.000` (pendiente de implementar)

---

### 1.3 Datos binarios (Master → PC)

#### Medición completa — tipo `0x02`

Enviada en respuesta a `CMD_GET_DATA` o automáticamente si `CMD_SYNC_START` está activo.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  meas_pkt_hdr_t  (12 bytes)                                                  │
│    type   : uint8  = 0x02     ← discriminador: 0x02 = medición completa      │
│    mac    : uint8[6]          ← MAC del nodo que capturó la medición          │
│    slot   : uint8             ← índice de slot (0-4) donde está guardada     │
│    length : uint32            ← bytes del payload que sigue (measurement_t)  │
├──────────────────────────────────────────────────────────────────────────────┤
│  measurement_t  (31 bytes fijos + muestras)                                  │
│    node_mac       : uint8[6]  ← MAC del nodo (igual que en el header TCP)    │
│    slot_index     : uint8     ← eco del slot_index del header                │
│    trigger_source : uint8     ← 0=automático (vigilante), 1=manual (PC)      │
│    timestamp_ms   : uint32    ← ms desde arranque cuando ocurrió el trigger  │
│    pre_trigger_ms : uint16    ← ventana pre-trigger configurada al grabar    │
│    post_trigger_ms: uint16    ← ventana post-trigger configurada al grabar   │
│    threshold_g    : float     ← umbral en g activo al momento del trigger    │
│    rms_trigger    : float     ← RMS que disparó el evento (0 si fue manual)  │
│    acel_range     : uint8     ← rango del acelerómetro (ver tabla abajo)     │
│    sample_rate_hz : uint16    ← ODR efectivo en Hz (nominalmente 225)        │
│    num_samples    : uint16    ← total de muestras = pre_samples + post       │
│    pre_samples    : uint16    ← cuántas de las num_samples son pre-trigger   │
│    samples[]      : int16[3] × num_samples  ← x, y, z por muestra (6B c/u) │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Cálculo de tamaño — ejemplo con configuración por defecto:**

```
Config:  pre=500ms, post=1000ms, ODR=225Hz

  pre_samples  = ceil(500 × 225 / 1000)  = 113 muestras
  post_samples = ceil(1000 × 225 / 1000) = 225 muestras
  num_samples  = 338 muestras

  meas_pkt_hdr_t :  12 bytes  (header TCP)
  measurement_t  :  31 bytes  (campos fijos)
  samples[]      : 338 × 6  = 2028 bytes

  Total paquete  :  12 + 31 + 2028 = 2071 bytes ≈ 2 KB
```

Cada muestra ocupa 6 bytes (int16 × 3 ejes). Para convertir a g:

| `acel_range` | Rango | Factor de escala |
|---|---|---|
| 0 | ±2g | 16384 LSB/g |
| 1 | ±4g | 8192 LSB/g |
| 2 | ±8g | 4096 LSB/g |
| 3 | ±16g | 2048 LSB/g |

```
valor_en_g = muestra_int16 / factor_de_escala
```

#### Chunk de stream — tipo `0x01`

Enviado continuamente durante un stream activo.

```
┌─────────────────────────────────────┐
│  stream_chunk_hdr_t  (9 bytes)      │
│    type  : uint8  = 0x01            │
│    mac   : uint8[6]                 │
│    count : uint16 (muestras en este chunk) │
├─────────────────────────────────────┤
│  muestras: int16[3] × count         │  ← x, y, z por muestra
└─────────────────────────────────────┘
```

Tamaño típico de un chunk: 45 muestras → 9 + 45×6 = **279 bytes**.

**Cadencia de envío:** `STREAM_CHUNK_SAMPLES = 45` muestras a 225 Hz → un chunk cada **200 ms**. La `tx_task` acumula 45 muestras de `g_post_buf` y las envía juntas, en lugar de enviar muestra a muestra, para reducir el overhead de cabeceras TCP.

---

## 2. Protocolo TCP — Esclavo ↔ Master (puerto 5001)

Protocolo binario interno. La PC nunca se conecta a este puerto.

### Tipos de paquete

```c
SLAVE_PKT_HANDSHAKE     = 0x01  // esclavo → master al conectar
SLAVE_PKT_HANDSHAKE_ACK = 0x02  // master → esclavo: confirmación
SLAVE_PKT_MEASUREMENT   = 0x03  // esclavo → master: medición completa
SLAVE_PKT_STREAM_CHUNK  = 0x04  // esclavo → master: chunk de stream
```

### Framing

```
┌─────────────────────────────────────┐
│  slave_pkt_header_t  (5 bytes)      │
│    type   : uint8                   │
│    length : uint32 (bytes del payload) │
├─────────────────────────────────────┤
│  payload  (según type)              │
└─────────────────────────────────────┘
```

### Flujo de conexión

```
Esclavo conecta a TCP/5001
  → SLAVE_PKT_HANDSHAKE  payload: mac[6]
  ← SLAVE_PKT_HANDSHAKE_ACK
  → SLAVE_PKT_MEASUREMENT / SLAVE_PKT_STREAM_CHUNK  (cuando hay datos)
```

**Ejemplo con valores reales:**

El esclavo tiene MAC `AA:BB:CC:DD:EE:11` y acaba de terminar una medición en el slot 0.

```
Esclavo → Master  [HANDSHAKE]
  Bytes enviados: 01  06 00 00 00  AA BB CC DD EE 11
                  │   └─────────── length = 6 (uint32, little-endian)
                  │                └─ payload: mac[6] del esclavo
                  └─────────────── type = 0x01 (SLAVE_PKT_HANDSHAKE)

Master → Esclavo  [HANDSHAKE_ACK]
  Bytes enviados: 02  00 00 00 00
                  │   └────────── length = 0 (sin payload)
                  └────────────── type = 0x02 (SLAVE_PKT_HANDSHAKE_ACK)

Esclavo → Master  [MEASUREMENT]
  Bytes enviados: 03  1F 08 00 00  <2079 bytes de measurement_t>
                  │   └─────────── length = 0x0000081F = 2079 bytes (little-endian)
                  └────────────── type = 0x03 (SLAVE_PKT_MEASUREMENT)
```

El ACK confirma que el master reconoció al esclavo y está listo para recibir datos. Sin él, el esclavo no envía la medición (evita que datos lleguen antes de que el master conozca la MAC del remitente).


---

## 3. Protocolo ESP-NOW — Nodo ↔ Nodo

Mensajes de control entre nodos. Máximo 250 bytes por paquete. Todos usan el mismo header:

```c
typedef struct {
    uint8_t msg_type;   // tipo de mensaje (ver tabla)
    uint8_t src_mac[6]; // MAC del remitente (completada automáticamente)
    uint8_t reserved;   // padding → lleva el header a 8 bytes (potencia de 2)
} msg_header_t;         // 8 bytes total
```

> **¿Para qué sirve `reserved`?**
> Sin el byte extra, `msg_header_t` tendría 7 bytes (1 + 6). Con `reserved`, queda en 8 bytes, alineado a potencia de 2. Esto facilita copias de memoria eficientes y evita que los campos del payload que van detrás queden en direcciones impares, lo que en algunos procesadores penaliza el acceso. Además, deja un byte libre para uso futuro (versión de protocolo, flags, etc.) sin romper compatibilidad.


### Tabla de mensajes

| Tipo | Valor | Dirección | Descripción |
|---|---|---|---|
| `MSG_TRIGGER` | `0x01` | Master → todos | Iniciar grabación en todos los nodos |
| `MSG_TRIGGER_ACK` | `0x02` | Nodo → Master | Confirmación de trigger recibido |
| `MSG_TRIGGER_NOTIFY` | `0x03` | Esclavo → Master | Vigilante detectó vibración |
| `MSG_STOP` | `0x04` | Master → todos | Detener grabación activa |
| `MSG_STOP_ACK` | `0x05` | Nodo → Master | Confirmación de stop |
| `MSG_CONFIG` | `0x10` | Master → todos | Nueva configuración de medición |
| `MSG_CONFIG_ACK` | `0x11` | Nodo → Master | Configuración aplicada |
| `MSG_HEARTBEAT` | `0x20` | Master → todos | Ping de presencia |
| `MSG_HEARTBEAT_ACK` | `0x21` | Nodo → Master | Respuesta con estado actual |
| `MSG_DATA_REQUEST` | `0x30` | Master → Esclavo | Solicitar medición de un slot |
| `MSG_NEW_MASTER` | `0x50` | Nodo → todos | Anuncio de nuevo master |
| `MSG_NEW_MASTER_ACK` | `0x51` | Nodo → Master | Reconocimiento del nuevo master |
| `MSG_SYNC_START` | `0x60` | Master → todos | Activar envío automático de mediciones |
| `MSG_SYNC_STOP` | `0x61` | Master → todos | Desactivar envío automático |
| `MSG_STREAM_START` | `0x70` | Master → Nodo | Activar modo stream |
| `MSG_STREAM_STOP` | `0x71` | Master → Nodo | Desactivar modo stream |

### Detalle de cada mensaje

#### `MSG_HEARTBEAT` — Master → todos

```c
uint32_t timestamp_ms;   // timestamp actual del master
```

> **¿Por qué interesa el timestamp del master?**
> Cada nodo tiene su propio reloj interno (`esp_timer_get_time()`) que arranca desde cero al reiniciarse, sin sincronización externa. El timestamp del heartbeat actúa como referencia temporal compartida: los esclavos pueden calcular cuánto difieren sus relojes del master y compensarlo al reconstruir la línea de tiempo de una medición. También permite detectar si el master se reinició (el timestamp retrocede a cero), lo que le indica al esclavo que debe reconectarse.


#### `MSG_HEARTBEAT_ACK` — Nodo → Master

```c
node_status_t status;       // IDLE=0, RECORDING=1, STREAMING=2
uint8_t       current_slot; // slot activo actualmente
uint32_t      free_heap;    // bytes libres en RAM (útil para debug)
```

#### `MSG_TRIGGER` — Master → todos

```c
uint8_t  slot_index;      // slot donde debe guardar la medición
uint8_t  trigger_source;  // 0=automático, 1=manual
uint32_t timestamp_ms;    // timestamp del master al disparar
```

El `timestamp_ms` es el mismo en todos los nodos — base temporal común para sincronización.

#### `MSG_TRIGGER_ACK` — Nodo → Master

```c
uint8_t  slot_index;     // eco del slot recibido
uint32_t timestamp_ms;   // timestamp local del nodo al recibir el trigger
```

La diferencia `ack.timestamp_ms - trigger.timestamp_ms` representa la latencia de red entre master y nodo.

**Ejemplo:**

```
Master envía MSG_TRIGGER    →  trigger.timestamp_ms = 5000 ms
Esclavo recibe y responde   →  ack.timestamp_ms     = 5003 ms

Latencia estimada: 5003 - 5000 = 3 ms

Esto significa que el esclavo empezó a grabar 3 ms más tarde que el master.
Si se quiere reconstruir una línea de tiempo conjunta, las muestras del esclavo
deben desplazarse 3 ms hacia atrás respecto a las del master.
```

#### `MSG_TRIGGER_NOTIFY` — Esclavo → Master

```c
float    rms_g;          // RMS que disparó la detección (22 muestras)
uint32_t timestamp_ms;   // cuándo ocurrió la detección en el esclavo
```


#### `MSG_CONFIG` — Master → todos

```c
measurement_config_t config; // pre_ms, post_ms, manual_ms, threshold_g
```

#### `MSG_DATA_REQUEST` — Master → Esclavo

```c
uint8_t slot_index;   // qué slot solicita el master
```

---

## 4. Notas de implementación

### Recepción ESP-NOW

Cuando llega un paquete ESP-NOW, el hardware WiFi del ESP32 llama automáticamente a `espnow_recv_cb`. Esta función no la invoca el código de la aplicación — la invoca el chip. Se llama **ISR** (Interrupt Service Routine, rutina de servicio de interrupción).

Mientras la ISR está corriendo, el procesador está interrumpido: no puede hacer otras cosas. Por eso debe ser brevísima (microsegundos). No puede llamar a `malloc`, no puede esperar un mutex, no puede bloquear.

La solución es separar la recepción del procesamiento en dos pasos:

```
WiFi ISR                      espnow_rx_task (Core 0)
─────────────────             ───────────────────────────────────────
paquete llega
espnow_recv_cb()
  copia dato → cola[16]  ──►  xQueueReceive(portMAX_DELAY)
  retorna (< 10 µs)            switch(msg_type)
                                 case MSG_TRIGGER:
                                   on_trigger((msg_trigger_t *)data)
                                 case MSG_HEARTBEAT_ACK:
                                   on_heartbeat_ack(...)
                                 ...
```

La ISR solo deposita el paquete en la cola y sale. La tarea `espnow_rx_task` lee la cola con calma y llama los callbacks. Si la cola de 16 elementos se llena (tarea demasiado lenta), los paquetes entrantes se descartan.

### Canal WiFi

ESP-NOW y el AP WiFi deben operar en el mismo canal. Ambos están configurados en el **canal 6** (`ESPNOW_CHANNEL` y `WIFI_AP_CHANNEL`). Cambiar uno sin cambiar el otro rompe la comunicación ESP-NOW.

### Todas las estructuras son `__attribute__((packed))`

Por defecto, el compilador de C añade bytes de relleno ("padding") entre campos para alinear cada campo a su tamaño natural. Eso es eficiente en CPU pero hace que el layout en memoria no coincida con los bytes del cable.

**Ejemplo sin `packed`:**
```c
struct ejemplo {
    uint8_t  tipo;    // 1 byte  → el compilador añade 3 bytes de padding
    uint32_t largo;   // 4 bytes
};
// sizeof = 8 bytes, pero los bytes 1-3 son basura
```

**Con `__attribute__((packed))`:**
```c
struct ejemplo {
    uint8_t  tipo;    // 1 byte
    uint32_t largo;   // 4 bytes — inmediatamente después
} __attribute__((packed));
// sizeof = 5 bytes — exactamente lo que viaja por el cable
```

Todas las estructuras del protocolo (ESP-NOW y TCP) están `packed` para que el layout en memoria sea exactamente el wire format. Esto permite castear el buffer recibido directamente a la estructura sin copiar ni deserializar:

```c
s_callbacks.on_trigger((const msg_trigger_t *)item.data);
```

Si una estructura no fuera `packed`, el cast leería campos en posiciones incorrectas y los valores serían basura.
