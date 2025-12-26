/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-23 16:48:45
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-23 20:56:02
 * @FilePath: \xn_ota_manger\components\xn_ota_manger\include\http_ota_manager.h
 * @Description: 
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#ifndef HTTP_OTA_MANAGER_H
#define HTTP_OTA_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP OTA 管理模块对外接口
 *
 * 设计目标：
 *  - 设备侧只需提供一个指向云端 `version.json` 的 URL（version_url）；
 *  - 本地固件版本号从 `sdkconfig` 的 `CONFIG_APP_PROJECT_VER` 读取（由 `sdkconfig.defaults` 配置）；
 *  - 云端 `version.json` 使用固定格式：
 *
 *    {"version":"1.0.4",
 *     "url":"http://your-domain/firmware/xxx.bin",
 *     "description":"第一次优化",
 *     "force":true}
 *
 *  - 模块内部完成：
 *      1) 访问 version_url，解析上述 JSON；
 *      2) 使用 CONFIG_APP_PROJECT_VER 作为本地版本，与 JSON.version 对比；
 *      3) 判定是否需要升级，并按 JSON.url 下载固件，走 ESP-IDF OTA 流程；
 *      4) 升级成功后按配置决定是否自动重启。
 */

/**
 * @brief 内部使用的字符串长度上限
 */
#define HTTP_OTA_VERSION_MAX_LEN   32    ///< 远端版本号最大长度（含 '\0'）
#define HTTP_OTA_URL_MAX_LEN       256   ///< URL 最大长度（含 '\0'）
#define HTTP_OTA_DESC_MAX_LEN      128   ///< 描述最大长度（含 '\0'，超出将被截断）

/**
 * @brief HTTP OTA 管理器状态（精简版）
 *
 * 仅区分“空闲 / 正在执行 / 成功 / 失败”四种结果，具体阶段通过日志查看。
 */
typedef enum {
    HTTP_OTA_STATE_IDLE = 0,   ///< 空闲，当前没有 OTA 任务
    HTTP_OTA_STATE_RUNNING,    ///< 正在检查版本或执行 OTA 升级
    HTTP_OTA_STATE_SUCCESS,    ///< 本次检查/升级流程成功结束（已是最新或升级成功）
    HTTP_OTA_STATE_FAILED,     ///< 本次检查/升级流程失败
} http_ota_state_t;

/**
 * @brief OTA 状态回调
 *
 * 由 OTA 管理模块在状态变化时调用，用于通知应用层：
 *  - 可用于更新 UI / 打日志 / 上报云端等；
 *  - 回调发生在 OTA 管理任务上下文中，建议避免在回调内执行耗时阻塞操作。
 *
 * @param state 当前 OTA 状态（见 @ref http_ota_state_t）
 */
typedef void (*http_ota_state_cb_t)(http_ota_state_t state);

/**
 * @brief 远端版本信息快照
 *
 * 反映最近一次成功从 version_url 获取到的 JSON 内容：
 *  - version      ← JSON 中的 "version"；
 *  - url          ← JSON 中的 "url"；
 *  - description  ← JSON 中的 "description"（如过长会被截断）；
 *  - force        ← JSON 中的 "force"（缺失时默认为 false）。
 */
typedef struct {
    char version[HTTP_OTA_VERSION_MAX_LEN];   ///< 远端固件版本号
    char url[HTTP_OTA_URL_MAX_LEN];          ///< 固件下载 URL
    char description[HTTP_OTA_DESC_MAX_LEN]; ///< 更新说明
    bool force;                              ///< 是否为“强制更新”
} http_ota_remote_info_t;

/**
 * @brief HTTP OTA 管理模块配置
 *
 * 典型使用方式：
 * @code
 * http_ota_manager_config_t cfg = HTTP_OTA_MANAGER_DEFAULT_CONFIG();
 * snprintf(cfg.version_url,
 *          sizeof(cfg.version_url),
 *          "http://your-domain.com/firmware/version.json");
 * cfg.state_cb = my_ota_state_cb;   // 可选
 * http_ota_manager_init(&cfg);
 * http_ota_manager_check_now();     // 手动触发一次检查
 * @endcode
 *
 * 本地版本号默认从 CONFIG_APP_PROJECT_VER 读取，请在 sdkconfig.defaults 中配置：
 *  - CONFIG_APP_PROJECT_VER_FROM_CONFIG=y
 *  - CONFIG_APP_PROJECT_VER="1.0.3"      // 示例
 */
typedef struct {
    char  version_url[HTTP_OTA_URL_MAX_LEN]; ///< 版本配置 JSON 地址，如 "http://host/firmware/version.json"
    int   check_interval_sec;                ///< 预留字段，当前实现未启用自动周期检查，建议填 0
    int   http_timeout_ms;                   ///< HTTP 请求超时时间（毫秒）
    bool  auto_reboot;                       ///< OTA 升级成功后是否自动 esp_restart()
    http_ota_state_cb_t state_cb;            ///< 状态变化回调，可为 NULL 表示不关心
} http_ota_manager_config_t;

/**
 * @brief HTTP OTA 管理模块默认配置
 *
 * 默认行为：
 *  - version_url 为空字符串，需要上层在初始化前填充；
 *  - 不自动周期检查（check_interval_sec = 0，需要调用 http_ota_manager_check_now 手动触发）；
 *  - HTTP 超时 15000 ms；
 *  - OTA 成功后自动重启（auto_reboot = true）；
 *  - 不注册状态回调（state_cb = NULL）。
 */
#define HTTP_OTA_MANAGER_DEFAULT_CONFIG()              \
    (http_ota_manager_config_t){                        \
        .version_url        = "",                      \
        .check_interval_sec = 0,                        \
        .http_timeout_ms    = 15000,                    \
        .auto_reboot        = true,                     \
        .state_cb           = NULL,                     \
    }

/**
 * @brief 初始化 HTTP OTA 管理模块
 *
 * 功能概览：
 *  - 保存配置参数，准备内部 HTTP 客户端等资源；
 *  - 可多次调用，仅首次生效，后续调用直接返回 ESP_OK。
 *
 * 使用建议：
 *  - 应在网络已就绪后调用（例如在 wifi_manage 回调 WIFI_MANAGE_STATE_CONNECTED 时）；
 *  - version_url 必须指向合法的 `version.json` 地址，其内容格式参考本头文件前部说明；
 *  - 本地版本号由 CONFIG_APP_PROJECT_VER 提供，确保在 sdkconfig.defaults 中正确配置。
 *
 * @param config 配置指针，不可为 NULL
 *
 * @return
 *  - ESP_OK              : 初始化成功（或已初始化）
 *  - ESP_ERR_INVALID_ARG : config 非法或关键字段为空
 *  - 其它 esp_err_t      : 内部资源不足等错误，具体见日志
 */
esp_err_t http_ota_manager_init(const http_ota_manager_config_t *config);

/**
 * @brief 立即触发一次 OTA 检查 / 升级流程
 *
 * 流程概览：
 *  1) HTTP GET version_url，解析 JSON；
 *  2) 使用 CONFIG_APP_PROJECT_VER 与 JSON.version 比较；
 *  3) 若无需升级：仅记录远端信息并返回成功；
 *  4) 若需要升级：按 JSON.url 下载固件并执行 OTA 流程。
 *
 * - 调用期间内部会将状态设置为 HTTP_OTA_STATE_RUNNING，结束后根据结果切换为
 *   HTTP_OTA_STATE_SUCCESS 或 HTTP_OTA_STATE_FAILED。
 * - 若当前已有 OTA 在执行，则立即返回 ESP_ERR_INVALID_STATE。
 *
 * @return
 *  - ESP_OK                 : 本次检查/升级流程执行完成（不一定发生升级）
 *  - ESP_ERR_INVALID_STATE  : 模块未初始化或当前正处于 OTA 流程中
 */
esp_err_t http_ota_manager_check_now(void);

/**
 * @brief 获取当前 OTA 管理器状态
 *
 * 该接口为轻量查询接口，可在任意任务中调用，用于在不依赖回调的场景下
 * 快速了解当前 OTA 状态。
 *
 * @return 当前 OTA 状态（见 @ref http_ota_state_t）
 */
http_ota_state_t http_ota_manager_get_state(void);

/**
 * @brief 获取最近一次远端版本信息快照
 *
 * 一般在 HTTP 访问 version_url 成功后更新。
 *
 * @param[out] info  调用方提供的结构体指针，不可为 NULL
 *
 * @return
 *  - ESP_OK              : 拷贝成功，info 中数据有效
 *  - ESP_ERR_INVALID_ARG : info 为 NULL
 *  - ESP_ERR_INVALID_STATE : 尚未成功从服务端获取过 version.json
 */
esp_err_t http_ota_manager_get_last_remote_info(http_ota_remote_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_OTA_MANAGER_H */
