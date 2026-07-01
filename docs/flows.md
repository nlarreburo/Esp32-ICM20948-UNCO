# Flujos del sistema

Cada sección describe el camino completo de un escenario: qué lo desencadena, qué funciones intervienen y en qué orden.

---

## 1. Detección / Trigger

### 1.1 Esclavo detecta vibración

```
[Esclavo - Core 1]
vigilante_task
  └─ circ_buffer_peek_last(g_buffer, 22 muestras)
  └─ RMS > threshold_g
  └─ xEventGroupSetBits(EVT_VIBRACION)       ← evita re-entrar
  └─ espnow_send(MSG_TRIGGER_NOTIFY)         → master

[Master - espnow_rx_task]
  └─ on_espnow_trigger_notify()
       └─ asigna slot libre
       └─ wifi_manager_send("EVENT TRIGGER MAC=... SLOT=n RMS=x\n")  → PC
       └─ espnow_send(MSG_TRIGGER, broadcast)  → todos los nodos
       └─ xEventGroupSetBits(EVT_TRIGGER)     ← despierta tx_task del master

[Esclavo - espnow_rx_task]
  └─ on_espnow_trigger()
       └─ xEventGroupSetBits(EVT_TRIGGER)     ← despierta tx_task del esclavo
       └─ espnow_send(MSG_TRIGGER_ACK)        → master

[tx_task — en cada nodo]
  └─ congela buffer: circ_buffer_copy(g_buffer → g_buffer_frozen)
  └─ activa g_post_active = true
  └─ [sensor_task empieza a llenar g_post_buf en paralelo]
  └─ espera: g_post_count >= post_samples  (o EVT_STOP)
  └─ measurement_alloc(pre_samples + post_samples)
  └─ copia g_buffer_frozen → samples[0..pre-1]
  └─ copia g_post_buf      → samples[pre..total-1]
  └─ guarda en g_slots[n]
  └─ guarda en SD (/sd/mediciones/MMMNNN_timestamp_slotN.bin)
  └─ [si esclavo y g_auto_sync] slave_client_task envía por TCP/5001
```

---

### 1.2 Master detecta vibración

Igual que 1.1 pero sin el paso inicial de `MSG_TRIGGER_NOTIFY` — el master es quien detecta directamente.

```
[Master - Core 1]
vigilante_task
  └─ RMS > threshold_g
  └─ xEventGroupSetBits(EVT_VIBRACION)
  └─ asigna slot libre
  └─ wifi_manager_send("EVENT TRIGGER ...")   → PC
  └─ espnow_send(MSG_TRIGGER, broadcast)      → todos los esclavos
  └─ xEventGroupSetBits(EVT_TRIGGER)          ← despierta su propia tx_task

[Esclavos - espnow_rx_task]
  └─ on_espnow_trigger() → xEventGroupSetBits(EVT_TRIGGER)
  └─ espnow_send(MSG_TRIGGER_ACK)

[tx_task — en cada nodo]
  └─ (igual que 1.1 desde "congela buffer")
```

---

### 1.3 Trigger manual desde PC

```
[PC]
  └─ envía: CMD_START\n

[Master - client_task]
  └─ handle_command("CMD_START")
       └─ verifica g_status == IDLE
       └─ asigna slot libre
       └─ espnow_send(MSG_TRIGGER, trigger_source=MANUAL, broadcast)
       └─ xEventGroupSetBits(EVT_TRIGGER)   ← despierta tx_task del master
       └─ responde: OK\n  → PC

[Esclavos - espnow_rx_task]
  └─ on_espnow_trigger() → xEventGroupSetBits(EVT_TRIGGER)

[tx_task — en cada nodo]
  └─ (igual que 1.1 desde "congela buffer")
  └─ measurement_t.trigger_source = TRIGGER_SOURCE_MANUAL
  └─ measurement_t.rms_trigger    = 0
```

> No se envía `EVENT TRIGGER` a la PC — fue ella quien inició la grabación.

---

## 2. Solicitud de datos

### 2.1 GET_DATA del master

```
[PC]
  └─ envía: CMD_GET_DATA MAC=<mac_master> SLOT=n\n

[Master - client_task]
  └─ handle_command()
       └─ detecta que MAC == mac propia
       └─ toma semáforo g_slots_mutex
       └─ calcula length = measurement_size(g_slots[n]->num_samples)
       └─ envía meas_pkt_hdr_t (12 bytes)  → PC
       └─ envía measurement_t + samples[]  → PC
```

---

### 2.2 GET_DATA de un esclavo

```
[PC]
  └─ envía: CMD_GET_DATA MAC=<mac_esclavo> SLOT=n\n

[Master - client_task]
  └─ handle_command()
       └─ detecta que MAC es de un esclavo
       └─ s_pending_data_request = true
       └─ espnow_send(MSG_DATA_REQUEST, slot_index=n)  → esclavo

[Esclavo - espnow_rx_task]
  └─ on_espnow_data_request()
       └─ lee g_slots[n]
       └─ slave_client_task envía SLAVE_PKT_MEASUREMENT por TCP/5001  → master

[Master - slave_server_task]
  └─ on_slave_measurement()
       └─ reenvía meas_pkt_hdr_t + measurement_t por TCP/5000  → PC
       └─ s_pending_data_request = false
```

> **Limitación conocida:** `s_pending_data_request` es un booleano. Si llegan dos `CMD_GET_DATA` de esclavos distintos antes de que responda el primero, el segundo pisa al primero.

---

### 2.3 Auto-sync (envío automático al terminar)

```
[PC]
  └─ envía: CMD_SYNC_START\n

[Master - client_task]
  └─ handle_command()
       └─ g_auto_sync = true
       └─ espnow_send(MSG_SYNC_START, broadcast)  → todos los esclavos
       └─ responde: OK\n

[Esclavos - espnow_rx_task]
  └─ on_espnow_sync_start() → g_auto_sync = true

--- al terminar una grabación ---

[tx_task del esclavo]
  └─ medición guardada en g_slots[n]
  └─ g_auto_sync == true
       └─ slave_client_task envía SLAVE_PKT_MEASUREMENT  → master (TCP/5001)

[slave_server_task del master]
  └─ reenvía meas_pkt_hdr_t + measurement_t  → PC (TCP/5000)

[tx_task del master]
  └─ medición guardada en g_slots[n]
  └─ g_auto_sync == true
       └─ wifi_manager_send(meas_pkt_hdr_t + measurement_t)  → PC (TCP/5000)
```

---

## 3. Stream

### 3.1 Stream de un nodo (master)

```
[PC]
  └─ envía: CMD_STREAM_START MAC=<mac_master>\n

[Master - client_task]
  └─ handle_command()
       └─ g_status = STREAMING
       └─ xEventGroupSetBits(EVT_TRIGGER)
       └─ responde: OK\n

[tx_task del master]
  └─ congela g_buffer → g_buffer_frozen
  └─ envía chunks del pre-trigger (g_buffer_frozen) por TCP/5000
       cada chunk: stream_chunk_hdr_t (9 bytes) + 45 muestras × 6 bytes
  └─ activa g_post_active
  └─ bucle: mientras no EVT_STOP
       └─ espera nuevas muestras en g_post_buf
       └─ envía chunk  → PC
```

---

### 3.2 Stream de un esclavo

```
[PC]
  └─ envía: CMD_STREAM_START MAC=<mac_esclavo>\n

[Master - client_task]
  └─ handle_command()
       └─ espnow_send(MSG_STREAM_START)  → esclavo
       └─ responde: OK\n

[Esclavo - espnow_rx_task]
  └─ on_espnow_stream_start()
       └─ g_status = STREAMING
       └─ xEventGroupSetBits(EVT_TRIGGER)

[tx_task del esclavo]
  └─ (igual que 3.1 pero los chunks van por TCP/5001)

[slave_server_task del master]
  └─ recibe SLAVE_PKT_STREAM_CHUNK
  └─ reenvía stream_chunk_hdr_t + muestras  → PC (TCP/5000)
```

---

### 3.3 Stream de todos los nodos

```
[PC]
  └─ envía: CMD_STREAM_START_ALL\n

[Master - client_task]
  └─ espnow_send(MSG_STREAM_START, broadcast)  → todos los esclavos
  └─ activa stream propio (igual que 3.1)
  └─ responde: OK\n

[Esclavos]
  └─ (igual que 3.2 por cada esclavo)

[PC]
  └─ recibe chunks entrelazados de todos los nodos
  └─ distingue el origen por stream_chunk_hdr_t.mac
```

---

### 3.4 Stop stream

```
[PC]
  └─ envía: CMD_STREAM_STOP\n  |  CMD_STREAM_STOP_ALL\n

[Master - client_task]
  └─ handle_command()
       └─ xEventGroupSetBits(EVT_STOP)            ← detiene tx_task propia
       └─ espnow_send(MSG_STREAM_STOP, broadcast)  → esclavos
       └─ responde: OK\n

[Esclavos - espnow_rx_task]
  └─ on_espnow_stream_stop()
       └─ xEventGroupSetBits(EVT_STOP)  ← detiene tx_task del esclavo
```

---

## 4. Configuración y estado

### 4.1 SET_CONFIG

```
[PC]
  └─ envía: CMD_SET_CONFIG PRE=1000 POST=2000 THR=2.0\n

[Master - client_task]
  └─ handle_command()
       └─ sscanf() parsea los parámetros presentes
       └─ toma semáforo g_config_mutex
       └─ actualiza g_config (solo los campos recibidos)
       └─ libera semáforo
       └─ espnow_send(MSG_CONFIG, measurement_config_t)  → todos los esclavos
       └─ responde: OK\n

[Esclavos - espnow_rx_task]
  └─ on_espnow_config()
       └─ actualiza su g_config
       └─ espnow_send(MSG_CONFIG_ACK)  → master
```

---

### 4.2 GET_STATUS

```
[PC]
  └─ envía: CMD_GET_STATUS\n

[Master - client_task]
  └─ handle_command()
       └─ toma semáforo g_nodes_mutex
       └─ snprintf() construye JSON con todos los g_nodes[]
       └─ libera semáforo
       └─ responde: STATUS <json>\n  → PC
```

---

## 5. Red y arranque

### 5.1 Arranque como master

```
app_main()
  └─ espnow_manager_init()   ← registra callbacks, inicia ISR
  └─ wifi_base_init()        ← AP oculto "ESP_NOW_SLAVE" canal 6
  └─ escucha MSG_HEARTBEAT durante 10 segundos
  └─ no recibe ninguno → ROL = MASTER
  └─ wifi_manager_init()     ← AP visible "BRIDGE_MONITOR", lanza server_task
  └─ slave_server_init()     ← lanza slave_server_task (TCP/5001)
  └─ crea tareas: sensor_task, vigilante_task, tx_task,
                  heartbeat_task, button_task, espnow_rx_task
  └─ LED GPIO2 = ON  (indicador de rol master)
```

---

### 5.2 Arranque como esclavo

```
app_main()
  └─ espnow_manager_init()
  └─ wifi_base_init()
  └─ escucha MSG_HEARTBEAT durante 10 segundos
  └─ recibe heartbeat → guarda MAC del master → ROL = ESCLAVO
  └─ wifi_slave_connect()    ← modo APSTA, conecta a "BRIDGE_MONITOR"
  └─ slave_client_init()     ← lanza slave_client_task
  └─ crea tareas: sensor_task, vigilante_task, tx_task,
                  button_task, espnow_rx_task

[slave_client_task]
  └─ conecta a TCP/5001 del master (192.168.4.1:5001)
  └─ envía SLAVE_PKT_HANDSHAKE (mac[6])
  └─ espera SLAVE_PKT_HANDSHAKE_ACK
  └─ queda bloqueado esperando datos para enviar
```

---

### 5.3 Heartbeat periódico

```
[heartbeat_task — cada 5 segundos]
  └─ espnow_send(MSG_HEARTBEAT, timestamp_ms)  → broadcast

[Esclavos - espnow_rx_task]
  └─ on_espnow_heartbeat()
       └─ espnow_send(MSG_HEARTBEAT_ACK, status + slot + free_heap)  → master

[Master - espnow_rx_task]
  └─ on_espnow_heartbeat_ack()
       └─ actualiza g_nodes[i].last_seen_ms = now
       └─ actualiza g_nodes[i].status, current_slot
```

---

### 5.4 Nodo se desconecta

```
[heartbeat_task — en cada ciclo]
  └─ para cada g_nodes[i]:
       └─ si (now - last_seen_ms) > 15000 ms
            └─ g_nodes[i].online = false

[CMD_GET_STATUS posterior]
  └─ JSON incluye "online": false para ese nodo

[CMD_GET_DATA posterior al nodo offline]
  └─ master envía MSG_DATA_REQUEST → sin respuesta
  └─ (timeout no implementado — s_pending_data_request queda en true)
```

---

### 5.5 Cambio de master

```
[button_task — nodo actual]
  └─ detecta pulsación larga (polling cada 10ms)
  └─ espnow_send(MSG_NEW_MASTER, broadcast)  → todos los nodos
  └─ esp_restart()

[Otros nodos - espnow_rx_task]
  └─ on_espnow_new_master()
       └─ espnow_send(MSG_NEW_MASTER_ACK)  → nodo que pulsó
       └─ xEventGroupSetBits(EVT_NUEVO_MASTER)  ← definido, no procesado aún

[Tras el reinicio del nodo que pulsó]
  └─ app_main() escucha heartbeats 10 segundos
  └─ no hay master activo → asume ROL = MASTER
  └─ (flujo de arranque como master — sección 5.1)

[Otros nodos]
  └─ slave_client_task detecta que la conexión TCP/5001 se cerró
  └─ intenta reconectar al nuevo master
```
