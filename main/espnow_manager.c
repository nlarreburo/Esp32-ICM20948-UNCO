#include "espnow_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ESPNOW_MGR";

//----------------------
// ESTADO INTERNO
//----------------------

static espnow_callbacks_t s_callbacks  = {0};
static uint8_t            s_own_mac[6] = {0};

// Cola de recepcion — el callback de ESP-NOW deposita mensajes aca
// y espnow_rx_task los consume
typedef struct {
    uint8_t  src_mac[6];
    uint8_t  data[ESP_NOW_MAX_DATA_LEN];  // 250 bytes
    uint8_t  len;
} espnow_rx_item_t;

static QueueHandle_t s_rx_queue = NULL;

//----------------------
// HELPER — completar header y enviar
//----------------------
//
// Todas las funciones de envio construyen su struct, llaman a espnow_send_raw
// que completa src_mac y hace la llamada a esp_now_send.

static esp_err_t espnow_send_raw(const uint8_t dst_mac[6],
                                  const void *data,
                                  size_t len)
{
    if (dst_mac == NULL || data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > ESP_NOW_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Mensaje demasiado grande: %d bytes (max %d)", len, ESP_NOW_MAX_DATA_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    // Completar src_mac en el header
    // El header siempre es el primer campo de cualquier struct de mensaje
    msg_header_t *hdr = (msg_header_t *)data;
    memcpy(hdr->src_mac, s_own_mac, 6);

    esp_err_t ret = esp_now_send(dst_mac, (const uint8_t *)data, len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send error: %s", esp_err_to_name(ret));
    }
    return ret;
}

//----------------------
// CALLBACKS INTERNOS DE ESP-NOW
//----------------------

// Se invoca cuando un mensaje fue enviado — confirma entrega al driver
// No confundir con confirmacion de recepcion por el peer (eso requeriria ACK)
// ESP-IDF v5.5.1 cambio la firma del send callback:
// antes: (const uint8_t *mac_addr, esp_now_send_status_t status)
// ahora: (const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
static void espnow_send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Envio fallido a %02x:%02x:%02x:%02x:%02x:%02x",
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    }
}

// Se invoca cuando llega un mensaje ESP-NOW
// IMPORTANTE: corre en contexto WiFi con stack limitado
// Solo copia el mensaje a la cola y retorna inmediatamente
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data,
                            int len)
{
    if (recv_info == NULL || data == NULL || len <= 0) return;
    if (len > ESP_NOW_MAX_DATA_LEN) return;

    ESP_LOGI("ESPNOW", "PKT recibido de %02x:%02x:%02x:%02x:%02x:%02x len=%d tipo=%d",
             recv_info->src_addr[0], recv_info->src_addr[1],
             recv_info->src_addr[2], recv_info->src_addr[3],
             recv_info->src_addr[4], recv_info->src_addr[5],
             len, data[0]);

    espnow_rx_item_t item = {0};
    memcpy(item.src_mac, recv_info->src_addr, 6);
    memcpy(item.data, data, len);
    item.len = (uint8_t)len;

    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) { //envio a la cola creada
        ESP_LOGW(TAG, "Cola llena — paquete descartado tipo=%d", data[0]);
    }
}

//----------------------
// TAREA DE RECEPCION
//----------------------
// Lee mensajes de la cola y los despacha al callback correspondiente
// segun msg_type. Corre en Core 0 con prioridad baja.

static void espnow_rx_task(void *pvParameters)
{
    espnow_rx_item_t item;

    while (1) {
        // Bloquear hasta que llegue un mensaje en la cola
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.len < sizeof(msg_header_t)) continue;

        const msg_header_t *hdr = (const msg_header_t *)item.data;

        ESP_LOGD(TAG, "Mensaje recibido tipo=0x%02x de %02x:%02x:%02x:%02x:%02x:%02x",
                 hdr->msg_type,
                 hdr->src_mac[0], hdr->src_mac[1], hdr->src_mac[2],
                 hdr->src_mac[3], hdr->src_mac[4], hdr->src_mac[5]);

        // Despachar al callback registrado segun el tipo de mensaje
        switch (hdr->msg_type) {

            case MSG_TRIGGER:
                if (s_callbacks.on_trigger && item.len >= sizeof(msg_trigger_t))
                    s_callbacks.on_trigger((const msg_trigger_t *)item.data);
                break;

            case MSG_TRIGGER_ACK:
                if (s_callbacks.on_trigger_ack && item.len >= sizeof(msg_trigger_ack_t))
                    s_callbacks.on_trigger_ack((const msg_trigger_ack_t *)item.data);
                break;

            case MSG_TRIGGER_NOTIFY:
                if (s_callbacks.on_trigger_notify && item.len >= sizeof(msg_trigger_notify_t))
                    s_callbacks.on_trigger_notify((const msg_trigger_notify_t *)item.data);
                break;

            case MSG_STOP:
                if (s_callbacks.on_stop)
                    s_callbacks.on_stop((const msg_stop_t *)item.data);
                break;

            case MSG_STOP_ACK:
                if (s_callbacks.on_stop_ack && item.len >= sizeof(msg_stop_ack_t))
                    s_callbacks.on_stop_ack((const msg_stop_ack_t *)item.data);
                break;

            case MSG_DATA_REQUEST:
                if (s_callbacks.on_data_request)
                    s_callbacks.on_data_request((const msg_data_request_t *)item.data);
                break;

            case MSG_CONFIG:
                // MSG_CONFIG no tiene callback propio — el nodo lo aplica directamente
                // En una implementacion mas completa se agregaria on_config
                ESP_LOGI(TAG, "MSG_CONFIG recibido");
                if (s_callbacks.on_config && item.len >= sizeof(msg_config_t))
                    s_callbacks.on_config((const msg_config_t *)item.data);
                break;

            case MSG_CONFIG_ACK:
                if (s_callbacks.on_config_ack && item.len >= sizeof(msg_config_ack_t))
                    s_callbacks.on_config_ack((const msg_config_ack_t *)item.data);
                break;

            case MSG_HEARTBEAT:
                if (s_callbacks.on_heartbeat && item.len >= sizeof(msg_heartbeat_t))
                    s_callbacks.on_heartbeat((const msg_heartbeat_t *)item.data);
                break;

            case MSG_HEARTBEAT_ACK:
                if (s_callbacks.on_heartbeat_ack && item.len >= sizeof(msg_heartbeat_ack_t))
                    s_callbacks.on_heartbeat_ack((const msg_heartbeat_ack_t *)item.data);
                break;

            case MSG_NEW_MASTER:
                if (s_callbacks.on_new_master && item.len >= sizeof(msg_new_master_t))
                    s_callbacks.on_new_master((const msg_new_master_t *)item.data);
                break;

            case MSG_NEW_MASTER_ACK:
                if (s_callbacks.on_new_master_ack && item.len >= sizeof(msg_new_master_ack_t))
                    s_callbacks.on_new_master_ack((const msg_new_master_ack_t *)item.data);
                break;

            case MSG_STREAM_START:
                if (s_callbacks.on_stream_start)
                    s_callbacks.on_stream_start((const msg_stream_start_t *)item.data);
                break;

            case MSG_STREAM_STOP:
                if (s_callbacks.on_stream_stop)
                    s_callbacks.on_stream_stop((const msg_stream_stop_t *)item.data);
                break;

            case MSG_SYNC_START:
                if (s_callbacks.on_sync_start)
                    s_callbacks.on_sync_start((const msg_sync_start_t *)item.data);
                break;

            case MSG_SYNC_STOP:
                if (s_callbacks.on_sync_stop)
                    s_callbacks.on_sync_stop((const msg_sync_stop_t *)item.data);
                break;

            default:
                ESP_LOGW(TAG, "Tipo de mensaje desconocido: 0x%02x", hdr->msg_type);
                break;
        }
    }
}

//----------------------
// API — inicializacion
//----------------------

esp_err_t espnow_manager_init(const espnow_callbacks_t *callbacks)
{
    if (callbacks == NULL) return ESP_ERR_INVALID_ARG;
    memcpy(&s_callbacks, callbacks, sizeof(espnow_callbacks_t));

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    esp_wifi_get_mac(mode == WIFI_MODE_STA ? WIFI_IF_STA : WIFI_IF_AP, s_own_mac);
    ESP_LOGI(TAG, "MAC propia: %02x:%02x:%02x:%02x:%02x:%02x",
             s_own_mac[0], s_own_mac[1], s_own_mac[2],
             s_own_mac[3], s_own_mac[4], s_own_mac[5]);

    // Crear cola de recepcion
    s_rx_queue = xQueueCreate(ESPNOW_RX_QUEUE_SIZE, sizeof(espnow_rx_item_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "Error creando cola de recepcion");
        return ESP_FAIL;
    }

    // Inicializar ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb)); //accion luego de transmitir dato 
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb)); //accion luego de recibo dato 

    // Registrar peer de broadcast — permite enviar a todos sin registrar cada uno
    esp_now_peer_info_t broadcast_peer = {
        .channel  = ESPNOW_CHANNEL,
        .ifidx    = mode == WIFI_MODE_STA ? WIFI_IF_STA : WIFI_IF_AP,
        .encrypt  = false,
    };
    memcpy(broadcast_peer.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&broadcast_peer));

    // Lanzar tarea de recepcion en Core 0, prioridad 2
    xTaskCreatePinnedToCore(espnow_rx_task, "espnow_rx",
                            4096, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "ESP-NOW inicializado");
    return ESP_OK;
}

void espnow_manager_get_own_mac(uint8_t mac_out[6])
{
    memcpy(mac_out, s_own_mac, 6);
}

void espnow_manager_update_callbacks(const espnow_callbacks_t *callbacks)
{
    if (callbacks == NULL) return;
    memcpy(&s_callbacks, callbacks, sizeof(espnow_callbacks_t));
    ESP_LOGI(TAG, "Callbacks actualizados");
}

//----------------------
// API — gestion de peers
//----------------------

esp_err_t espnow_manager_add_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return ESP_OK;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    esp_now_peer_info_t peer = {
        .channel = ESPNOW_CHANNEL,
        .ifidx   = (mode == WIFI_MODE_STA) ? WIFI_IF_STA : WIFI_IF_AP,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, 6);

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Peer agregado: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return ret;
}

esp_err_t espnow_manager_remove_peer(const uint8_t mac[6])
{
    if (!esp_now_is_peer_exist(mac)) return ESP_OK;
    return esp_now_del_peer(mac);
}

bool espnow_manager_peer_exists(const uint8_t mac[6])
{
    return esp_now_is_peer_exist(mac);
}

//----------------------
// API — envio de mensajes
//----------------------

esp_err_t espnow_send_trigger(const uint8_t dst_mac[6],
                               uint8_t slot_index,
                               uint8_t trigger_source,
                               uint32_t timestamp_ms)
{
    msg_trigger_t msg = {
        .header        = { .msg_type = MSG_TRIGGER },
        .slot_index    = slot_index,
        .trigger_source = trigger_source,
        .timestamp_ms  = timestamp_ms,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_trigger_ack(const uint8_t dst_mac[6],
                                   uint8_t slot_index,
                                   uint32_t timestamp_ms)
{
    msg_trigger_ack_t msg = {
        .header       = { .msg_type = MSG_TRIGGER_ACK },
        .slot_index   = slot_index,
        .timestamp_ms = timestamp_ms,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_trigger_notify(const uint8_t dst_mac[6],
                                      float rms_g,
                                      uint32_t timestamp_ms)
{
    msg_trigger_notify_t msg = {
        .header       = { .msg_type = MSG_TRIGGER_NOTIFY },
        .rms_g        = rms_g,
        .timestamp_ms = timestamp_ms,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_stop(const uint8_t dst_mac[6])
{
    msg_stop_t msg = { .header = { .msg_type = MSG_STOP } };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_stop_ack(const uint8_t dst_mac[6], uint8_t status)
{
    msg_stop_ack_t msg = {
        .header = { .msg_type = MSG_STOP_ACK },
        .status = status,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_data_request(const uint8_t dst_mac[6], uint8_t slot_index)
{
    msg_data_request_t msg = {
        .header     = { .msg_type = MSG_DATA_REQUEST },
        .slot_index = slot_index,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_config(const uint8_t dst_mac[6],
                              const measurement_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    msg_config_t msg = {
        .header = { .msg_type = MSG_CONFIG },
        .config = *config,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_config_ack(const uint8_t dst_mac[6], uint8_t ok)
{
    msg_config_ack_t msg = {
        .header = { .msg_type = MSG_CONFIG_ACK },
        .ok     = ok,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_heartbeat(const uint8_t dst_mac[6], uint32_t timestamp_ms)
{
    msg_heartbeat_t msg = {
        .header       = { .msg_type = MSG_HEARTBEAT },
        .timestamp_ms = timestamp_ms,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_heartbeat_ack(const uint8_t dst_mac[6],
                                     node_status_t status,
                                     uint8_t current_slot,
                                     uint32_t free_heap)
{
    msg_heartbeat_ack_t msg = {
        .header       = { .msg_type = MSG_HEARTBEAT_ACK },
        .status       = status,
        .current_slot = current_slot,
        .free_heap    = free_heap,
    };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_new_master(const uint8_t dst_mac[6])
{
    msg_new_master_t msg = { .header = { .msg_type = MSG_NEW_MASTER } };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_stream_start(const uint8_t dst_mac[6])
{
    msg_stream_start_t msg = { .header = { .msg_type = MSG_STREAM_START } };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_stream_stop(const uint8_t dst_mac[6])
{
    msg_stream_stop_t msg = { .header = { .msg_type = MSG_STREAM_STOP } };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_new_master_ack(const uint8_t dst_mac[6])
{
    msg_new_master_ack_t msg = { .header = { .msg_type = MSG_NEW_MASTER_ACK } };
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_sync_start(const uint8_t dst_mac[6])
{
    msg_sync_start_t msg = { .header = { .msg_type = MSG_SYNC_START}};
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}

esp_err_t espnow_send_sync_stop(const uint8_t dst_mac[6])
{
    msg_sync_start_t msg = { .header = { .msg_type = MSG_SYNC_STOP}};
    return espnow_send_raw(dst_mac, &msg, sizeof(msg));
}