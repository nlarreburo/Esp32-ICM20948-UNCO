#include "circ_buffer.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "CIRCU_BUF";

void circ_buffer_init(circ_buffer_t *buf)
{
    memset(buf->data, 0, sizeof(buf->data)); //Limpiamos el array con 0
    buf->head = 0;  //todos los indice a 0
    buf->tail = 0;
    buf->count = 0;
    buf->mutex = xSemaphoreCreateMutex();   //creamos mutex

    if (buf->mutex == NULL){
        ESP_LOGE(TAG,"Error creado mutex del buffer circular");
    } else {
        ESP_LOGI(TAG,"Buffer circular listo | Capacidad: %d muestras (%d bytes)",
        CIRC_BUFFER_CAPACITY,
        (int)(CIRC_BUFFER_CAPACITY * sizeof(icm20948_raw_sample_t)));
    }
}



void circ_buffer_push(circ_buffer_t *buf, const icm20948_raw_sample_t *samples, uint16_t count)
{
    if (buf == NULL || samples == NULL || count == 0) return;

    //Limitar a la capacidad total del buffer de una sola vez
    //Si intentan pushear mas muestras cuando esta lleno, volvemos a empezar
    if (count>CIRC_BUFFER_CAPACITY){
        samples += (count - CIRC_BUFFER_CAPACITY);
        count = CIRC_BUFFER_CAPACITY;
    }
    
    xSemaphoreTake(buf->mutex, portMAX_DELAY);

    for (uint16_t i=0; i<count; i++){
        
        //Si el buffer esta lleno, re escribimos los datos

        if (buf->count == CIRC_BUFFER_CAPACITY){
            buf->tail = (buf->tail+1)%CIRC_BUFFER_CAPACITY;
        } else {
            buf->count++;
        }

        buf->data[buf->head] = samples[i];
        buf->head = (buf->head + 1) % CIRC_BUFFER_CAPACITY;
    }
    
    xSemaphoreGive(buf->mutex);
}

// t=0: buffer vacio, push(A,B,C,D,E)
// tail=0, head=5, count=5  → [A][B][C][D][E][ ][ ][ ]

// t=1: push(F,G,H), completa el buffer
// H se escribe en data[7], head = (7+1)%8 = 0 (wrappea)
// tail=0, head=0, count=8  → [A][B][C][D][E][F][G][H] buffer lleno

// t=2: push(X) - estado de entrada: tail=0, head=0, count=8
// count == CAPACITY:
//  tail = (0+1)%8 = 1   // A descartada 
//  data[0] = X          // escribe en head=0 
//  head = (0+1)%8 = 1   // head avanza a 1
// buffer -> [X][B][C][D][E][F][G][H]

uint16_t circ_buffer_pop(circ_buffer_t *buf,
                         icm20948_raw_sample_t *out,
                         uint16_t max_count)
{
    if (buf == NULL || out == NULL || max_count == 0) return 0;

    xSemaphoreTake(buf->mutex, portMAX_DELAY);

    //No leer mas de lo que hay disponible
    uint16_t to_read = (max_count < buf->count) ? max_count : buf->count;
    
    for (uint16_t i=0; i < to_read; i++){
        out[i] = buf->data[buf->tail];
        buf->tail = (buf->tail + 1) % CIRC_BUFFER_CAPACITY;
    }
    buf->count -= to_read;

    xSemaphoreGive(buf->mutex);
    return to_read;
}

uint16_t circ_buffer_count(circ_buffer_t *buf)
{
    if (buf == NULL) return 0;
    xSemaphoreTake(buf->mutex, portMAX_DELAY);
    uint16_t c = buf->count;
    xSemaphoreGive(buf->mutex);
    return c;
}

bool circ_buffer_is_full(circ_buffer_t *buf)
{
    if (buf == NULL) return false;
    xSemaphoreTake(buf->mutex, portMAX_DELAY);
    bool full = (buf->count == CIRC_BUFFER_CAPACITY);
    xSemaphoreGive(buf->mutex);
    return full;
}

void circ_buffer_clear(circ_buffer_t *buf)
{
    if (buf == NULL) return;
    xSemaphoreTake(buf->mutex, portMAX_DELAY);
    buf->head  = 0;
    buf->tail  = 0;
    buf->count = 0;
    xSemaphoreGive(buf->mutex);
}

uint16_t circ_buffer_peek_last(circ_buffer_t *buf,
                                icm20948_raw_sample_t *out,
                                uint16_t n)
{
    if (buf == NULL || out == NULL || n == 0) return 0;

    xSemaphoreTake(buf->mutex, portMAX_DELAY);

    // No podemos leer mas de lo que hay
    uint16_t to_read = (n < buf->count) ? n : buf->count;

    // Las ultimas to_read muestras terminan justo antes de head
    // El indice de la mas antigua de ese bloque es:
    //   start = (head - to_read + CAPACITY) % CAPACITY
    //
    // Ejemplo: head=5, to_read=3, CAPACITY=10
    //   start = (5 - 3 + 10) % 10 = 2
    //   leemos data[2], data[3], data[4]  → las 3 mas recientes
    uint16_t start = (buf->head + CIRC_BUFFER_CAPACITY - to_read) % CIRC_BUFFER_CAPACITY;

    for (uint16_t i = 0; i < to_read; i++) {
        out[i] = buf->data[(start + i) % CIRC_BUFFER_CAPACITY];
    }

    xSemaphoreGive(buf->mutex);
    return to_read;
}