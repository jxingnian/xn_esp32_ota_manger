/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-10-29
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-23 17:20:00
 * @FilePath: \xn_ota_manger\components\xn_ota_manger\src\http_client_module.c
 * @Description: HTTP 客户端模块实现（基于 esp_http_client）
 *
 * 统一网络架构：WiFi 与 4G（通过 USB RNDIS）都通过本模块发送 HTTP 请求，
 * 上层无需关心具体链路差异，仅关注 URL / 超时等参数。
 */

#include "http_client_module.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_client_module";

/**
 * @brief HTTP 客户端内部结构
 */
typedef struct {
    esp_http_client_handle_t client;
    int content_length;
    int status_code;
} http_client_t;

/**
 * @brief 创建 HTTP 客户端
 */
http_client_handle_t http_client_create(const http_client_config_t *config)
{
    if (!config || !config->url) {
        ESP_LOGE(TAG, "配置参数无效");
        return NULL;
    }

    http_client_t *client = (http_client_t *)calloc(1, sizeof(http_client_t));
    if (!client) {
        ESP_LOGE(TAG, "分配内存失败");
        return NULL;
    }

    // 配置esp_http_client
    esp_http_client_config_t http_config = {
        .url = config->url,
        .timeout_ms = config->timeout_ms,
        .keep_alive_enable = true,
        .buffer_size = 1024,
        .skip_cert_common_name_check = true,
    };

    client->client = esp_http_client_init(&http_config);
    if (!client->client) {
        ESP_LOGE(TAG, "初始化HTTP客户端失败");
        free(client);
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP客户端创建成功");
    return (http_client_handle_t)client;
}

/**
 * @brief 销毁 HTTP 客户端
 */
void http_client_destroy(http_client_handle_t handle)
{
    if (!handle) {
        return;
    }

    http_client_t *client = (http_client_t *)handle;
    
    if (client->client) {
        esp_http_client_cleanup(client->client);
    }
    
    free(client);
    ESP_LOGI(TAG, "HTTP客户端已销毁");
}

/**
 * @brief 设置 HTTP 请求头
 */
esp_err_t http_client_set_header(http_client_handle_t handle, const char *key, const char *value)
{
    if (!handle || !key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    http_client_t *client = (http_client_t *)handle;
    
    esp_err_t err = esp_http_client_set_header(client->client, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置HTTP头失败: %s = %s", key, value);
        return err;
    }
    
    ESP_LOGD(TAG, "设置HTTP头: %s = %s", key, value);
    return ESP_OK;
}

/**
 * @brief 打开 HTTP 连接
 */
esp_err_t http_client_open(http_client_handle_t handle, const char *method)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    http_client_t *client = (http_client_t *)handle;

    // 设置HTTP方法
    esp_err_t err = esp_http_client_set_method(client->client, 
                                                strcmp(method, "GET") == 0 ? HTTP_METHOD_GET : HTTP_METHOD_POST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置HTTP方法失败: %s", esp_err_to_name(err));
        return err;
    }

    // 打开连接
    err = esp_http_client_open(client->client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开连接失败: %s", esp_err_to_name(err));
        return err;
    }

    // 获取响应头
    client->content_length = esp_http_client_fetch_headers(client->client);
    client->status_code = esp_http_client_get_status_code(client->client);

    ESP_LOGI(TAG, "HTTP连接已打开 - 状态码: %d, 内容长度: %d", 
             client->status_code, client->content_length);

    return ESP_OK;
}

/**
 * @brief 获取 HTTP 状态码
 */
int http_client_get_status_code(http_client_handle_t handle)
{
    if (!handle) {
        return 0;
    }

    http_client_t *client = (http_client_t *)handle;
    return client->status_code;
}

/**
 * @brief 获取内容长度
 */
int http_client_get_content_length(http_client_handle_t handle)
{
    if (!handle) {
        return -1;
    }

    http_client_t *client = (http_client_t *)handle;
    return client->content_length;
}

/**
 * @brief 读取响应数据
 */
int http_client_read(http_client_handle_t handle, void *buffer, size_t len)
{
    if (!handle || !buffer) {
        return -1;
    }

    http_client_t *client = (http_client_t *)handle;
    return esp_http_client_read(client->client, (char *)buffer, len);
}

/**
 * @brief 关闭 HTTP 连接
 */
esp_err_t http_client_close(http_client_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    http_client_t *client = (http_client_t *)handle;
    
    esp_err_t err = esp_http_client_close(client->client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "关闭连接失败: %s", esp_err_to_name(err));
    }

    return err;
}

