# circ_buffer

Buffer circular thread-safe para muestras del acelerómetro. Actúa como ventana deslizante de pre-trigger: siempre contiene las últimas N muestras sin necesitar saber de antemano cuándo va a ocurrir un evento.

---

## Concepto

```
              tail                  head
               │                    │
  [ _ | _ | A | B | C | D | E | F | _ | _ ]
               └────── count ───────┘

  push(): escribe en head, avanza head
          si lleno, avanza tail también (descarta el más antiguo)

  pop():  lee desde tail, avanza tail (consume)

  peek_last(): lee las últimas N desde head hacia atrás, sin mover tail
```

Capacidad: **900 muestras** = 4 segundos a 225 Hz = 5400 bytes en RAM interna.

---

## API

```c
void circ_buffer_init(circ_buffer_t *buf);
```
Inicializa el buffer y crea el mutex FreeRTOS. Llamar antes de lanzar las tareas que lo usan.

```c
void circ_buffer_push(circ_buffer_t *buf,
                      const icm20948_raw_sample_t *samples,
                      uint16_t count);
```
Escribe `count` muestras. Si el buffer está lleno, sobreescribe las más antiguas (avanza `tail`). Thread-safe.

```c
uint16_t circ_buffer_pop(circ_buffer_t *buf,
                         icm20948_raw_sample_t *out,
                         uint16_t max_count);
```
Lee hasta `max_count` muestras y las elimina del buffer. Devuelve cuántas se leyeron realmente. Thread-safe.

```c
uint16_t circ_buffer_peek_last(circ_buffer_t *buf,
                                icm20948_raw_sample_t *out,
                                uint16_t n);
```
Copia las últimas `n` muestras **sin consumirlas** — `tail` no se mueve. Usado por `vigilante_task` para calcular el RMS sin interferir con `tx_task`. Devuelve cuántas muestras se copiaron (puede ser menor a `n` si el buffer tiene menos).

```c
uint16_t circ_buffer_count(circ_buffer_t *buf);
bool     circ_buffer_is_full(circ_buffer_t *buf);
void     circ_buffer_clear(circ_buffer_t *buf);
```

---

## Quién usa qué operación

| Tarea | Operación | Por qué |
|---|---|---|
| `sensor_task` | `push` | Añade muestras del FIFO del sensor |
| `vigilante_task` | `peek_last` | Inspecciona las últimas 22 sin consumir |
| `tx_task` | copia del buffer completo | Congela el pre-trigger al ocurrir un trigger |

---

## Invariante interno

```
head = (tail + count) % CAPACITY
```

`head` siempre apunta a la posición donde se escribirá la **próxima** muestra — no a la última escrita.

---

## Concurrencia

El mutex interno se toma y libera en cada operación. `sensor_task` (Core 1) y `vigilante_task` / `tx_task` (Core 0) pueden llamarlo simultáneamente sin riesgo de corrupción.
