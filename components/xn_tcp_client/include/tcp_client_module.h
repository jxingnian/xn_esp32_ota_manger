/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-12-25 00:00:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-12-25 00:00:00
 * @FilePath: \xn_ota_manger\components\xn_tcp_client\include\tcp_client_module.h
 * @Description: TCP 客户端模块对外接口
 *
 * - 提供 TCP 连接、断开、发送、接收功能；
 * - 支持设置服务端 IP 和端口；
 * - 支持异步接收回调通知。
 *
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved.
 */

#ifndef TCP_CLIENT_MODULE_H
#define TCP_CLIENT_MODULE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TCP 客户端连接状态
 */
typedef enum {
    TCP_CLIENT_STATE_DISCONNECTED = 0, ///< 已断开连接
    TCP_CLIENT_STATE_CONNECTING,       ///< 正在连接中
    TCP_CLIENT_STATE_CONNECTED,        ///< 已连接
    TCP_CLIENT_STATE_ERROR,            ///< 连接错误
} tcp_client_state_t;

/**
 * @brief TCP 客户端事件类型
 */
typedef enum {
    TCP_CLIENT_EVENT_CONNECTED = 0,    ///< 连接成功
    TCP_CLIENT_EVENT_DISCONNECTED,     ///< 连接断开
    TCP_CLIENT_EVENT_DATA_RECEIVED,    ///< 收到数据
    TCP_CLIENT_EVENT_ERROR,            ///< 发生错误
} tcp_client_event_t;

/**
 * @brief TCP 客户端事件数据
 */
typedef struct {
    tcp_client_event_t event;          ///< 事件类型
    uint8_t           *data;           ///< 接收到的数据指针（仅 DATA_RECEIVED 事件有效）
    size_t             data_len;       ///< 数据长度
    int                error_code;     ///< 错误码（仅 ERROR 事件有效）
} tcp_client_event_data_t;

/**
 * @brief TCP 客户端事件回调函数
 *
 * @param event_data 事件数据指针
 */
typedef void (*tcp_client_event_cb_t)(const tcp_client_event_data_t *event_data);

/**
 * @brief TCP 客户端配置
 */
typedef struct {
    char     server_ip[64];            ///< 服务端 IP 地址或域名
    uint16_t server_port;              ///< 服务端端口
    int      connect_timeout_ms;       ///< 连接超时时间（毫秒）
    int      recv_timeout_ms;          ///< 接收超时时间（毫秒）
    size_t   recv_buf_size;            ///< 接收缓冲区大小
    bool     auto_reconnect;           ///< 是否自动重连
    int      reconnect_interval_ms;    ///< 重连间隔（毫秒）
    tcp_client_event_cb_t event_cb;    ///< 事件回调函数
} tcp_client_config_t;

/**
 * @brief TCP 客户端默认配置
 */
#define TCP_CLIENT_DEFAULT_CONFIG()                    \
    (tcp_client_config_t){                             \
        .server_ip            = "",                    \
        .server_port          = 0,                     \
        .connect_timeout_ms   = 5000,                  \
        .recv_timeout_ms      = 3000,                  \
        .recv_buf_size        = 1024,                  \
        .auto_reconnect       = false,                 \
        .reconnect_interval_ms = 5000,                 \
        .event_cb             = NULL,                  \
    }

/**
 * @brief 初始化 TCP 客户端模块
 *
 * @param config 配置指针，若为 NULL 则使用默认配置
 *
 * @return
 *      - ESP_OK      : 初始化成功
 *      - 其它 esp_err_t : 错误码
 */
esp_err_t tcp_client_init(const tcp_client_config_t *config);

/**
 * @brief 反初始化 TCP 客户端模块
 *
 * @return
 *      - ESP_OK      : 反初始化成功
 *      - 其它 esp_err_t : 错误码
 */
esp_err_t tcp_client_deinit(void);

/**
 * @brief 设置服务端地址
 *
 * @param ip   服务端 IP 地址或域名
 * @param port 服务端端口
 *
 * @return
 *      - ESP_OK              : 设置成功
 *      - ESP_ERR_INVALID_ARG : 参数无效
 */
esp_err_t tcp_client_set_server(const char *ip, uint16_t port);

/**
 * @brief 连接到服务端
 *
 * @return
 *      - ESP_OK              : 连接成功
 *      - ESP_ERR_INVALID_STATE : 已连接或正在连接
 *      - 其它 esp_err_t      : 连接失败
 */
esp_err_t tcp_client_connect(void);

/**
 * @brief 断开与服务端的连接
 *
 * @return
 *      - ESP_OK              : 断开成功
 *      - ESP_ERR_INVALID_STATE : 未连接
 */
esp_err_t tcp_client_disconnect(void);

/**
 * @brief 发送数据到服务端
 *
 * @param data     数据指针
 * @param data_len 数据长度
 *
 * @return
 *      - ESP_OK              : 发送成功
 *      - ESP_ERR_INVALID_ARG : 参数无效
 *      - ESP_ERR_INVALID_STATE : 未连接
 *      - 其它 esp_err_t      : 发送失败
 */
esp_err_t tcp_client_send(const uint8_t *data, size_t data_len);

/**
 * @brief 同步接收数据（阻塞）
 *
 * @param[out] buf      接收缓冲区
 * @param[in]  buf_size 缓冲区大小
 * @param[out] recv_len 实际接收到的数据长度
 * @param[in]  timeout_ms 超时时间（毫秒），0 表示使用配置的默认超时
 *
 * @return
 *      - ESP_OK              : 接收成功
 *      - ESP_ERR_INVALID_ARG : 参数无效
 *      - ESP_ERR_INVALID_STATE : 未连接
 *      - ESP_ERR_TIMEOUT     : 接收超时
 *      - 其它 esp_err_t      : 接收失败
 */
esp_err_t tcp_client_recv(uint8_t *buf, size_t buf_size, size_t *recv_len, int timeout_ms);

/**
 * @brief 获取当前连接状态
 *
 * @return 当前连接状态
 */
tcp_client_state_t tcp_client_get_state(void);

/**
 * @brief 检查是否已连接
 *
 * @return
 *      - true  : 已连接
 *      - false : 未连接
 */
bool tcp_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_CLIENT_MODULE_H */
