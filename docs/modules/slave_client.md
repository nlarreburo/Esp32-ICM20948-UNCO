# slave_client

Cliente TCP/5001 que corre en el **esclavo**. Mantiene la conexión con el master y envía mediciones y chunks de stream cuando `tx_task` los produce.

---

## Responsabilidades

- Conectar al master por TCP/5001 y completar el handshake
- Mantenerse conectado (reconexión automática si se cae la conexión)
- Enviar mediciones completas (`SLAVE_PKT_MEASUREMENT`)
- Enviar chunks de stream (`SLAVE_PKT_STREAM_CHUNK`)

---

## Arquitectura interna

```
slave_client_task  (Core 0, prioridad 3)
  └─ conecta a 192.168.4.1:5001
  └─ envía SLAVE_PKT_HANDSHAKE (mac[6])
  └─ espera SLAVE_PKT_HANDSHAKE_ACK
  └─ queda bloqueado esperando llamadas de tx_task

tx_task (cuando termina grabación o tiene chunk de stream)
  └─ slave_client_send_measurement()  →  SLAVE_PKT_MEASUREMENT
  └─ slave_client_send_stream_chunk() →  SLAVE_PKT_STREAM_CHUNK
```

---

## API

```c
esp_err_t slave_client_init(void);
```
Lanza `slave_client_task`. Debe llamarse en el esclavo después de `wifi_slave_connect()`.

```c
esp_err_t slave_client_send_measurement(const measurement_t *meas);
```
Envía una medición completa al master. Bloquea hasta que se envían todos los bytes o falla la conexión. El master la reenvía a la PC si hay cliente TCP activo.

```c
esp_err_t slave_client_send_stream_chunk(const icm20948_raw_sample_t *samples,
                                          uint16_t count);
```
Envía un chunk de stream al master. Bloquea hasta completar el envío. El master lo reenvía a la PC inmediatamente.

---

## Protocolo de conexión

```
Esclavo → Master  SLAVE_PKT_HANDSHAKE
  header: type=0x01, length=6
  payload: mac[6]

Master → Esclavo  SLAVE_PKT_HANDSHAKE_ACK
  header: type=0x02, length=0
  (sin payload)

[conexión establecida — el esclavo puede enviar datos]
```

Si el master no responde el ACK o la conexión se corta, `slave_client_task` reintenta la conexión automáticamente.

---

## Notas

- No tiene callbacks hacia `main.c` — es un módulo de solo envío.
- El master identifica al esclavo por la MAC del handshake, no por la IP de la conexión TCP.
