# wifi_manager

Gestiona el AP WiFi y el servidor TCP/5000. Parsea los comandos de texto que llegan de la PC y los despacha a los callbacks registrados por `main.c`. Solo corre en el **master**.

---

## Responsabilidades

- Levantar el AP WiFi "BRIDGE_MONITOR" (visible para la PC)
- Levantar el AP oculto "ESP_NOW_SLAVE" (canal 6, solo para ESP-NOW)
- Aceptar conexiones TCP en el puerto 5000 (una a la vez)
- Parsear comandos de texto y despacharlos
- Enviar respuestas de texto y datos binarios a la PC
- Proveer `wifi_slave_connect()` para que los esclavos se conecten al master

---

## Arquitectura interna

```
server_task  (Core 0, prioridad 3)
  └─ listen(5000)
  └─ accept()  → crea client_task por conexión
  └─ solo una conexión simultánea — rechaza nuevas con ERROR

client_task  (Core 0, prioridad 3)
  └─ recv() byte a byte hasta \n
  └─ handle_command(line)
       └─ strcmp / strncmp por comando
       └─ llama s_callbacks.on_X()
       └─ responde OK\n / ERROR\n / datos binarios
```

> `CMD_STREAM_START_ALL` se compara antes que `CMD_STREAM_START` con `strcmp` exacto, para evitar que el prefijo `strncmp` de `CMD_STREAM_START` lo capture primero.

---

## API

```c
esp_err_t wifi_base_init(void);
```
Inicializa el driver WiFi en modo AP básico (AP oculto para ESP-NOW). Debe llamarse en **ambos** roles porque ESP-NOW requiere WiFi inicializado.

```c
esp_err_t wifi_manager_init(const wifi_callbacks_t *callbacks);
```
Levanta el AP visible "BRIDGE_MONITOR" y lanza `server_task`. Solo llamar en el **master**, después de `wifi_base_init()`.

```c
esp_err_t wifi_slave_connect(void);
```
Conecta el nodo al AP del master en modo APSTA. Solo llamar en el **esclavo**.

```c
esp_err_t wifi_manager_send_event(const char *event_str);
```
Envía un mensaje espontáneo a la PC (por ejemplo `EVENT TRIGGER ...`). Thread-safe. Si no hay PC conectada, descarta silenciosamente.

```c
esp_err_t wifi_manager_send_raw(const void *data, size_t len);
```
Envía bytes crudos al socket activo. Mantiene el mutex interno durante todo el envío para evitar intercalado de datos.

```c
esp_err_t wifi_manager_send_stream_chunk(const icm20948_raw_sample_t *samples,
                                          uint16_t count);
```
Construye y envía un `stream_chunk_hdr_t` + muestras en una sola llamada.

```c
bool wifi_manager_is_connected(void);
```
Devuelve `true` si hay un cliente TCP activo.

---

## Callbacks registrados por main.c

```c
typedef struct {
    wifi_cb_start_t           on_start;           // CMD_START
    wifi_cb_stop_t            on_stop;            // CMD_STOP
    wifi_cb_get_status_t      on_get_status;      // CMD_GET_STATUS → escribe JSON en out_json
    wifi_cb_set_config_t      on_set_config;      // CMD_SET_CONFIG → recibe config ya parseada
    wifi_cb_sync_start_t      on_sync_start;      // CMD_SYNC_START
    wifi_cb_sync_stop_t       on_sync_stop;       // CMD_SYNC_STOP
    wifi_cb_stream_start_t    on_stream_start;    // CMD_STREAM_START → recibe mac
    wifi_cb_stream_stop_t     on_stream_stop;     // CMD_STREAM_STOP → recibe mac
    wifi_cb_stream_start_all_t on_stream_start_all; // CMD_STREAM_START_ALL
    wifi_cb_stream_stop_all_t  on_stream_stop_all;  // CMD_STREAM_STOP_ALL
    wifi_cb_get_data_t        on_get_data;        // CMD_GET_DATA → recibe mac, slot, fd
} wifi_callbacks_t;
```

Si el callback devuelve `ESP_OK`, `client_task` responde `OK\n`. Si devuelve `ESP_FAIL`, responde `ERROR <motivo>\n`. La excepción es `on_get_status` y `on_get_data`, que escriben directamente al socket.

---

## Constantes

| Define | Valor | Descripción |
|---|---|---|
| `WIFI_AP_SSID` | `"BRIDGE_MONITOR"` | Nombre de la red visible para la PC |
| `WIFI_AP_PASSWORD` | `"monitor123"` | Contraseña WPA2 |
| `WIFI_AP_CHANNEL` | `6` | Canal — debe coincidir con `ESPNOW_CHANNEL` |
| `WIFI_AP_MAX_CONN` | `5` | Máximo de clientes WiFi (nivel de asociación) |
| `WIFI_AP_IP` | `"192.168.4.1"` | IP fija del master |
