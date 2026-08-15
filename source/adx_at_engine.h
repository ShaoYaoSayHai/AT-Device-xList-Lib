/**
 * @file adx_at_engine.h
 * @author Zorian(1551769443@qq.com)
 * @brief  ADX (AT Device X) AT指令引擎核心
 *
 * 引擎核心特性：
 *   1. 监控Map：预设「AT指令 + 状态过滤 + 触发间隔 + portName + 回调」
 *              全扫描取「最久未执行且到期」的项触发，循环往复
 *   2. 动态队列：开发者自由插入临时指令(HTTP/MQTT/业务)
 *              队列优先，每处理1条队列指令穿插1次map检查(保底防饿死)
 *   3. AT状态机 IDLE->SEND->WAITING->RESP_OK/TIMEOUT
 *   4. URC全局回调表
 *
 * 依赖关系：
 *   本文件仅依赖 adx_config.h 和 adx_port.h，不直接依赖任何RTOS或硬件。
 * 所有类型(回调、URC条目等)在内部重定义，彻底解耦业务项目。
 *
 * @version 0.1
 * @date 2026-08-16
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef _ADX_AT_ENGINE_H_
#define _ADX_AT_ENGINE_H_

#include "adx_config.h"
#include "adx_port.h"
#include <stdint.h>

/* ======================================================================== */
/*                          回调函数类型                                      */
/* ======================================================================== */

/**
 * @brief AT指令响应回调
 * @param buffer 接收窗口聚合后的数据
 * @param len    数据长度
 * @return ADX_OK=成功解析, ADX_FAIL=未匹配继续等待
 */
typedef int (*adx_at_recv_cb_t)(uint8_t *buffer, uint16_t len);

/**
 * @brief AT指令超时回调
 * @param cmd 超时的指令字符串(便于日志)
 * @return ADX_OK=已处理, ADX_FAIL=未处理
 */
typedef int (*adx_at_timeout_cb_t)(char *cmd);

/**
 * @brief URC接收回调
 * @param buffer URC数据
 * @param len    数据长度
 * @return ADX_OK=已处理, ADX_FAIL=未处理
 */
typedef int (*adx_urc_cb_t)(const uint8_t *buffer, uint16_t len);

/* ======================================================================== */
/*                          AT引擎状态机                                     */
/* ======================================================================== */

typedef enum
{
    ADX_AT_STATE_IDLE = 0U,  /* 空闲，等待下一条指令(来自map或queue) */
    ADX_AT_STATE_SEND,       /* 正在通过UART发送AT指令 */
    ADX_AT_STATE_WAITING,    /* 已发送，等待模组响应，期间回调解析 */
    ADX_AT_STATE_RESP_OK,    /* 回调返回成功，响应已处理 */
    ADX_AT_STATE_TIMEOUT,    /* 超时未收到有效响应 */
} adx_at_state_t;

/* ======================================================================== */
/*                          监控状态枚举(业务层)                             */
/* ======================================================================== */
/*
 * 这是业务层状态机，用于Map项的状态过滤。
 * 引擎本身不关心具体含义，由使用者在回调中通过 adx_at_monitor_state_set() 推进。
 * 以下为默认的WIFI模组状态，使用者也可忽略直接用0/1/2...自定义语义。
 */
typedef enum
{
    ADX_MONITOR_STATE_UNKNOWN = 0U,
    ADX_MONITOR_STATE_NOT_CONNECTED,
    ADX_MONITOR_STATE_IS_BUSY,
    ADX_MONITOR_STATE_IP_LOST,
    ADX_MONITOR_STATE_CONNECTED,
    ADX_MONITOR_STATE_FAILED,
    ADX_MONITOR_STATE_MAX,
} adx_monitor_state_t;

/* ======================================================================== */
/*                          监控Map条目                                      */
/* ======================================================================== */

/**
 * @brief 监控Map条目
 *
 *  - cmd          : AT指令字符串(含\r\n)，如 "AT+STAINFO?\r\n"
 *  - port_name    : 端口名，区分「同一AT指令在不同状态机上下文下的不同解析回调」
 *                   例如 AT+STAINFO? 在 IS_BUSY 状态下 port_name="busy_probe"
 *                   在 CONNECTED 状态下 port_name="connected_poll"，各自绑不同回调
 *  - monitor_state: 该项仅在模组处于此状态时才可能被触发(单状态过滤)
 *  - interval_ms  : 触发间隔，两次执行至少间隔此时间
 *  - rx_cb        : 响应解析回调
 *  - timeout_cb   : 超时回调
 *  - timeout_ms   : 单次指令超时时间
 *  - last_run_tick: 运行时记录上次执行的tick，用于「最久未执行」排序(引擎维护)
 *  - is_used      : 该槽位是否启用(引擎维护)
 */
typedef struct
{
    const char *cmd;                        /* AT指令字符串(静态常量) */
    const char *port_name;                  /* 端口名 */
    adx_monitor_state_t monitor_state;      /* 仅在此状态触发 */
    uint32_t interval_ms;                   /* 触发间隔(ms) */
    adx_at_recv_cb_t rx_cb;                 /* 响应回调 */
    adx_at_timeout_cb_t timeout_cb;         /* 超时回调 */
    uint32_t timeout_ms;                    /* 指令超时(ms) */

    /* ---- 运行时字段(引擎维护，注册时清零) ---- */
    adx_tick_t last_run_tick;               /* 上次执行tick */
    uint8_t is_used;                        /* 槽位启用标志 */
} adx_map_item_t;

/* ======================================================================== */
/*                          动态队列条目                                     */
/* ======================================================================== */

typedef struct
{
    char cmd[ADX_CMD_BUFFER_SIZE];          /* AT指令(含\r\n) */
    uint16_t cmd_len;                       /* 指令长度 */
    adx_at_recv_cb_t rx_cb;                 /* 响应回调 */
    adx_at_timeout_cb_t timeout_cb;         /* 超时回调 */
    uint32_t timeout_ms;                    /* 超时(ms) */
} adx_queue_item_t;

/* ======================================================================== */
/*                          URC回调表条目                                    */
/* ======================================================================== */

typedef struct
{
    char name[ADX_URC_NAME_LEN];            /* 注册名称(调试用) */
    adx_urc_cb_t urc_cb;                    /* URC回调 */
    uint8_t is_used;                        /* 槽位启用标志 */
} adx_urc_entry_t;

/* ======================================================================== */
/*                              对外API                                      */
/* ======================================================================== */

/**
 * @brief 引擎初始化
 *        内部完成：UART初始化(port)、状态机复位
 *        在开始调用 polling() 之前调用一次
 * @return ADX_OK / ADX_FAIL
 */
int adx_at_engine_init(void);

/**
 * @brief 链式反应主循环(非阻塞)
 *
 * 每次调用执行一轮：收串口帧 → URC分发 → AT状态机推进一步
 * 移植者需高频调用此函数：
 *   - RTOS：在任务 while(1) 里调用，配合 vTaskDelay 控制节奏
 *   - 裸机：在 main while(1) 里调用
 *
 * 引擎内部用时间戳控制实际执行节奏(ADX_LOOP_INTERVAL_MS / ADX_RESP_OK_COOLDOWN_MS)，
 * 不会阻塞，高频调用是安全的。
 */
void adx_chain_reaction_polling(void);

/* ---- 监控Map管理 ---- */

/**
 * @brief 向监控Map注册一个条目
 * @param item 条目内容(cmd/port_name/状态/间隔/回调等)
 * @return ADX_OK=成功, ADX_FAIL=表满或参数错误
 */
int adx_at_map_register(const adx_map_item_t *item);

/**
 * @brief 通过port_name查找并更新Map条目的回调
 * @param port_name 端口名
 * @param rx_cb     新的响应回调(传NULL表示不修改)
 * @param timeout_cb 新的超时回调(传NULL表示不修改)
 * @return ADX_OK=成功, ADX_FAIL=未找到
 */
int adx_at_map_update_callback(const char *port_name,
                               adx_at_recv_cb_t rx_cb,
                               adx_at_timeout_cb_t timeout_cb);

/* ---- 动态队列 ---- */

/**
 * @brief 开发者向动态队列插入一条AT指令(高优先级)
 *        队列指令优先于Map指令执行，每处理1条队列指令穿插1次map检查
 * @param cmd        AT指令字符串(含\r\n)
 * @param rx_cb      响应回调(可为NULL，表示不解析响应)
 * @param timeout_ms 超时时间(ms)
 * @param timeout_cb 超时回调(可为NULL)
 * @return ADX_OK=成功入队, ADX_FAIL=队列满或参数错误
 */
int adx_at_enqueue(const char *cmd,
                   adx_at_recv_cb_t rx_cb,
                   uint32_t timeout_ms,
                   adx_at_timeout_cb_t timeout_cb);

/* ---- 状态机访问 ---- */

/**
 * @brief 设置当前模组监控状态(线程安全)
 *        URC回调和AT回调内部调用此函数推进状态机
 */
void adx_at_monitor_state_set(adx_monitor_state_t new_state);

/**
 * @brief 获取当前模组监控状态(线程安全)
 */
adx_monitor_state_t adx_at_monitor_state_get(void);

/* ---- URC全局回调 ---- */

/**
 * @brief URC全局监听函数注册
 * @param name     注册名称(便于调试)
 * @param callback 回调函数
 * @return ADX_OK=成功, ADX_FAIL=表满
 */
int adx_at_urc_register(const char *name, adx_urc_cb_t callback);

/**
 * @brief URC全局轮询分发(引擎内部调用，开发者通常无需直接调用)
 */
int adx_at_urc_polling(const uint8_t *buffer, uint16_t len);

/* ---- 调试/查询 ---- */

/**
 * @brief 获取当前AT状态机状态(调试用)
 */
adx_at_state_t adx_at_engine_state_get(void);

/**
 * @brief 获取动态队列当前待处理数量
 */
uint8_t adx_at_queue_count_get(void);

#endif /* _ADX_AT_ENGINE_H_ */
