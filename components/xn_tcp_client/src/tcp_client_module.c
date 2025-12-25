/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-12-25 00:00:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-12-25 00:00:00
 * @FilePath: \xn_ota_manger\components\xn_tcp_client\src\tcp_client_module.c
 * @Description: TCP 客户端模块实现
 */

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "tcp_client_module.h"

static const char *TAG = "tcp_client";

/* 模块内部状态 */
static tcp_client_config_t s_config;
static tcp_client_state_t  s_state       = TCP_CLIENT_STATE_DISCONNECTED;
static int                 s_socket      = -1;
static bool                s_initialized = false;
static TaskHandle_t        s_recv_task   = NULL;
static SemaphoreHandle_t   s_mutex       = NULL;
static bool                s_task_running = false;

/* 前向声明 */
static void tcp_client_recv_task(void *arg);
static void tcp_client_notify_event(tcp_client_event_t event, uint8_t *data, size_t len, int err);

/* -------------------- 内部辅助函数 -------------------- */

/**
 * @brief 通知上层事件
 */
static void tcp_client_notify_event(tcp_client_event_t event, uint8_t *data, size_t len, int err)
{
    if (s_config.event_cb == NULL) {
        return;
    }

    tcp_client_event_data_t event_data = {
        .event      = event,
        .data       = data,
        .data_len   = len,
        .error_code = err,
    };

    s_config.event_cb(&event_data);
}

/**
 * @brief 更新连接状态
 */
static void tcp_client_set_state(tcp_client_state_t new_state)
{
    s_state = new_state;
}

/**
 * @brief 接收任务
 */
static void tcp_client_recv_task(void *arg)
{
    (void)arg;

    uint8_t *recv_buf = (uint8_t *)malloc(s_config.recv_buf_size);
    if (recv_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate recv buffer");
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    s_task_running = true;

    while (s_task_running && s_state == TCP_CLIENT_STATE_CONNECTED) {
        int len = recv(s_socket, recv_buf, s_config.recv_buf_size, 0);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 超时，继续等待 */
                continue;
            }
            /* 接收错误 */
            ESP_LOGE(TAG, "recv error: errno=%d", errno);
            tcp_client_notify_event(TCP_CLIENT_EVENT_ERROR, NULL, 0, errno);
            break;
        } else if (len == 0) {
            /* 连接被对端关闭 */
            ESP_LOGI(TAG, "Connection closed by peer");
            break;
        } else {
            /* 收到数据，通知上层 */
            tcp_client_notify_event(TCP_CLIENT_EVENT_DATA_RECEIVED, recv_buf, (size_t)len, 0);
        }
    }

    free(recv_buf);

    /* 如果是因为连接断开退出，更新状态并通知 */
    if (s_state == TCP_CLIENT_STATE_CONNECTED) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_socket >= 0) {
                close(s_socket);
                s_socket = -1;
            }
            tcp_client_set_state(TCP_CLIENT_STATE_DISCONNECTED);
            xSemaphoreGive(s_mutex);
        }
        tcp_client_notify_event(TCP_CLIENT_EVENT_DISCONNECTED, NULL, 0, 0);

        /* 自动重连逻辑 */
        if (s_config.auto_reconnect && s_initialized) {
            vTaskDelay(pdMS_TO_TICKS(s_config.reconnect_interval_ms));
            if (s_initialized) {
                tcp_client_connect();
            }
        }
    }

    s_task_running = false;
    s_recv_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------- 对外 API 实现 -------------------- */

esp_err_t tcp_client_init(const tcp_client_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* 使用默认配置或上层传入配置 */
    if (config == NULL) {
        s_config = TCP_CLIENT_DEFAULT_CONFIG();
    } else {
        s_config = *config;
    }

    /* 创建互斥锁 */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_socket      = -1;
    s_state       = TCP_CLIENT_STATE_DISCONNECTED;
    s_initialized = true;

    ESP_LOGI(TAG, "TCP client initialized");
    return ESP_OK;
}

esp_err_t tcp_client_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* 停止接收任务 */
    s_task_running = false;
    s_initialized  = false;

    /* 断开连接 */
    tcp_client_disconnect();

    /* 等待任务退出 */
    if (s_recv_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 删除互斥锁 */
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "TCP client deinitialized");
    return ESP_OK;
}

esp_err_t tcp_client_set_server(const char *ip, uint16_t port)
{
    if (ip == NULL || ip[0] == '\0' || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strncpy(s_config.server_ip, ip, sizeof(s_config.server_ip) - 1);
    s_config.server_ip[sizeof(s_config.server_ip) - 1] = '\0';
    s_config.server_port = port;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Server set to %s:%u", s_config.server_ip, s_config.server_port);
    return ESP_OK;
}

esp_err_t tcp_client_connect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_config.server_ip[0] == '\0' || s_config.server_port == 0) {
        ESP_LOGE(TAG, "Server address not set");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_state == TCP_CLIENT_STATE_CONNECTED || s_state == TCP_CLIENT_STATE_CONNECTING) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    tcp_client_set_state(TCP_CLIENT_STATE_CONNECTING);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Connecting to %s:%u...", s_config.server_ip, s_config.server_port);

    /* 解析地址 */
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", s_config.server_port);

    int err = getaddrinfo(s_config.server_ip, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed: %d", err);
        tcp_client_set_state(TCP_CLIENT_STATE_ERROR);
        tcp_client_notify_event(TCP_CLIENT_EVENT_ERROR, NULL, 0, err);
        return ESP_FAIL;
    }

    /* 创建 socket */
    s_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno=%d", errno);
        freeaddrinfo(res);
        tcp_client_set_state(TCP_CLIENT_STATE_ERROR);
        tcp_client_notify_event(TCP_CLIENT_EVENT_ERROR, NULL, 0, errno);
        return ESP_FAIL;
    }

    /* 设置接收超时 */
    struct timeval timeout = {
        .tv_sec  = s_config.recv_timeout_ms / 1000,
        .tv_usec = (s_config.recv_timeout_ms % 1000) * 1000,
    };
    setsockopt(s_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* 连接服务端 */
    err = connect(s_socket, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (err != 0) {
        ESP_LOGE(TAG, "Socket connect failed: errno=%d", errno);
        close(s_socket);
        s_socket = -1;
        tcp_client_set_state(TCP_CLIENT_STATE_ERROR);
        tcp_client_notify_event(TCP_CLIENT_EVENT_ERROR, NULL, 0, errno);
        return ESP_FAIL;
    }

    tcp_client_set_state(TCP_CLIENT_STATE_CONNECTED);
    ESP_LOGI(TAG, "Connected to %s:%u", s_config.server_ip, s_config.server_port);

    /* 通知连接成功 */
    tcp_client_notify_event(TCP_CLIENT_EVENT_CONNECTED, NULL, 0, 0);

    /* 创建接收任务 */
    if (s_config.event_cb != NULL && s_recv_task == NULL) {
        BaseType_t ret = xTaskCreate(
            tcp_client_recv_task,
            "tcp_recv",
            4096,
            NULL,
            tskIDLE_PRIORITY + 2,
            &s_recv_task);

        if (ret != pdPASS) {
            ESP_LOGW(TAG, "Failed to create recv task");
        }
    }

    return ESP_OK;
}

esp_err_t tcp_client_disconnect(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* 停止接收任务 */
    s_task_running = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_socket >= 0) {
        shutdown(s_socket, SHUT_RDWR);
        close(s_socket);
        s_socket = -1;
    }

    tcp_client_set_state(TCP_CLIENT_STATE_DISCONNECTED);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

esp_err_t tcp_client_send(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_state != TCP_CLIENT_STATE_CONNECTED || s_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = send(s_socket, data, data_len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Send failed: errno=%d", errno);
        tcp_client_notify_event(TCP_CLIENT_EVENT_ERROR, NULL, 0, errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sent %d bytes", sent);
    return ESP_OK;
}

esp_err_t tcp_client_recv(uint8_t *buf, size_t buf_size, size_t *recv_len, int timeout_ms)
{
    if (buf == NULL || buf_size == 0 || recv_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_state != TCP_CLIENT_STATE_CONNECTED || s_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 设置临时超时 */
    int use_timeout = (timeout_ms > 0) ? timeout_ms : s_config.recv_timeout_ms;
    struct timeval timeout = {
        .tv_sec  = use_timeout / 1000,
        .tv_usec = (use_timeout % 1000) * 1000,
    };
    setsockopt(s_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int len = recv(s_socket, buf, buf_size, 0);

    if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *recv_len = 0;
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "Recv failed: errno=%d", errno);
        return ESP_FAIL;
    } else if (len == 0) {
        /* 连接被关闭 */
        *recv_len = 0;
        return ESP_ERR_INVALID_STATE;
    }

    *recv_len = (size_t)len;
    ESP_LOGD(TAG, "Received %d bytes", len);
    return ESP_OK;
}

tcp_client_state_t tcp_client_get_state(void)
{
    return s_state;
}

bool tcp_client_is_connected(void)
{
    return (s_state == TCP_CLIENT_STATE_CONNECTED && s_socket >= 0);
}
