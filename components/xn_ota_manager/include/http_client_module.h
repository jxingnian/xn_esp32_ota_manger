/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-10-29
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-23 17:20:00
 * @FilePath: \xn_ota_manger\components\xn_ota_manger\include\http_client_module.h
 * @Description: HTTP 客户端模块对外接口（基于 esp_http_client 的轻量封装）
 *
 * 仅负责“收发 HTTP 请求本身”，不关心具体业务：
 *  - 统一封装 WiFi / 4G (RNDIS) 等网络通道的 HTTP 行为；
 *  - 由上层模块（如 http_ota_module）负责解析业务数据。
 */

#ifndef HTTP_CLIENT_MODULE_H
#define HTTP_CLIENT_MODULE_H

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP 客户端句柄（不透明指针）
 */
typedef void* http_client_handle_t;

/**
 * @brief HTTP 客户端配置
 */
typedef struct {
    const char *url;              ///< URL地址
    int timeout_ms;               ///< 超时时间（毫秒）
} http_client_config_t;

/**
 * @brief 创建 HTTP 客户端
 *
 * @param config 配置参数
 * @return http_client_handle_t 客户端句柄，失败返回 NULL
 */
http_client_handle_t http_client_create(const http_client_config_t *config);

/**
 * @brief 销毁 HTTP 客户端
 *
 * @param handle 客户端句柄
 */
void http_client_destroy(http_client_handle_t handle);

/**
 * @brief 设置 HTTP 请求头
 *
 * @param handle 客户端句柄
 * @param key    头字段名
 * @param value  头字段值
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t http_client_set_header(http_client_handle_t handle, const char *key, const char *value);

/**
 * @brief 打开 HTTP 连接
 *
 * @param handle 客户端句柄
 * @param method HTTP 方法（"GET", "POST" 等）
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t http_client_open(http_client_handle_t handle, const char *method);

/**
 * @brief 获取 HTTP 状态码
 *
 * @param handle 客户端句柄
 * @return int HTTP 状态码（如 200, 404 等）
 */
int http_client_get_status_code(http_client_handle_t handle);

/**
 * @brief 获取响应内容长度
 *
 * @param handle 客户端句柄
 * @return int 内容长度（字节），-1 表示未知
 */
int http_client_get_content_length(http_client_handle_t handle);

/**
 * @brief 读取响应数据
 *
 * @param handle 客户端句柄
 * @param buffer 接收缓冲区
 * @param len    缓冲区大小
 * @return int 实际读取的字节数，0 表示 EOF，<0 表示错误
 */
int http_client_read(http_client_handle_t handle, void *buffer, size_t len);

/**
 * @brief 关闭 HTTP 连接
 *
 * @param handle 客户端句柄
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t http_client_close(http_client_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // HTTP_CLIENT_MODULE_H

