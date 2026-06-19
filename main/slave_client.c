#include "slave_client.h"
#include "protocol.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "SLAVE_CLIENT";

#define MASTER_IP           "192.168.4.1"
#define RECONNECT_DELAY_MS  3000

//ESTADOS INTERNOS

static int       s_fd        = -1;
static volatile bool      s_connected  = false;
static SemaphoreHandle_t    s_mutex = NULL;

//HELPERS
static esp_err_t send_exact(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len){
        int r = send(fd, (const uint8_t *)buf + sent, len - sent, 0);
        if (r <= 0) return ESP_FAIL;
        sent += r;
    }
    return ESP_OK;
}

//CONEXION
static esp_err_t do_connect(void)
{
    //crear socket
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0){
        ESP_LOGE(TAG, "ERROR CREANDO SOCKET");
        return ESP_FAIL;
    }

    //conectar al master
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(SLAVE_TCP_PORT),
    };
    inet_pton(AF_INET, MASTER_IP, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "connect() fallo — master no disponible");
        close(fd);
        return ESP_FAIL;
    }

    // Enviar handshake
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    slave_pkt_header_t hdr = {
        .type   = SLAVE_PKT_HANDSHAKE,
        .length = sizeof(slave_pkt_handshake_t),
    };
    slave_pkt_handshake_t hs;
    memcpy(hs.mac, mac, 6);

    if (send_exact(fd, &hdr, sizeof(hdr)) != ESP_OK ||
        send_exact(fd, &hs,  sizeof(hs))  != ESP_OK) {
        ESP_LOGE(TAG, "Error enviando handshake");
        close(fd);
        return ESP_FAIL;
    }

    // Esperar ACK
    slave_pkt_header_t ack;
    size_t received = 0;
    while (received < sizeof(ack)) {
        int r = recv(fd, (uint8_t *)&ack + received, sizeof(ack) - received, 0);
        if (r <= 0) {
            ESP_LOGE(TAG, "Error esperando ACK");
            close(fd);
            return ESP_FAIL;
        }
        received += r;
    }

    if (ack.type != SLAVE_PKT_HANDSHAKE_ACK) {
        ESP_LOGE(TAG, "ACK invalido: 0x%02x", ack.type);
        close(fd);
        return ESP_FAIL;
    }

    // Conexion establecida
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_fd        = fd;
    s_connected = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Conectado al master");
    return ESP_OK;
}

//TAREA DE CONEXION
static void slave_connect_task(void *arg)
{
    while(1){
        if (do_connect() != ESP_OK){
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
        continue;
        }
        
        //moniterio de conexion
        while(s_connected){
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // Conexion caida — limpiar y reconectar
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_fd >= 0) {
            close(s_fd);
            s_fd = -1;
        }
        xSemaphoreGive(s_mutex);

        ESP_LOGW(TAG, "Conexion caida — reconectando...");
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

//API
esp_err_t slave_client_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_FAIL;

    xTaskCreatePinnedToCore(slave_connect_task, "slave_connect",
                            4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Slave client iniciado");
    return ESP_OK;
}

esp_err_t slave_client_send_measurement(const measurement_t *meas)
{
    if (meas == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_connected) {
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    size_t total = measurement_size(meas->num_samples);
    slave_pkt_header_t hdr = {
        .type   = SLAVE_PKT_MEASUREMENT,
        .length = total,
    };

    esp_err_t ret = send_exact(s_fd, &hdr, sizeof(hdr));
    if (ret == ESP_OK) ret = send_exact(s_fd, meas, total);
    if (ret != ESP_OK) s_connected = false;

    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t slave_client_send_stream_chunk(const icm20948_raw_sample_t *samples,
                                          uint16_t count)
{
    if (samples == NULL || count == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_connected) {
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    size_t payload = count * sizeof(icm20948_raw_sample_t);
    slave_pkt_header_t hdr = {
        .type   = SLAVE_PKT_STREAM_CHUNK,
        .length = payload,
    };

    esp_err_t ret = send_exact(s_fd, &hdr, sizeof(hdr));
    if (ret == ESP_OK) ret = send_exact(s_fd, samples, payload);
    if (ret != ESP_OK) s_connected = false;

    xSemaphoreGive(s_mutex);
    return ret;
}
