# Problemas identificados — pendientes de resolución

Documento para revisión en equipo. Cada problema incluye dónde ocurre, qué lo causa, qué efecto tiene en el sistema y una propuesta de solución.

---

## Severidad Alta

### 1. Conexión semi-abierta (half-open connection)

**Dónde:** `slave_client.c` — `slave_connect_task`

**Qué pasa:** El esclavo detecta una desconexión únicamente cuando `send()` falla. Si el master se cae mientras el esclavo está inactivo (sin enviar datos), la variable `s_connected` queda en `true` indefinidamente. El esclavo cree que está conectado cuando la conexión lleva minutos caída.

```c
// El loop de monitoreo solo comprueba una flag, no el estado real del socket
while(s_connected){
    vTaskDelay(pdMS_TO_TICKS(1000));  // nunca detecta caída si no hay envíos
}
```

**Efecto:** Cuando ocurre un trigger y el esclavo intenta enviar la medición, `send()` falla, se setea `s_connected = false` y comienza la reconexión — pero ya es tarde, la medición se pierde.

**Propuesta:** Activar `SO_KEEPALIVE` en el socket del esclavo o implementar un `recv()` con timeout en un hilo paralelo que detecte el cierre del lado del master.

---

### 2. Sin `SO_REUSEADDR` en el servidor TCP/5001

**Dónde:** `slave_server.c` — `slave_accept_task`

**Qué pasa:** Cuando el master se reinicia, intenta hacer `bind()` en el puerto 5001. Si había conexiones previas, el OS las retiene en estado `TIME_WAIT` hasta 60 segundos. Sin `SO_REUSEADDR`, `bind()` devuelve `EADDRINUSE` y falla. El problema es que ese error no se verifica — la tarea sigue corriendo pero `accept()` falla en cada iteración sin que nadie lo detecte.

```c
// slave_server.c — retornos ignorados
bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
listen(server_fd, MAX_SLAVES);
// si bind falla, accept() falla siempre en silencio
```

**Efecto:** Tras un reinicio del master (cambio de rol por botón, por ejemplo), ningún esclavo puede conectarse a TCP/5001 hasta que expire el TIME_WAIT (~60 segundos). Durante ese tiempo el sistema no puede transferir mediciones ni stream.

**Propuesta:** Añadir `SO_REUSEADDR` antes del `bind()` y verificar los retornos de `bind()` y `listen()`.

---

### 3. Sin timeout en el handshake ACK

**Dónde:** `slave_client.c` — `do_connect()`

**Qué pasa:** Tras enviar el `SLAVE_PKT_HANDSHAKE`, el esclavo espera el ACK del master con un `recv()` que bloquea sin límite de tiempo. Si el master acepta la conexión TCP pero falla antes de enviar el ACK (crash, reset, OOM), el esclavo queda colgado indefinidamente en esa línea.

```c
while (received < sizeof(ack)) {
    int r = recv(fd, (uint8_t *)&ack + received, sizeof(ack) - received, 0);
    // bloqueo infinito — nunca sale si el master no responde
}
```

**Efecto:** `slave_connect_task` queda bloqueada. El esclavo no puede reconectar ni detectar que algo fue mal. El sistema queda en un estado muerto hasta reinicio manual.

**Propuesta:** Configurar un timeout en el socket con `SO_RCVTIMEO` antes de entrar en el recv(), o usar `select()` con timeout antes de llamar a `recv()`.

---

### 4. MAC inconsistente entre ESP-NOW y TCP/5001

**Dónde:** `slave_client.c` línea 61

**Qué pasa:** El esclavo obtiene su MAC con `WIFI_IF_AP` para enviarla en el handshake TCP. Sin embargo, ESP-NOW opera sobre la interfaz STA. En ESP32, las interfaces AP y STA tienen MACs distintas (difieren típicamente en el último byte).

```c
esp_wifi_get_mac(WIFI_IF_AP, mac);  // MAC del AP
// pero ESP-NOW usa la MAC de STA — son diferentes
```

**Efecto:** El master registra al esclavo con la MAC del AP (desde el handshake TCP) y lo identifica en ESP-NOW con la MAC STA. Internamente los trata como dos dispositivos distintos: uno en `g_nodes[]` (vía ESP-NOW) y otro sin nodo asociado (vía TCP). El `CMD_GET_STATUS` mostraría datos inconsistentes y el reenvío de mediciones a la PC podría fallar.

**Propuesta:** Usar `WIFI_IF_STA` en el handshake TCP para que ambos protocolos usen la misma MAC, verificando que esta sea también la que el master recibe en los mensajes ESP-NOW del mismo nodo.

---

## Severidad Media

### 5. `hdr.length` sin validar antes del `malloc`

**Dónde:** `slave_server.c` — `slave_rx_task`, líneas 89 y 103

**Qué pasa:** Al recibir un paquete, el servidor hace `malloc(hdr.length)` sin comprobar que ese valor sea razonable. Si llega un paquete corrupto (corte de conexión a mitad de transmisión, ruido, bug en el esclavo), `hdr.length` podría ser un número enorme.

```c
measurement_t *meas = malloc(hdr.length);  // sin límite máximo
```

**Efecto:** `malloc` de varios MB → fallo de memoria → `break` → cierre de conexión. En el peor caso, si el heap está fragmentado, podría desestabilizar otras tareas.

**Propuesta:** Validar que `hdr.length` esté dentro de un rango esperado antes del `malloc` (por ejemplo, `hdr.length <= measurement_size(MAX_SAMPLES)`).

---

### 6. `MSG_TRIGGER` sin confirmación de entrega

**Dónde:** `espnow_manager.c` — envío de `MSG_TRIGGER`

**Qué pasa:** ESP-NOW no garantiza entrega. Si el paquete `MSG_TRIGGER` se pierde en el aire (interferencia, colisión), el esclavo nunca se entera del trigger. El master asume que todos los nodos están grabando porque envió el mensaje, pero ese esclavo sigue en IDLE.

**Efecto:** La medición de ese esclavo nunca ocurre. Si la PC pide los datos de ese slot, el esclavo responderá con datos vacíos o del slot anterior. No hay forma de distinguirlo de una grabación exitosa mirando solo el estado del sistema.

**Propuesta:** Usar el `MSG_TRIGGER_ACK` que ya existe: si el master no recibe ACK de un nodo en X ms, reenviar el trigger (con un límite de reintentos).

---

### 7. `EVT_TRIGGER` acumulado durante grabación activa

**Dónde:** `main.c` — `tx_task` y `vigilante_task`

**Qué pasa:** Si llega un segundo trigger (automático o por ESP-NOW) mientras `tx_task` ya está grabando, el bit `EVT_TRIGGER` se setea en el EventGroup. Cuando la grabación termina y `tx_task` vuelve a `xEventGroupWaitBits`, encuentra el bit ya seteado y arranca inmediatamente otra grabación sin que nadie la haya pedido.

**Efecto:** Grabaciones espurias encadenadas. Si hay vibraciones continuas (maquinaria en funcionamiento), el sistema podría entrar en un bucle de grabaciones continuas sin llegar a transmitir ninguna.

**Propuesta:** Limpiar `EVT_TRIGGER` al inicio de cada grabación y añadir un cooldown explícito en `tx_task` (similar al `VIGILANTE_COOLDOWN_MS` del vigilante).

---

## Severidad Baja

### 8. Estado de nodo desactualizado entre heartbeats

**Dónde:** `main.c` — `heartbeat_task` y callbacks TCP

**Qué pasa:** El master actualiza el estado de cada nodo (`status`, `current_slot`) solo cuando recibe un `MSG_HEARTBEAT_ACK`, cada 5 segundos. Entre heartbeats, el estado en `g_nodes[]` puede no reflejar la realidad.

**Efecto:** `CMD_GET_STATUS` puede devolver un nodo como `recording` cuando ya terminó, o como `idle` cuando acaba de empezar. Es inconsistencia informativa, no pérdida de datos.

**Propuesta:** Hacer que los nodos envíen un `MSG_HEARTBEAT_ACK` espontáneo cuando cambian de estado (al iniciar y al finalizar una grabación), sin esperar al próximo heartbeat del master.

---

## Resumen

| # | Problema | Archivo | Severidad |
|---|---|---|---|
| 1 | Conexión semi-abierta | `slave_client.c` | Alta |
| 2 | Sin `SO_REUSEADDR` | `slave_server.c` | Alta |
| 3 | Sin timeout en handshake ACK | `slave_client.c` | Alta |
| 4 | MAC inconsistente AP vs STA | `slave_client.c` | Alta |
| 5 | `hdr.length` sin validar | `slave_server.c` | Media |
| 6 | `MSG_TRIGGER` sin reintento | `espnow_manager.c` | Media |
| 7 | `EVT_TRIGGER` acumulado | `main.c` | Media |
| 8 | Estado desactualizado entre heartbeats | `main.c` | Baja |
