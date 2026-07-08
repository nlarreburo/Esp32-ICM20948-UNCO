#include "wifi_manager.h"
#include "protocol.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "icm20948.h"

static const char *TAG = "WIFI_MGR";

//----------------------
// ESTADO INTERNO
//----------------------

static wifi_callbacks_t  s_callbacks  = {0};
static int               s_client_fd  = -1;    // socket del cliente activo (-1 = ninguno)
static SemaphoreHandle_t s_client_mutex = NULL; // protege s_client_fd entre tareas
static bool s_server_running = false;
static bool s_sta_active     = false;  // true solo en esclavo (wifi_slave_connect lo activa)

//----------------------
// HELPERS DE SOCKET
//----------------------

// Envia una cadena de texto terminada en \n al socket del cliente.
// Devuelve ESP_OK si se envio correctamente.
static esp_err_t socket_send_str(int fd, const char *str)
{
    if (fd < 0 || str == NULL) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(str);
    int sent = send(fd, str, len, 0);
    if (sent < 0) {
        ESP_LOGW(TAG, "Error enviando al socket: %s", strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

//----------------------
// PARSEO DE COMANDOS
//----------------------
//
// Todos los comandos son texto plano terminado en \n.
// Esta funcion parsea la linea recibida e invoca el callback correspondiente.
// La respuesta al cliente se envia directamente desde aca.

static void handle_command(int fd, char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }

    if (len == 0) return;

    ESP_LOGI(TAG, "Comando recibido: '%s'", line);

    if (strcmp(line, CMD_GET_STATUS) == 0) {
        if (s_callbacks.on_get_status == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        char json[512] = {0};
        esp_err_t ret = s_callbacks.on_get_status(json, sizeof(json));
        if (ret == ESP_OK) {
            char resp[600];
            snprintf(resp, sizeof(resp), RESP_STATUS " %s\n", json);
            socket_send_str(fd, resp);
        } else {
            socket_send_str(fd, RESP_ERROR " error obteniendo estado\n");
        }
        return;
    }

    if (strcmp(line, CMD_START) == 0) {
        if (s_callbacks.on_start == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        esp_err_t ret = s_callbacks.on_start();
        socket_send_str(fd, ret == ESP_OK ? RESP_OK "\n" : RESP_ERROR " fallo el trigger\n");
        return;
    }

    if (strcmp(line, CMD_STOP) == 0) {
        if (s_callbacks.on_stop == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        s_callbacks.on_stop();
        socket_send_str(fd, RESP_OK "\n");
        return;
    }

    if (strncmp(line, CMD_SET_CONFIG, strlen(CMD_SET_CONFIG)) == 0) {
        if (s_callbacks.on_set_config == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        measurement_config_t config;
        if (s_callbacks.on_get_config){
            s_callbacks.on_get_config(&config);
        } else {
            config.pre_trigger_ms     = CONFIG_DEFAULT_PRE_MS;
            config.post_trigger_ms    = CONFIG_DEFAULT_POST_MS;
            config.manual_duration_ms = CONFIG_DEFAULT_MANUAL_MS;
            config.threshold_g        = CONFIG_DEFAULT_THRESHOLD_G;
        }
        uint16_t val_u;
        float    val_f;

        if (sscanf(line, "%*s PRE=%hu",    &val_u) == 1) config.pre_trigger_ms     = val_u;
        if (sscanf(line, "%*s POST=%hu",   &val_u) == 1) config.post_trigger_ms    = val_u;
        if (sscanf(line, "%*s MANUAL=%hu", &val_u) == 1) config.manual_duration_ms = val_u;
        if (sscanf(line, "%*s THR=%f",     &val_f) == 1) config.threshold_g        = val_f;

        esp_err_t ret = s_callbacks.on_set_config(&config);
        socket_send_str(fd, ret == ESP_OK ? RESP_OK "\n" : RESP_ERROR " fallo aplicar config\n");
        return;
    }

    if (strncmp(line, CMD_GET_DATA, strlen(CMD_GET_DATA)) == 0) {
        if (s_callbacks.on_get_data == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        uint8_t mac[6] = {0};
        uint8_t slot   = 0;
        int parsed = sscanf(line,
            CMD_GET_DATA " MAC=%hhx:%hhx:%hhx:%hhx:%hhx:%hhx SLOT=%hhu",
            &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5], &slot);
        if (parsed != 7) {
            socket_send_str(fd, RESP_ERROR " formato invalido\n");
            return;
        }
        esp_err_t ret = s_callbacks.on_get_data(mac, slot, fd);
        if (ret != ESP_OK)
            socket_send_str(fd, RESP_ERROR " fallo obteniendo datos\n");
        return;
    }
    
    if (strcmp(line, CMD_STREAM_START_ALL) == 0) {
        if (s_callbacks.on_stream_start_all == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        esp_err_t ret = s_callbacks.on_stream_start_all();
        socket_send_str(fd, ret == ESP_OK ? RESP_OK "\n" : RESP_ERROR " fallo stream start all\n");
        return;
    }

    if (strcmp(line, CMD_STREAM_STOP_ALL) == 0) {
        if (s_callbacks.on_stream_stop_all == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        s_callbacks.on_stream_stop_all();
        socket_send_str(fd, RESP_OK "\n");
        return;
    }

    if (strncmp(line, CMD_STREAM_START, strlen(CMD_STREAM_START)) == 0) {
        if (s_callbacks.on_stream_start == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        uint8_t mac[6] = {0};
        int parsed = sscanf(line,
            CMD_STREAM_START " MAC=%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
            &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
        if (parsed != 6) {
            socket_send_str(fd, RESP_ERROR " formato invalido. Uso: CMD_STREAM_START MAC=XX:XX:XX:XX:XX:XX\n");
            return;
        }
        esp_err_t ret = s_callbacks.on_stream_start(mac);
        socket_send_str(fd, ret == ESP_OK ? RESP_OK "\n" : RESP_ERROR " fallo stream start\n");
        return;
    }

    if (strcmp(line, CMD_STREAM_STOP) == 0) {
        if (s_callbacks.on_stream_stop == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        s_callbacks.on_stream_stop(NULL);
        socket_send_str(fd, RESP_OK "\n");
        return;
    }

    if (strcmp(line, CMD_SYNC_START) == 0) {
        if (s_callbacks.on_sync_start == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        esp_err_t ret = s_callbacks.on_sync_start();
        socket_send_str(fd, ret == ESP_OK ? RESP_OK "\n" : RESP_ERROR " fallo sync start\n");
        return;
    }

    if (strcmp(line, CMD_SYNC_STOP) == 0) {
        if (s_callbacks.on_sync_stop == NULL) {
            socket_send_str(fd, RESP_ERROR " callback no registrado\n");
            return;
        }
        s_callbacks.on_sync_stop();
        socket_send_str(fd, RESP_OK "\n");
        return;
    }

    char resp[64];
    snprintf(resp, sizeof(resp), RESP_ERROR " comando desconocido: '%s'\n", line);
    socket_send_str(fd, resp);
}

//----------------------
// TAREA DEL CLIENTE TCP
//----------------------
//
// Se crea una instancia de esta tarea por cada cliente que se conecta.
// Lee lineas del socket y las pasa a handle_command().
// Cuando el cliente se desconecta, la tarea se elimina sola.

static void client_task(void *pvParameters)
{
    int fd = (int)(intptr_t)pvParameters;

    ESP_LOGI(TAG, "Cliente conectado (fd=%d)", fd);

    // Registrar el fd del cliente activo
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client_fd = fd;
    xSemaphoreGive(s_client_mutex);

    char buf[256] = {0};
    int  buf_pos  = 0;

    while (1) {
        // Leer bytes del socket de a uno hasta encontrar \n
        // Esto permite recibir comandos de cualquier longitud
        char c;
        int n = recv(fd, &c, 1, 0);

        if (n <= 0) {
            // n == 0: cliente cerro la conexion limpiamente
            // n < 0: error de red
            if (n < 0) {
                ESP_LOGW(TAG, "Error de socket: %s", strerror(errno));
            }
            ESP_LOGI(TAG, "Cliente desconectado (fd=%d)", fd);
            break;
        }

        if (c == '\n') {
            // Linea completa — procesar
            buf[buf_pos] = '\0';
            handle_command(fd, buf);
            buf_pos = 0;
            memset(buf, 0, sizeof(buf));
        } else if (buf_pos < (int)sizeof(buf) - 1) {
            buf[buf_pos++] = c;
        } else {
            // Linea demasiado larga — descartar y resetear
            ESP_LOGW(TAG, "Linea demasiado larga, descartando");
            buf_pos = 0;
            memset(buf, 0, sizeof(buf));
        }
    }

    // Limpiar al desconectarse
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client_fd == fd) {
        s_client_fd = -1;
    }
    xSemaphoreGive(s_client_mutex);


    close(fd);
    vTaskDelete(NULL);
}

//----------------------
// TAREA DEL SERVIDOR TCP
//----------------------
//
// Escucha en WIFI_TCP_PORT y acepta conexiones entrantes.
// Solo permite una conexion a la vez (WIFI_AP_MAX_CONN = 1).
// Por cada cliente crea una client_task independiente.

static void server_task(void *pvParameters)
{
    // Crear socket TCP
    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "Error creando socket: %s", strerror(errno));
        vTaskDelete(NULL);
        return;
    }

    // SO_REUSEADDR: permite reusar el puerto inmediatamente despues de reiniciar
    // Sin esto hay que esperar ~60 segundos antes de poder volver a bindear
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bindear al puerto
    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(WIFI_TCP_PORT),
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Error en bind: %s", strerror(errno));
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    // Escuchar conexiones entrantes (backlog = 1 — solo 1 en cola)
    if (listen(server_fd, 1) < 0) {
        ESP_LOGE(TAG, "Error en listen: %s", strerror(errno));
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Servidor TCP escuchando en %s:%d", WIFI_AP_IP, WIFI_TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // accept() bloquea hasta que un cliente se conecta
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            ESP_LOGW(TAG, "Error en accept: %s", strerror(errno));
            continue;
        }

        // Si ya hay un cliente conectado, rechazar la nueva conexion
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        bool hay_cliente = (s_client_fd >= 0);
        xSemaphoreGive(s_client_mutex);

        if (hay_cliente) {
            ESP_LOGW(TAG, "Ya hay un cliente conectado, rechazando nueva conexion");
            socket_send_str(client_fd, RESP_ERROR " ya hay un cliente conectado\n");
            close(client_fd);
            continue;
        }

        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Crear tarea para manejar este cliente
        // Se pasa el fd como puntero (cast de int a intptr_t)
        xTaskCreatePinnedToCore(client_task, "wifi_client",
                                4096, (void *)(intptr_t)client_fd,
                                3, NULL, 0);
    }
}

//----------------------
// EVENTOS WIFI
//----------------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                // La conexion inicial la hace wifi_slave_connect() explicitamente,
                // despues de set_config(). El reconnect lo maneja STA_DISCONNECTED.
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Conectado al master AP");
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (!s_sta_active) break;
                ESP_LOGW(TAG, "Desconectado del master AP — reintentando...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP iniciado — SSID: %s | IP: %s | Puerto: %d",
                         WIFI_AP_SSID, WIFI_AP_IP, WIFI_TCP_PORT);
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "PC conectada al AP — MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                         e->mac[0], e->mac[1], e->mac[2],
                         e->mac[3], e->mac[4], e->mac[5]);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "PC desconectada del AP — MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                         e->mac[0], e->mac[1], e->mac[2],
                         e->mac[3], e->mac[4], e->mac[5]);
                break;
            }
            default:
                break;
        }
    }
}

//----------------------
// API
//----------------------

esp_err_t wifi_base_init(void)
{
    // Inicializar netif y driver WiFi — necesario para ESP-NOW en ambos nodos
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Registrar handler de eventos WiFi
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // Configurar AP oculto sin contraseña — solo para que ESP-NOW funcione
    // El esclavo no necesita que la PC se conecte
    wifi_config_t wifi_config = {
        .ap = {
            .ssid           = "ESP_NOW_SLAVE",
            .ssid_len       = 0,
            .channel        = WIFI_AP_CHANNEL,
            .password       = "",
            .max_connection = 0,          // no aceptar conexiones
            .authmode       = WIFI_AUTH_OPEN,
            .ssid_hidden    = 1,          // ← AP oculto, no visible
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi base inicializado");
    return ESP_OK;
}

esp_err_t wifi_manager_init(const wifi_callbacks_t *callbacks)
{
    if (callbacks == NULL) return ESP_ERR_INVALID_ARG;

    memcpy(&s_callbacks, callbacks, sizeof(wifi_callbacks_t));

    // Transicion esclavo → master: cancelar reconexion STA y volver a AP puro
    if (s_sta_active) {
        s_sta_active = false;
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    // Crear mutex solo si no existe
    if (s_client_mutex == NULL) {
        s_client_mutex = xSemaphoreCreateMutex();
        if (s_client_mutex == NULL) return ESP_FAIL;
    }

    // Configurar AP con SSID visible — solo el master hace esto
    wifi_config_t wifi_config = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .password       = WIFI_AP_PASSWORD,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    // Lanzar servidor TCP en Core 0 solo si no esta corriendo ya
    if (!s_server_running){
        s_server_running = true;
        xTaskCreatePinnedToCore(server_task, "wifi_server",
                                4096, NULL, 3, NULL, 0);
    }

    return ESP_OK;
}

esp_err_t wifi_slave_connect(void)
{
    s_sta_active = true;

    // STA netif necesario para recibir IP del DHCP del master y poder usar TCP
    esp_netif_create_default_wifi_sta();

    // 1. Activar APSTA primero — crea la interfaz STA (AP oculto sigue para ESP-NOW)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // 2. Ahora si existe WIFI_IF_STA — configurar credenciales
    //    El handler STA_START ya NO llama esp_wifi_connect(), asi que no hay race.
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_AP_SSID,
            .password = WIFI_AP_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // 3. Conectar explicitamente una vez que el config esta cargado
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Esclavo conectando al AP: %s", WIFI_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_manager_send_event(const char *event_str)
{
    if (event_str == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    int fd = s_client_fd;
    xSemaphoreGive(s_client_mutex);

    if (fd < 0) return ESP_OK;  // no hay PC conectada, descartar silenciosamente

    return socket_send_str(fd, event_str);
}

esp_err_t wifi_manager_send_raw(const void *data, size_t len)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    int fd = s_client_fd;

    if (fd < 0) {
        xSemaphoreGive(s_client_mutex);
        return ESP_FAIL;
    }

    size_t sent = 0;
    while (sent < len) {
        int r = send(fd, (const uint8_t *)data + sent, len - sent, 0);
        if (r <= 0) {
            xSemaphoreGive(s_client_mutex);
            return ESP_FAIL;
        }
        sent += r;
    }

    xSemaphoreGive(s_client_mutex);
    return ESP_OK;
}

esp_err_t wifi_manager_send_stream_chunk(const icm20948_raw_sample_t *samples, uint16_t count)
{
    if (samples == NULL || count == 0) return ESP_ERR_INVALID_ARG;

    size_t payload = count * sizeof(icm20948_raw_sample_t);
    size_t total = sizeof(stream_chunk_hdr_t) + payload;
    uint8_t *buf = malloc(total);
    if (buf == NULL) return ESP_FAIL;

    stream_chunk_hdr_t hdr = {
        .type = STREAM_PKT_CHUNK,
        .count = count,
    };

    //mac del master
    esp_wifi_get_mac(WIFI_IF_AP, hdr.mac);
                                                        //buf:
    memcpy(buf, &hdr, sizeof(hdr));                     //| hdr (9 bytes) | samples (270 bytes) |
    memcpy(buf + sizeof(hdr), samples, payload);        //^               ^
                                                        //buf             buf + sizeof(hdr)
    esp_err_t ret = wifi_manager_send_raw(buf, total);
    free(buf);
    return ret;
}

bool wifi_manager_is_connected(void)
{
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    bool connected = (s_client_fd >= 0);
    xSemaphoreGive(s_client_mutex);
    return connected;
}