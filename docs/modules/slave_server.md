# slave_server

Servidor TCP/5001 que corre en el **master**. Acepta conexiones de esclavos, verifica el handshake y notifica a `main.c` cuando llegan mediciones o chunks de stream.

---

## Responsabilidades

- Escuchar en el puerto 5001
- Verificar el handshake de cada esclavo que conecta
- Recibir mediciones completas y chunks de stream
- Notificar a `main.c` vía callbacks para que los reenvíe a la PC

---

## Arquitectura interna

```
slave_server_task  (Core 0, prioridad 3)
  └─ listen(5001)
  └─ accept()  → nueva conexión de un esclavo
  └─ lee SLAVE_PKT_HANDSHAKE → extrae mac[6]
  └─ responde SLAVE_PKT_HANDSHAKE_ACK
  └─ bucle de recepción:
       └─ lee slave_pkt_header_t (5 bytes)
       └─ lee payload (length bytes)
       └─ switch(type):
            SLAVE_PKT_MEASUREMENT   → s_callbacks.on_measurement(mac, slot, meas)
            SLAVE_PKT_STREAM_CHUNK  → s_callbacks.on_stream_chunk(mac, samples, count)
```

---

## API

```c
esp_err_t slave_server_init(const slave_server_callbacks_t *callbacks);
```
Lanza `slave_server_task`. Debe llamarse en el master después de `wifi_manager_init()`.

---

## Callbacks registrados por main.c

```c
typedef struct {
    slave_cb_measurement_t  on_measurement;   // medición completa recibida
    slave_cb_stream_chunk_t on_stream_chunk;  // chunk de stream recibido
} slave_server_callbacks_t;
```

```c
// Firmas de los callbacks
void on_measurement (const uint8_t *mac, uint8_t slot, const measurement_t *meas);
void on_stream_chunk(const uint8_t *mac, const icm20948_raw_sample_t *samples, uint16_t count);
```

`main.c` en estos callbacks reenvía los datos a la PC por TCP/5000 usando `wifi_manager_send_raw()`.

---

## Notas

- A diferencia de TCP/5000, este puerto no tiene límite de una conexión — puede aceptar varios esclavos simultáneamente (uno por tarea interna).
- El módulo no almacena las mediciones — las pasa directamente al callback. Es responsabilidad de `main.c` escribirlas en `g_slots[]` si es necesario.
