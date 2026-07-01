# Arquitectura del sistema

---

## 1. Capas de software

```
┌────────────────────────────────────────────────────┐
│                     main.c                         │
│   lógica de negocio · tareas · callbacks · roles   │
├───────────┬───────────┬─────────────┬──────────────┤
│  espnow_  │  wifi_    │  slave_     │  slave_      │
│  manager  │  manager  │  client     │  server      │
│           │           │  (esclavo)  │  (master)    │
├───────────┴───────────┴─────────────┴──────────────┤
│              measurement  │  circ_buffer            │
├───────────────────────────┴─────────────────────────┤
│                      icm20948                       │
├─────────────────────────────────────────────────────┤
│           ESP-IDF  (FreeRTOS · WiFi · SPI · NVS)   │
└─────────────────────────────────────────────────────┘
```

`main.c` es el único archivo que importa a todos los demás. Los módulos de red y de datos no se importan entre sí — se comunican con `main.c` exclusivamente a través de callbacks.

---

## 2. Módulos

| Módulo | Archivo | Responsabilidad |
|---|---|---|
| **main** | `main.c` | Punto de entrada, creación de tareas, registro de callbacks, toda la lógica de negocio |
| **espnow_manager** | `espnow_manager.c/h` | Envío y recepción ESP-NOW, gestión de peers, desacoplamiento ISR→cola→tarea |
| **wifi_manager** | `wifi_manager.c/h` | AP WiFi, servidor TCP/5000, parseo de comandos de la PC |
| **slave_client** | `slave_client.c/h` | Cliente TCP/5001 — corre en el esclavo, envía mediciones y stream al master |
| **slave_server** | `slave_server.c/h` | Servidor TCP/5001 — corre en el master, recibe mediciones y stream de esclavos |
| **measurement** | `measurement.c/h` | Alloc en PSRAM, conversión ms→muestras, cálculo de RMS y peak |
| **circ_buffer** | `circ_buffer.c/h` | Buffer circular thread-safe con mutex interno |
| **icm20948** | `icm20948.c/h` | Driver SPI del acelerómetro: inicialización, FIFO, lectura de muestras |

---

## 3. Roles: Master y Esclavo

El rol se determina una sola vez al arranque y no cambia durante la ejecución (salvo reinicio por cambio de master).

```
app_main()
  │
  ├─ escucha heartbeats ESP-NOW durante 10 segundos
  │
  ├─ si recibió heartbeat  →  ROL = ESCLAVO
  │     init: wifi_slave_connect(), slave_client_init()
  │
  └─ si no recibió nada   →  ROL = MASTER
        init: wifi_manager_init(), slave_server_init()
              heartbeat_task, server_task, slave_server_task
```

Ambos roles comparten: `sensor_task`, `vigilante_task`, `tx_task`, `button_task`, `espnow_rx_task`.

---

## 4. Tareas FreeRTOS

En FreeRTOS, **mayor número = mayor prioridad**. La prioridad 5 (`sensor_task`) es la más alta del sistema — el planificador (scheduler) la ejecuta primero ante cualquier conflicto.

| Tarea | Core | Prioridad | Espera | Función |
|---|---|---|---|---|
| `sensor_task` | 1 | 5 | `vTaskDelay(50ms)` | Lee el FIFO del ICM-20948 y escribe en `g_buffer` |
| `vigilante_task` | 1 | 4 | `vTaskDelay(100ms)` | Calcula RMS sobre las últimas 22 muestras; si supera umbral, dispara trigger |
| `tx_task` | 0 | 3 | `xEventGroupWaitBits` | Arma medición (pre+post) o envía stream al recibir `EVT_TRIGGER` |
| `heartbeat_task` | 0 | 2 | `vTaskDelay(5000ms)` | Solo master: envía `MSG_HEARTBEAT` y marca nodos offline si no responden |
| `button_task` | 0 | 1 | `vTaskDelay(10ms)` poll | Detecta pulsación larga para cambio de master |
| `espnow_rx_task` | 0 | 2 | `xQueueReceive` | Consume la cola de la ISR y despacha callbacks ESP-NOW |
| `server_task` / `client_task` | 0 | 3 | `accept()` / `recv()` | Solo master: TCP/5000 — comandos de la PC |
| `slave_server_task` | 0 | 3 | `accept()` / `recv()` | Solo master: TCP/5001 — recibe datos de esclavos |
| `slave_client_task` | 0 | 3 | `xEventGroupWaitBits` | Solo esclavo: TCP/5001 — envía mediciones y stream al master |

**Core 1** aloja las tareas de sensor y vigilante para aislarlas del tráfico de red (Core 0). Así, ni la latencia TCP ni los callbacks ESP-NOW afectan la cadencia de muestreo.

---

## 5. Comunicación entre tareas

### EventGroup `g_eventos`

Un único EventGroup actúa como pizarra de señales entre tareas.

| Bit | Define | Quién lo setea | Quién lo espera |
|---|---|---|---|
| BIT0 | `EVT_VIBRACION` | `vigilante_task` (RMS > umbral) | `vigilante_task` mismo (evita re-entrar) |
| BIT1 | `EVT_TRIGGER` | `vigilante_task`, callback `on_espnow_trigger` | `tx_task` |
| BIT2 | `EVT_STOP` | callback `on_espnow_stop`, `handle_command` (CMD_STOP) | `tx_task` |
| BIT3 | `EVT_NUEVO_MASTER` | callback `on_new_master` | — (definido, no usado aún) |

`xEventGroupSetBits` despierta inmediatamente a cualquier tarea bloqueada en `xEventGroupWaitBits` que tenga ese bit en su máscara. Los bits permanecen seteados hasta que la tarea los limpia manualmente.

### Cola ESP-NOW

La ISR de recepción WiFi no puede bloquear ni llamar callbacks. Usa una cola de 16 elementos:

```
ISR espnow_recv_cb()  →  [cola 16 items]  →  espnow_rx_task  →  callbacks
     (< 10 µs)                                  (tarea normal)
```

Si la cola está llena, el paquete entrante se descarta.

### Mutex / Semáforos

Ambos bloquean el acceso a un recurso compartido, pero con reglas distintas:

| | Mutex | Semáforo binario |
|---|---|---|
| **Quién puede liberarlo** | Solo la tarea que lo tomó | Cualquier tarea o ISR |
| **Uso típico** | Proteger una variable compartida | Señalizar entre tareas |
| **Herencia de prioridad** | Sí — evita que una tarea de baja prioridad bloquee a una de alta | No |

En este proyecto los **mutex** protegen datos que una tarea modifica y luego libera ella misma. Los **semáforos binarios** se usan donde el que toma y el que da son contextos distintos.

| Variable protegida | Tipo | Por qué |
|---|---|---|
| `g_buffer` | mutex interno de `circ_buffer` | `sensor_task` escribe, `vigilante_task` y `tx_task` leen |
| `g_config` | semáforo binario | callbacks de red escriben, `vigilante_task` y `tx_task` leen |
| `g_nodes[8]` | semáforo binario | `heartbeat_task` y callbacks ESP-NOW escriben, callbacks TCP leen |
| `g_slots[5]` | semáforo binario | `tx_task` escribe, callbacks TCP leen |
| `s_client_fd` (wifi_manager) | mutex interno | evita que dos contextos envíen al mismo socket simultáneamente |

---

## 6. Variables globales clave

| Variable | Tipo | Quién escribe | Quién lee |
|---|---|---|---|
| `g_buffer` | `circ_buffer_t` | `sensor_task` | `vigilante_task`, `tx_task` |
| `g_buffer_frozen` | `circ_buffer_t` | `tx_task` (copia al trigger) | `tx_task` |
| `g_post_buf[900]` | `icm20948_raw_sample_t[]` | `sensor_task` (cuando `g_post_active`) | `tx_task` |
| `g_post_count` | `uint16_t` | `sensor_task` | `tx_task` |
| `g_post_active` | `bool` | `tx_task` activa/desactiva | `sensor_task` (condición de escritura) |
| `g_slots[5]` | `measurement_t *` | `tx_task` | callbacks TCP, `slave_client_task` |
| `g_nodes[8]` | `node_state_t` | `heartbeat_task`, callbacks ESP-NOW | callbacks TCP |
| `g_config` | `measurement_config_t` | callbacks TCP y ESP-NOW | `vigilante_task`, `tx_task` |
| `g_status` | `node_status_t` | `tx_task` | heartbeat, callbacks TCP |
| `g_role` | `node_role_t` | `app_main` (una sola vez) | todas las tareas |
| `g_auto_sync` | `bool` | callbacks TCP | `tx_task` (esclavo) |
| `s_pending_data_request` | `bool` | callbacks TCP | callbacks ESP-NOW |

---

## 7. Patrón de callbacks

El problema: `espnow_manager.c` necesita avisarle a `main.c` cuando llega un evento, pero no puede llamar a sus funciones directamente — eso crearía una dependencia circular (`main.c` → `espnow_manager.c` → `main.c`).

La solución es usar **punteros a función**. En vez de escribir el nombre de la función (lo que crearía la dependencia), `main.c` le pasa la dirección de memoria de su función al módulo durante la inicialización. El módulo la guarda y la llama cuando ocurre el evento — sin saber a qué archivo pertenece ni qué hace.

```c
// espnow_manager.h define la struct con punteros a función
typedef struct {
    void (*on_trigger)(const msg_trigger_t *);        // dirección, no una llamada
    void (*on_trigger_notify)(const msg_trigger_notify_t *);
    void (*on_heartbeat_ack)(const msg_heartbeat_ack_t *, const uint8_t *mac);
    // ...
} espnow_callbacks_t;

// main.c rellena la struct con las direcciones de sus funciones y la pasa al módulo
static espnow_callbacks_t s_espnow_cb = {
    .on_trigger        = on_espnow_trigger,       // <- dirección de memoria
    .on_trigger_notify = on_espnow_trigger_notify,
    .on_heartbeat_ack  = on_espnow_heartbeat_ack,
};
espnow_manager_init(&s_espnow_cb);

// espnow_manager guarda la struct internamente
static espnow_callbacks_t s_callbacks;
// ...y cuando llega un paquete, salta a esa dirección:
s_callbacks.on_trigger(msg);  // equivale a: ir a 0x400D1234 y ejecutar
```

`espnow_manager.c` nunca escribió el nombre `on_espnow_trigger` en su código — solo tiene una dirección. Por eso no depende de `main.c` y el acoplamiento va en una sola dirección: `main.c` → módulos.

---

## 8. Flujo de datos — de sensor a PC

```
ICM-20948 (SPI)
    │  FIFO: ráfaga de muestras cada 50ms
    ▼
sensor_task  ──────────────────────►  g_buffer (circ_buffer, 900 muestras)
                                           │
                               vigilante_task lee últimas 22
                                           │ RMS > umbral
                                           ▼
                               EVT_TRIGGER  +  MSG_TRIGGER_NOTIFY (ESP-NOW)
                                           │
                               tx_task despierta
                                  │
                                  ├─ copia g_buffer → g_buffer_frozen (pre-trigger)
                                  ├─ activa g_post_active
                                  ├─ espera g_post_buf lleno (post-trigger)
                                  ├─ arma measurement_t en g_slots[n]
                                  ├─ guarda en SD
                                  │
                                  └─ [si esclavo]  slave_client_task  →  TCP/5001  →  master
                                     [si master]   wifi_manager_send  →  TCP/5000  →  PC
```

---

## 9. Decisiones de diseño

**PSRAM para mediciones**

Cada slot ocupa ~2 KB (338 muestras × 6 bytes + 43 bytes de header). Con 5 slots son ~10 KB reservados permanentemente.

| | ESP32 base (sin PSRAM) | ESP32-S3 N16R8 (8 MB PSRAM) |
|---|---|---|
| Dónde van los slots | RAM interna (~520 KB total) | PSRAM (8 MB dedicada) |
| Impacto | Compite con pilas de tareas (~4 KB/tarea × 9 tareas), stack WiFi (~100 KB) y stack TCP/IP (~60 KB). El margen libre es ajustado. | Impacto en RAM interna = 0. Los slots no compiten con nada. |
| Riesgo | Heap agotado con mediciones largas o muchos nodos | Sin riesgo de agotamiento por slots |

**Buffer circular para pre-trigger**

El vigilante no sabe cuándo va a ocurrir un golpe. El buffer circular actúa como ventana deslizante: cuando llega el trigger, las últimas N muestras ya están capturadas sin haber usado memoria extra. En ambas variantes de hardware el buffer vive en RAM interna (900 muestras × 6 bytes = ~5,4 KB).

**Un cliente TCP a la vez (puerto 5000)**

Simplifica radicalmente el modelo de concurrencia: no hay que arbitrar entre clientes que piden datos simultáneamente. El servidor rechaza conexiones adicionales con un mensaje de error explícito. Esta limitación es igual en ESP32 y ESP32-S3 — es una decisión de diseño de software, no de hardware.

**Core 1 exclusivo para sensor y vigilante**

Aísla las tareas de tiempo real del tráfico de red. Ni una retransmisión TCP ni un callback ESP-NOW pueden retrasar la lectura del FIFO del sensor.

| | ESP32 base (LX6 240 MHz) | ESP32-S3 (LX7 240 MHz) |
|---|---|---|
| Arquitectura | Dual-core, misma frecuencia | Dual-core, ~40% más rápido por ciclo |
| Beneficio del aislamiento | El mismo: Core 1 libre de interrupciones de red | Mayor margen: el Core 1 procesa el FIFO más rápido, reduciendo el riesgo de pérdida de muestras bajo carga |
