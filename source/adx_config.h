/**
 * @file adx_config.h
 * @author Zorian(1551769443@qq.com)
 * @brief  ADX (AT Device X) 配置中心
 *         缓冲区大小、心跳基准、临界区策略、轮询参数等
 *         移植时只需修改本文件，无需改动引擎核心
 *
 * 运行模式说明：
 *   ADX 统一采用「非阻塞轮询」架构，不依赖任何 RTOS 任务机制。
 *   引擎核心提供两个函数由移植者驱动：
 *     - adx_heartbeat()              心跳函数，提供时间基准(周期调用)
 *     - adx_chain_reaction_polling() 链式反应主循环(高频调用)
 *   RTOS 项目：在任务 while(1) 里调用 polling + heartbeat
 *   裸机项目：在 main while(1) 或定时器中断里调用
 *
 * @version 0.2
 * @date 2026-08-16
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef _ADX_CONFIG_H_
#define _ADX_CONFIG_H_

#include <stdint.h>

/* ======================================================================== */
/*                        心跳时间基准配置                                    */
/* ======================================================================== */
/*
 * 心跳机制说明：
 *   adx_heartbeat() 内部维护一个 volatile uint32_t 计数器，每次调用自增1。
 *   移植者必须保证调用频率恒定，且与 ADX_HEARTBEAT_PERIOD_MS 一致。
 *   例如 ADX_HEARTBEAT_PERIOD_MS=1，则移植者需每 1ms 调用一次 adx_heartbeat()。
 *
 *   常见放置位置：
 *     - SysTick / Timer 中断里(最推荐，精度高)
 *     - RTOS 软件定时器回调里
 *     - 裸机主循环里配合硬件延时(精度较低)
 *
 *   adx_port_get_tick() 直接返回这个计数器，引擎所有时间判断基于此。
 */

#ifndef ADX_HEARTBEAT_PERIOD_MS
#define ADX_HEARTBEAT_PERIOD_MS (1U) /* 心跳周期，单位ms，默认1ms一次 */
#endif

/* ======================================================================== */
/*                        临界区策略配置                                     */
/* ======================================================================== */
/*
 * 临界区用于保护 Map/队列/状态变量 不会被中断与主循环同时访问。
 * 根据实际场景二选一：
 *
 *   ADX_CRITICAL_USE_DISABLE_IRQ  - 关中断实现(默认，安全)
 *     port 层用 __disable_irq()/__enable_irq() 或等效API实现
 *     适用于：中断里也可能访问引擎数据的场景
 *
 *   ADX_CRITICAL_USE_NONE         - 空实现(无保护)
 *     port 层 enter/exit_critical 为空函数
 *     适用于：确认主循环是唯一访问者，中断不碰引擎数据的场景
 */

#ifndef ADX_CRITICAL_USE_DISABLE_IRQ
#ifndef ADX_CRITICAL_USE_NONE
#define ADX_CRITICAL_USE_DISABLE_IRQ /* 默认启用关中断临界区 */
#endif
#endif

#if defined(ADX_CRITICAL_USE_DISABLE_IRQ) && defined(ADX_CRITICAL_USE_NONE)
#error "adx_config: ADX_CRITICAL_USE_DISABLE_IRQ 与 ADX_CRITICAL_USE_NONE 不能同时启用"
#endif

/* ======================================================================== */
/*                        缓冲区大小配置                                     */
/* ======================================================================== */

/* AT指令发送缓冲区(单条指令最大长度，含\r\n和'\0') */
#ifndef ADX_CMD_BUFFER_SIZE
#define ADX_CMD_BUFFER_SIZE (256U)
#endif

/* AT响应接收窗口缓冲区(一次接收窗口聚合的最大数据量) */
#ifndef ADX_RX_WINDOW_BUFFER_SIZE
#define ADX_RX_WINDOW_BUFFER_SIZE (1024U)
#endif

/* ======================================================================== */
/*                        引擎参数配置                                       */
/* ======================================================================== */

/* 接收窗口时间(ms)：多少ms内无新帧则认为本次响应聚合完成 */
#ifndef ADX_RX_WINDOW_MS
#define ADX_RX_WINDOW_MS (320U)
#endif

/* 监控Map最大条目数 */
#ifndef ADX_MAP_SIZE
#define ADX_MAP_SIZE (32U)
#endif

/* 动态队列最大条目数 */
#ifndef ADX_QUEUE_SIZE
#define ADX_QUEUE_SIZE (16U)
#endif

/* URC回调表最大条目数 */
#ifndef ADX_URC_TABLE_SIZE
#define ADX_URC_TABLE_SIZE (16U)
#endif

/* portName最大长度(含'\0') */
#ifndef ADX_PORT_NAME_LEN
#define ADX_PORT_NAME_LEN (32U)
#endif

/* URC注册名称最大长度(含'\0') */
#ifndef ADX_URC_NAME_LEN
#define ADX_URC_NAME_LEN (24U)
#endif

/* ======================================================================== */
/*                        轮询节奏配置                                       */
/* ======================================================================== */
/*
 * 以下两个参数控制链式反应轮询的节奏。
 * 引擎内部用时间戳判断是否到期，不阻塞，移植者可按需调整。
 *
 *   ADX_RESP_OK_COOLDOWN_MS  - RESP_OK后冷却时间，让模组喘息后再发下一条
 *   ADX_LOOP_INTERVAL_MS     - 非冷却状态下的最小轮询间隔(0=每次都跑)
 *
 * 注意：这些不是延时，而是时间戳判断。移植者仍需高频调用 polling()，
 *       引擎内部会自动按这些参数控制实际执行节奏。
 */

#ifndef ADX_RESP_OK_COOLDOWN_MS
#define ADX_RESP_OK_COOLDOWN_MS (50U)
#endif

#ifndef ADX_LOOP_INTERVAL_MS
#define ADX_LOOP_INTERVAL_MS (5U)
#endif

/* 串口读帧默认超时(ms) */
#ifndef ADX_UART_READ_TIMEOUT_MS
#define ADX_UART_READ_TIMEOUT_MS (10U)
#endif

/* 互斥锁等待超时(ms) */
#ifndef ADX_MUTEX_WAIT_TIMEOUT_MS
#define ADX_MUTEX_WAIT_TIMEOUT_MS (1000U)
#endif

#endif /* _ADX_CONFIG_H_ */
