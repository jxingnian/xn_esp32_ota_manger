/*
 * @Author: 星年 jixingnian@gmail.com
 * @Date: 2025-11-22 13:43:50
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-12-25 00:00:00
 * @FilePath: \xn_ota_manger\main\main.c
 * @Description: esp32 OTA管理组件 By.星年
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "xn_wifi_manage.h"
#include "http_ota_manager.h"
#include "tcp_client_module.h"

static const char *TAG = "app_main";

/* 仅在首次拿到 IP 后初始化一次 OTA 管理 */
static bool s_ota_inited = false;

/* TCP 回环任务：收到数据后原样发回 */
static void tcp_loopback_task(void *arg)
{
	(void)arg;

	/* 初始化 TCP 客户端 */
	tcp_client_config_t tcp_cfg = TCP_CLIENT_DEFAULT_CONFIG();
	tcp_cfg.auto_reconnect = true;
	tcp_cfg.reconnect_interval_ms = 5000;

	esp_err_t ret = tcp_client_init(&tcp_cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "tcp_client_init failed: %s", esp_err_to_name(ret));
		vTaskDelete(NULL);
		return;
	}

	/* 设置服务端地址 */
	tcp_client_set_server("192.168.2.188", 2345);

	/* 连接服务端 */
	ret = tcp_client_connect();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "tcp_client_connect failed: %s", esp_err_to_name(ret));
	}

	uint8_t recv_buf[256];
	size_t recv_len = 0;

	for (;;) {
		if (tcp_client_is_connected()) {
			/* 等待接收数据 */
			ret = tcp_client_recv(recv_buf, sizeof(recv_buf), &recv_len, 1000);
			if (ret == ESP_OK && recv_len > 0) {
				ESP_LOGI(TAG, "Recv %d bytes, echo back", recv_len);
				/* 原样发回 */
				tcp_client_send(recv_buf, recv_len);
			}
		} else {
			/* 未连接时尝试重连 */
			tcp_client_connect();
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}
}

/*
 * @brief OTA 初始化任务
 *
 * 在独立任务栈中调用 ota_manage_init，避免在 sys_evt 任务中发生栈溢出。
 * OTA 检查完成后启动 TCP 发送任务。
 */
static void ota_init_task(void *arg)
{
	(void)arg;

	http_ota_manager_config_t cfg = HTTP_OTA_MANAGER_DEFAULT_CONFIG();
	snprintf(cfg.version_url,
		 sizeof(cfg.version_url),
		 "http://win.xingnian.vip:16623/firmware/version.json");

	esp_err_t ret = http_ota_manager_init(&cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "http_ota_manager_init failed: %s", esp_err_to_name(ret));
		/* OTA 初始化失败，仍然启动 TCP 任务 */
		goto start_tcp;
	}

	ret = http_ota_manager_check_now();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "http_ota_manager_check_now failed: %s", esp_err_to_name(ret));
	}

	/* OTA 检查完成（无论成功失败），启动 TCP 任务 */
start_tcp:
	xTaskCreate(tcp_loopback_task, "tcp_loopback", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);

	vTaskDelete(NULL);
}

/*
 * @brief WiFi 管理状态回调
 *
 * 当 WiFi 管理状态变为 CONNECTED（已拿到 IP）时，创建任务初始化 OTA 管理模块。
 */
static void wifi_manage_event_cb(wifi_manage_state_t state)
{
	if (state != WIFI_MANAGE_STATE_CONNECTED || s_ota_inited) {
		return;
	}

	BaseType_t ret = xTaskCreate(ota_init_task,
				    "ota_init",
				    1024*8,
				    NULL,
				    tskIDLE_PRIORITY + 2,
				    NULL);
	if (ret != pdPASS) {
		ESP_LOGE(TAG, "create ota_init task failed");
		return;
	}

	s_ota_inited = true;
}

/*
 * @brief 应用入口：初始化 WiFi 管理，WiFi 连上后再初始化 OTA 管理。
 */
void app_main(void)
{
	printf("esp32 OTA管理组件 By.星年\n");

	wifi_manage_config_t wifi_cfg = WIFI_MANAGE_DEFAULT_CONFIG();
	wifi_cfg.wifi_event_cb        = wifi_manage_event_cb;

	esp_err_t ret = wifi_manage_init(&wifi_cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "wifi_manage_init failed: %s", esp_err_to_name(ret));
	}
}
