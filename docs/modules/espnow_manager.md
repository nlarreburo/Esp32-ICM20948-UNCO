# espnow_manager

Capa de abstracción sobre el protocolo ESP-NOW. Gestiona el envío, la recepción y los peers. Desacopla la ISR de recepción del procesamiento mediante una cola.

---

## Responsabilidades

- Inicializar ESP-NOW y registrar la ISR de recepción
- Gestionar la lista de peers (add / remove / exists)
- Enviar mensajes estructurados (una función por tipo de mensaje)
- Despachar mensajes recibidos a los callbacks registrados por `main.c`

---

## Arquitectura interna

```
WiFi ISR
  espnow_recv_cb()
    └─ copia paquete → s_rx_queue (16 items)   ← no bloquea, < 10 µs

espnow_rx_task  (Core 0, prioridad 2)
  └─ xQueueReceive(portMAX_DELAY)
  └─ switch(msg_type)
       └─ castea buffer → struct correcta
       └─ llama s_callbacks.on_X(msg)
```

Si `s_rx_queue` se llena, los paquetes entrantes se descartan silenciosamente.

---

## API

### Inicialización

```c
esp_err_t espnow_manager_init(const espnow_callbacks_t *callbacks);
```
Inicializa ESP-NOW, registra la ISR y lanza `espnow_rx_task`. Debe llamarse después de `esp_wifi_start()`. Registra automáticamente la dirección de broadcast como peer.

```c
void espnow_manager_update_callbacks(const espnow_callbacks_t *callbacks);
```
Reemplaza los callbacks sin reinicializar ESP-NOW. Se usa en `app_main` tras determinar el rol (master/esclavo), ya que los callbacks difieren según el rol.

```c
void espnow_manager_get_own_mac(uint8_t mac_out[6]);
```
Devuelve la MAC propia. Se obtiene una vez en `init` con `esp_wifi_get_mac()`.

---

### Gestión de peers

ESP-NOW requiere registrar cada destinatario antes de enviarle.

```c
esp_err_t espnow_manager_add_peer(const uint8_t mac[6]);
esp_err_t espnow_manager_remove_peer(const uint8_t mac[6]);
bool      espnow_manager_peer_exists(const uint8_t mac[6]);
```

`add_peer` es idempotente — si el peer ya existe, no hace nada.

---

### Envío de mensajes

Todas las funciones de envío construyen el mensaje, completan `src_mac` con la MAC propia y llaman a `esp_now_send()`. Para broadcast usar `ESPNOW_BROADCAST_MAC`.

#### Control de medición

```c
esp_err_t espnow_send_trigger(const uint8_t dst_mac[6],
                               uint8_t slot_index,
                               uint8_t trigger_source,
                               uint32_t timestamp_ms);

esp_err_t espnow_send_trigger_ack(const uint8_t dst_mac[6],
                                   uint8_t slot_index,
                                   uint32_t timestamp_ms);

esp_err_t espnow_send_trigger_notify(const uint8_t dst_mac[6],
                                      float rms_g,
                                      uint32_t timestamp_ms);

esp_err_t espnow_send_stop(const uint8_t dst_mac[6]);
esp_err_t espnow_send_stop_ack(const uint8_t dst_mac[6], uint8_t status);
```

#### Configuración

```c
esp_err_t espnow_send_config(const uint8_t dst_mac[6],
                              const measurement_config_t *config);
esp_err_t espnow_send_config_ack(const uint8_t dst_mac[6], uint8_t ok);
```

#### Heartbeat

```c
esp_err_t espnow_send_heartbeat(const uint8_t dst_mac[6], uint32_t timestamp_ms);
esp_err_t espnow_send_heartbeat_ack(const uint8_t dst_mac[6],
                                     node_status_t status,
                                     uint8_t current_slot,
                                     uint32_t free_heap);
```

#### Stream

```c
esp_err_t espnow_send_stream_start(const uint8_t dst_mac[6]);
esp_err_t espnow_send_stream_stop(const uint8_t dst_mac[6]);
```

#### Datos y red

```c
esp_err_t espnow_send_data_request(const uint8_t dst_mac[6], uint8_t slot_index);
esp_err_t espnow_send_new_master(const uint8_t dst_mac[6]);
esp_err_t espnow_send_new_master_ack(const uint8_t dst_mac[6]);
esp_err_t espnow_send_sync_start(const uint8_t dst_mac[6]);
esp_err_t espnow_send_sync_stop(const uint8_t dst_mac[6]);
```

---

## Callbacks registrados por main.c

```c
typedef struct {
    espnow_cb_trigger_t         on_trigger;         // MSG_TRIGGER recibido
    espnow_cb_trigger_ack_t     on_trigger_ack;     // MSG_TRIGGER_ACK recibido
    espnow_cb_trigger_notify_t  on_trigger_notify;  // esclavo notifica vibración
    espnow_cb_stop_t            on_stop;
    espnow_cb_stop_ack_t        on_stop_ack;
    espnow_cb_config_t          on_config;
    espnow_cb_config_ack_t      on_config_ack;
    espnow_cb_heartbeat_t       on_heartbeat;
    espnow_cb_heartbeat_ack_t   on_heartbeat_ack;
    espnow_cb_new_master_t      on_new_master;
    espnow_cb_new_master_ack_t  on_new_master_ack;
    espnow_cb_sync_start_t      on_sync_start;
    espnow_cb_sync_stop_t       on_sync_stop;
    espnow_cb_stream_start_t    on_stream_start;
    espnow_cb_stream_stop_t     on_stream_stop;
    espnow_cb_data_request_t    on_data_request;
} espnow_callbacks_t;
```

Los callbacks se invocan desde `espnow_rx_task`. El puntero recibido apunta directamente al buffer de la cola — no hacer `free()` sobre él.

---

## Constantes

| Define | Valor | Descripción |
|---|---|---|
| `ESPNOW_RX_QUEUE_SIZE` | 16 | Máximo de mensajes en cola antes de descartar |
| `ESPNOW_SEND_TIMEOUT_MS` | 100 | Timeout de confirmación de envío |
| `ESPNOW_CHANNEL` | 6 | Canal WiFi — debe coincidir con `WIFI_AP_CHANNEL` |
