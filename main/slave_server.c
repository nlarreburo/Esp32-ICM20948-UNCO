#include "slave_server.h"
#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "SLAVE_SERVER";

#define MAX_SLAVES  4


// ESTADO INTERNO

typedef struct {
    int          fd;
    uint8_t      mac[6];
    TaskHandle_t task;
} slave_conn_t;

static slave_conn_t             s_conns[MAX_SLAVES];    // array de 4 slave_conn_t
static slave_server_callbacks_t s_callbacks;

// HELPERS
// Recibe exactamente 'len' bytes — loopea hasta completar o detectar desconexion
static esp_err_t recv_exact(int fd, void *buf, size_t len) 
{
    size_t received = 0;
    while (received < len) {
        int r = recv(fd, (uint8_t *)buf + received, len - received, 0);
        if (r <= 0) return ESP_FAIL;
        received += r;
    }
    return ESP_OK;
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_SLAVES; i++) {
        if (s_conns[i].fd < 0) return i;
    }
    return -1;
}

static void clear_slot(int idx)
{
    s_conns[idx].fd   = -1;
    s_conns[idx].task = NULL;
    memset(s_conns[idx].mac, 0, 6);
}


// TAREA POR ESCLAVO


static void slave_rx_task(void *arg)
{
    int idx = (int)arg;
    int fd  = s_conns[idx].fd;

    //Handshake: recibir MAC del esclavo
    slave_pkt_header_t hdr;
    slave_pkt_handshake_t hs;

    if (recv_exact(fd, &hdr, sizeof(hdr)) != ESP_OK ||
        hdr.type != SLAVE_PKT_HANDSHAKE    ||
        hdr.length != sizeof(hs)           ||
        recv_exact(fd, &hs, sizeof(hs)) != ESP_OK) {
        ESP_LOGW(TAG, "Handshake fallido en slot %d", idx);
        goto cleanup; //cierra el socket y elimina la tarea
    }

    memcpy(s_conns[idx].mac, hs.mac, 6);
    ESP_LOGI(TAG, "Esclavo conectado: %02x:%02x:%02x:%02x:%02x:%02x",
             hs.mac[0], hs.mac[1], hs.mac[2],
             hs.mac[3], hs.mac[4], hs.mac[5]);

    // Responder ACK
    slave_pkt_header_t ack = { .type = SLAVE_PKT_HANDSHAKE_ACK, .length = 0 };
    send(fd, &ack, sizeof(ack), 0);

    //Loop principal: recibir paquetes
    while (1) {
        if (recv_exact(fd, &hdr, sizeof(hdr)) != ESP_OK) break;

        if (hdr.type == SLAVE_PKT_MEASUREMENT) {
            measurement_t *meas = malloc(hdr.length);
            if (meas == NULL) {
                ESP_LOGE(TAG, "Sin memoria para medicion (%lu bytes)", hdr.length);
                break;
            }
            if (recv_exact(fd, meas, hdr.length) != ESP_OK) {
                free(meas);
                break;
            }
            if (s_callbacks.on_measurement != NULL)
                s_callbacks.on_measurement(s_conns[idx].mac, meas->slot_index, meas);
            free(meas);
        } else if (hdr.type == SLAVE_PKT_STREAM_CHUNK) {
            uint16_t count = hdr.length / sizeof(icm20948_raw_sample_t);
            icm20948_raw_sample_t *samples = malloc(hdr.length);
            if (samples == NULL) {
                ESP_LOGE(TAG, "Sin memoria para chunk (%lu bytes)", hdr.length);
                break;
            }
            if (recv_exact(fd, samples, hdr.length) != ESP_OK) {
                free(samples);
                break;
            }
            if (s_callbacks.on_stream_chunk != NULL)
                s_callbacks.on_stream_chunk(s_conns[idx].mac, samples, count);
            free(samples);
        } else {
            ESP_LOGW(TAG, "Tipo de paquete desconocido: 0x%02x", hdr.type);
            break;
        }
    }

    cleanup:
        ESP_LOGW(TAG, "Esclavo desconectado slot %d", idx);
        close(fd);
        clear_slot(idx);
        vTaskDelete(NULL);
}


// TAREA DE ACEPTACION


static void slave_accept_task(void *arg)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); //creo el sv socket

    struct sockaddr_in addr = { //configuro el sv
        .sin_family      = AF_INET,
        .sin_port        = htons(SLAVE_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)); //asociamos el socket a la config addr
    listen(server_fd, MAX_SLAVES);  //ponemos el socket en modo escucha, acepta hasta maximo MAX_SLAVES

    ESP_LOGI(TAG, "Esperando esclavos en puerto %d", SLAVE_TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr; //struct para accept()
        socklen_t addr_len = sizeof(client_addr);   //tamaño para accept()
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        int idx = find_free_slot();
        if (idx < 0) {
            ESP_LOGW(TAG, "Sin slots libres — rechazando conexion");
            close(client_fd);
            continue;
        }

        struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        s_conns[idx].fd = client_fd;

        char task_name[20];
        snprintf(task_name, sizeof(task_name), "slave_rx_%d", idx);
        xTaskCreatePinnedToCore(slave_rx_task, task_name,
                                4096, (void *)idx, 5, &s_conns[idx].task, 0);
    }
}


// API


esp_err_t slave_server_init(const slave_server_callbacks_t *callbacks)
{
    if (callbacks == NULL) return ESP_ERR_INVALID_ARG;
    memcpy(&s_callbacks, callbacks, sizeof(slave_server_callbacks_t));

    for (int i = 0; i < MAX_SLAVES; i++) clear_slot(i);

    xTaskCreatePinnedToCore(slave_accept_task, "slave_accept",
                            4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Slave server iniciado");
    return ESP_OK;
}




