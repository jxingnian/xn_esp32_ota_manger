/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-23 19:19:08
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-23 20:48:29
 * @FilePath: \xn_ota_manger\components\xn_ota_manger\src\http_ota_manager.c
 * @Description: HTTP OTA 管理模块实现
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#include "http_ota_manager.h"

#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "cJSON.h"

/* 本模块日志 TAG，用于 ESP_LOGx 宏输出 */
static const char *TAG = "http_ota_manager";

/* -------------------- 静态上下文与工具函数 -------------------- */

/* 上层传入的 OTA 管理配置副本，仅在 http_ota_manager_init 中赋值一次 */
static http_ota_manager_config_t s_cfg;
/* 表示模块是否已完成初始化（http_ota_manager_init 是否被成功调用） */
static bool                      s_inited          = false;
/* 当前 OTA 管理状态，初始为 IDLE，流程中会切换为 RUNNING/SUCCESS/FAILED */
static http_ota_state_t          s_state           = HTTP_OTA_STATE_IDLE;
/* 最近一次从远端 version.json 获取到的版本信息快照 */
static http_ota_remote_info_t    s_last_remote_info;
/* 标记 s_last_remote_info 中的数据是否有效 */
static bool                      s_has_remote_info = false;

/**
 * @brief 内部状态切换辅助函数
 *
 * 在修改 s_state 的同时，若配置中注册了 state_cb 回调，
 * 则主动通知上层“状态变化”，便于应用层做 UI/LOG 处理。
 *
 * @param state 新的 OTA 状态
 */
static void ota_set_state(http_ota_state_t state)
{
	s_state = state;
	if (s_cfg.state_cb) {
		s_cfg.state_cb(state);
	}
}

/**
 * @brief 从配置中的 version_url 获取 version.json 文本
 *
 * @param[out] buf       用于存放 version.json 字符串的缓冲区
 * @param[in]  buf_size  缓冲区大小（字节数）
 *
 * @return
 *  - ESP_OK              : 获取成功，buf 中为以 '\0' 结尾的 JSON 字符串
 *  - ESP_ERR_INVALID_ARG : 参数非法或 version_url 为空
 *  - 其它 esp_err_t      : 底层 HTTP 访问失败，具体错误见日志
 */
static esp_err_t fetch_version_json(char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	if (s_cfg.version_url[0] == '\0') {
		ESP_LOGE(TAG, "version_url is empty");
		return ESP_ERR_INVALID_ARG;
	}

	/* HTTP 客户端配置：使用 GET 方法，超时与 URL 由上层配置决定 */
	esp_http_client_config_t http_cfg = {
		.url                         = s_cfg.version_url,
		.timeout_ms                  = s_cfg.http_timeout_ms,
		.method                      = HTTP_METHOD_GET,
		/* 若使用 HTTPS 且证书校验严格，可根据实际需求调整此选项 */
		.skip_cert_common_name_check = true,
	};

	esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
	if (!client) {
		ESP_LOGE(TAG, "esp_http_client_init failed");
		return ESP_FAIL;
	}

	/* 打开连接并发送请求（GET 请求 body 长度为 0） */
	esp_err_t err = esp_http_client_open(client, 0);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "open version_url failed: %s", esp_err_to_name(err));
		esp_http_client_cleanup(client);
		return err;
	}

	/* 可选：获取 Content-Length，方便上层调试观察版本文件大小 */
	int content_length = esp_http_client_fetch_headers(client);
	ESP_LOGI(TAG, "version.json content_length=%d", content_length);

	/* 校验 HTTP 响应状态码，要求为 200 OK */
	int status_code = esp_http_client_get_status_code(client);
	if (status_code != 200) {
		ESP_LOGE(TAG, "unexpected HTTP status: %d", status_code);
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return ESP_FAIL;
	}

	/* 循环读取响应体到缓冲区，注意保留 '\0' 结尾的空间 */
	int total_read = 0;
	while (total_read < (int)buf_size - 1) {
		int read_len = esp_http_client_read(client,
						buf + total_read,
						buf_size - 1 - total_read);
		if (read_len < 0) {
			ESP_LOGE(TAG, "read version.json failed");
			esp_http_client_close(client);
			esp_http_client_cleanup(client);
			return ESP_FAIL;
		}
		if (read_len == 0) {
			/* 服务器主动关闭连接，视为读取结束 */
			break;
		}
		total_read += read_len;
	}

	/* 补 '\0'，确保 buf 是一个 C 风格字符串 */
	buf[total_read] = '\0';
	ESP_LOGD(TAG, "version.json body: %s", buf);

	esp_http_client_close(client);
	esp_http_client_cleanup(client);

	if (total_read <= 0) {
		ESP_LOGE(TAG, "empty version.json body");
		return ESP_FAIL;
	}

	return ESP_OK;
}

/**
 * @brief 解析 version.json，并填充 http_ota_remote_info_t 结构体
 *
 * @param[in]  json_str JSON 文本（以 '\0' 结尾）
 * @param[out] out      解析结果输出结构体，不可为 NULL
 *
 * @return
 *  - ESP_OK                  : 解析成功，out 中数据有效
 *  - ESP_ERR_INVALID_ARG     : 参数非法
 *  - ESP_ERR_INVALID_RESPONSE: JSON 缺少必需字段或类型不匹配
 *  - ESP_FAIL                : JSON 解析失败
 */
static esp_err_t parse_version_json(const char *json_str, http_ota_remote_info_t *out)
{
	if (!json_str || !out) {
		return ESP_ERR_INVALID_ARG;
	}

	cJSON *root = cJSON_Parse(json_str);
	if (!root) {
		ESP_LOGE(TAG, "cJSON_Parse failed");
		return ESP_FAIL;
	}

	/* 逐项从 JSON 根对象中获取相关字段 */
	cJSON *j_version = cJSON_GetObjectItemCaseSensitive(root, "version");
	cJSON *j_url     = cJSON_GetObjectItemCaseSensitive(root, "url");
	cJSON *j_desc    = cJSON_GetObjectItemCaseSensitive(root, "description");
	cJSON *j_force   = cJSON_GetObjectItemCaseSensitive(root, "force");

	if (!cJSON_IsString(j_version) || !cJSON_IsString(j_url)) {
		ESP_LOGE(TAG, "version/url missing or not string");
		cJSON_Delete(root);
		return ESP_ERR_INVALID_RESPONSE;
	}

	/* 先清零，再逐字段拷贝，保证字符串以 '\0' 结尾 */
	memset(out, 0, sizeof(*out));
	strncpy(out->version, j_version->valuestring, HTTP_OTA_VERSION_MAX_LEN - 1);
	strncpy(out->url, j_url->valuestring, HTTP_OTA_URL_MAX_LEN - 1);
	if (cJSON_IsString(j_desc) && j_desc->valuestring) {
		strncpy(out->description, j_desc->valuestring, HTTP_OTA_DESC_MAX_LEN - 1);
	} else {
		out->description[0] = '\0';
	}
	/* force 字段不存在时默认 false，存在时根据布尔值设置 */
	out->force = cJSON_IsBool(j_force) ? cJSON_IsTrue(j_force) : false;

	cJSON_Delete(root);
	return ESP_OK;
}

/**
 * @brief 使用给定的固件 URL 执行 OTA 升级
 *
 * @param[in] url 固件镜像下载地址
 *
 * @return
 *  - ESP_OK : OTA 更新完成（包括完整下载及写入）
 *  - 其它    : esp_https_ota 失败，具体错误码见日志
 */
static esp_err_t do_ota_with_url(const char *url)
{
	if (!url || url[0] == '\0') {
		ESP_LOGE(TAG, "OTA url is empty");
		return ESP_ERR_INVALID_ARG;
	}

	/* OTA 使用的 HTTP 客户端配置，与获取 version.json 类似 */
	esp_http_client_config_t http_cfg = {
		.url                         = url,
		.timeout_ms                  = s_cfg.http_timeout_ms,
		/* 如需严格校验证书，可在此关闭 skip，并配置证书等信息 */
		.skip_cert_common_name_check = true,
	};

	esp_https_ota_config_t ota_cfg = {
		.http_config = &http_cfg,
	};

	ESP_LOGI(TAG, "start OTA from: %s", url);
	esp_err_t ret = esp_https_ota(&ota_cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_https_ota failed: %s", esp_err_to_name(ret));
		return ret;
	}

	ESP_LOGI(TAG, "OTA update succeeded");
	return ESP_OK;
}

/* -------------------- 对外 API 实现 -------------------- */

/**
 * @brief 初始化 HTTP OTA 管理模块
 *
 * @param[in] config 上层提供的配置指针，不可为 NULL
 *
 * @return
 *  - ESP_OK              : 初始化成功（或已完成初始化）
 *  - ESP_ERR_INVALID_ARG : config 为 NULL 或 version_url 为空
 */
esp_err_t http_ota_manager_init(const http_ota_manager_config_t *config)
{
	if (!config) {
		return ESP_ERR_INVALID_ARG;
	}

	if (config->version_url[0] == '\0') {
		ESP_LOGE(TAG, "version_url is empty in config");
		return ESP_ERR_INVALID_ARG;
	}

	/* 已初始化则直接返回，避免重复操作 */
	if (s_inited) {
		return ESP_OK;
	}

	/* 保存一份配置副本，避免上层变量生命周期结束后影响本模块 */
	memset(&s_cfg, 0, sizeof(s_cfg));
	s_cfg = *config;
	if (s_cfg.http_timeout_ms <= 0) {
		s_cfg.http_timeout_ms = 15000;
	}

	s_state           = HTTP_OTA_STATE_IDLE;
	s_has_remote_info = false;
	s_inited          = true;

	ESP_LOGI(TAG, "http_ota_manager initialized, version_url=%s", s_cfg.version_url);
	return ESP_OK;
}
/**
 * @brief 立即触发一次 OTA 检查 / 升级流程
 *
 * @return
 *  - ESP_OK                 : 流程执行结束（无论是否进行了实际升级，只要未出错）
 *  - ESP_ERR_INVALID_STATE  : 模块未初始化，或当前已有 OTA 流程正在进行
 *  - 其它 esp_err_t         : HTTP 访问、JSON 解析或 OTA 过程中的错误码
 */
esp_err_t http_ota_manager_check_now(void)
{
	/* 模块尚未初始化，直接拒绝执行 */
	if (!s_inited) {
		return ESP_ERR_INVALID_STATE;
	}

	/* 若当前状态为运行中，说明已有 OTA 流程在进行，避免并发操作 */
	if (s_state == HTTP_OTA_STATE_RUNNING) {
		ESP_LOGW(TAG, "OTA already running");
		return ESP_ERR_INVALID_STATE;
	}

	/* 标记开始进行 OTA 检查 / 升级流程 */
	ota_set_state(HTTP_OTA_STATE_RUNNING);

	esp_err_t err;
	/* 用于存放从服务端获取到的 version.json 文本 */
	char      json_buf[512];

	/* 第一步：从远端拉取 version.json */
	err = fetch_version_json(json_buf, sizeof(json_buf));
	if (err != ESP_OK) {
		/* HTTP 拉取失败则直接结束流程 */
		ota_set_state(HTTP_OTA_STATE_FAILED);
		return err;
	}

	/* 第二步：解析 JSON 为结构体形式，便于后续处理 */
	http_ota_remote_info_t remote = {0};
	err = parse_version_json(json_buf, &remote);
	if (err != ESP_OK) {
		ota_set_state(HTTP_OTA_STATE_FAILED);
		return err;
	}

	/* 记录最新一次获取到的远端版本信息快照 */
	s_last_remote_info = remote;
	s_has_remote_info  = true;

	/* 本地版本号从 sdkconfig 中读取，若未配置则退化为空串 */
	const char *local_ver = CONFIG_APP_PROJECT_VER;
	if (!local_ver) {
		local_ver = "";
	}

	ESP_LOGI(TAG,
		 "local version=%s, remote version=%s, force=%d",
		 local_ver,
		 remote.version,
		 remote.force);

	/* 若本地版本号与远端版本号一致，则认为无需升级 */
	if (strcmp(remote.version, local_ver) == 0) {
		ESP_LOGI(TAG, "already on latest version, no OTA needed");
		ota_set_state(HTTP_OTA_STATE_SUCCESS);
		return ESP_OK;
	}

	/* 版本不同，开始执行 OTA 升级流程 */
	err = do_ota_with_url(remote.url);
	if (err != ESP_OK) {
		ota_set_state(HTTP_OTA_STATE_FAILED);
		return err;
	}

	/* OTA 升级成功 */
	ota_set_state(HTTP_OTA_STATE_SUCCESS);

	/* 根据配置决定是否在升级成功后自动重启设备 */
	if (s_cfg.auto_reboot) {
		ESP_LOGI(TAG, "auto_reboot enabled, restarting...");
		esp_restart();
	}

	return ESP_OK;
}

/**
 * @brief 获取当前 HTTP OTA 管理器状态
 *
 * @return 当前 OTA 状态（参见 @ref http_ota_state_t 枚举定义）
 */
http_ota_state_t http_ota_manager_get_state(void)
{
	return s_state;
}

/**
 * @brief 获取最近一次成功从服务端获取到的远端版本信息快照
 *
 * @param[out] info  调用方提供的结构体指针，用于接收远端版本信息，不可为 NULL
 *
 * @return
 *  - ESP_OK                : 拷贝成功，info 中的数据有效
 *  - ESP_ERR_INVALID_ARG   : info 为 NULL
 *  - ESP_ERR_INVALID_STATE : 尚未成功获取并解析过 version.json
 */
esp_err_t http_ota_manager_get_last_remote_info(http_ota_remote_info_t *info)
{
	if (!info) {
		return ESP_ERR_INVALID_ARG;
	}

	/* 需保证模块已初始化且至少成功获取过一次远端版本信息 */
	if (!s_inited || !s_has_remote_info) {
		return ESP_ERR_INVALID_STATE;
	}

	*info = s_last_remote_info;
	return ESP_OK;
}
