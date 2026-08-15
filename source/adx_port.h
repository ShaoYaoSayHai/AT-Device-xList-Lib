/**
 * @file adx_port.h
 * @author Zorian(1551769443@qq.com)
 * @brief  ADX (AT Device X) 硬件/RTOS抽象层接口
 *
 * port层职责（精简为4类，无任务管理）：
 *   1. 心跳时间：adx_heartbeat / adx_port_get_tick / adx_port_tick_from_ms
 *   2. 串口收发：adx_port_uart_init / adx_port_uart_send / adx_port_uart_read_frame
 *   3. 临界区  ：adx_port_enter_critical / adx_port_exit_critical
 *   4. 互斥锁  ：adx_port_mutex_create / adx_port_mutex_lock / adx_port_mutex_unlock
 *
 * 引擎核心(adx_at_engine.c)只调用本文件声明的接口，不直接依赖任何RTOS或硬件。
 * 移植时只需实现 adx_port.c，引擎核心无需修改。
 *
 * 运行模式：
 *   ADX 统一采用非阻塞轮询，不创建任务。
 *   移植者在自己的任务或main循环里调用：
 *     - adx_heartbeat()              (周期调用，提供时间基准)
 *     - adx_chain_reaction_polling() (高频调用，驱动引擎)
 *
 * @version 0.2
 * @date 2026-08-16
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef _ADX_PORT_H_
#define _ADX_PORT_H_

#include "adx_config.h"
#include <stdint.h>

/* ======================================================================== */
/*                             基本类型                                      */
/* ======================================================================== */

/* tick类型(心跳计数器，对应毫秒数) */
typedef uint32_t adx_tick_t;

/* 互斥锁句柄(port内部定义为具体RTOS类型) */
typedef void *adx_mutex_t;

/* port层返回值：0=成功, -1=失败 */
#define ADX_OK (0)
#define ADX_FAIL (-1)

/* 无限等待标志(用于互斥锁) */
#define ADX_WAIT_FOREVER (0xFFFFFFFFU)

/* ======================================================================== */
/*                    1. 心跳与时间接口(核心)                                */
/* ======================================================================== */

/**
 * @brief 心跳函数，提供引擎的时间基准
 *
 * 内部维护一个 volatile uint32_t 计数器，每次调用自增1并返回新值。
 * 移植者必须保证调用频率恒定，且周期 = ADX_HEARTBEAT_PERIOD_MS。
 *
 * 典型用法(任务模式)：
 *   while (1) {
 *       adx_chain_reaction_polling();        // 推进引擎(内部读当前tick)
 *       adx_heartbeat();                     // 心跳自增(更新tick)
 *       vTaskDelay(ADX_HEARTBEAT_PERIOD_MS); // 延时与心跳周期一致
 *   }
 *
 * 典型用法(裸机中断模式)：
 *   void SysTick_Handler(void) { adx_heartbeat(); }  // 1ms中断
 *   while (1) { adx_chain_reaction_polling(); }
 *
 * @return 本次心跳后的计数器值
 */
adx_tick_t adx_heartbeat(void);

/**
 * @brief 获取当前心跳tick值(供引擎内部读取)
 *        移植者通常无需直接调用，引擎内部用此函数读当前时间
 * @return 当前tick值
 */
adx_tick_t adx_tick_get_now(void);

/**
 * @brief 毫秒转tick数
 *        根据 ADX_HEARTBEAT_PERIOD_MS 换算
 * @param ms 毫秒数
 * @return 对应的tick数
 */
adx_tick_t adx_tick_from_ms(uint32_t ms);

/* ======================================================================== */
/*                          2. 串口收发接口                                  */
/* ======================================================================== */

/**
 * @brief 串口初始化(UART/DMA配置)
 *         引擎首次polling前调用一次
 * @return ADX_OK / ADX_FAIL
 */
int adx_port_uart_init(void);

/**
 * @brief 串口发送数据(非阻塞或短阻塞发送)
 * @param data 待发送数据
 * @param len  数据长度
 * @return ADX_OK / ADX_FAIL
 */
int adx_port_uart_send(const uint8_t *data, uint16_t len);

/**
 * @brief 串口读取一帧数据(带超时)
 *         底层通常对接DMA+空闲中断或环形缓冲区
 * @param out_buf        输出缓冲区
 * @param buf_size       缓冲区容量
 * @param out_frame_len  实际读到的帧长度
 * @param timeout_ms     超时时间(ms)，0=非阻塞立即返回
 * @return ADX_OK=读到数据, ADX_FAIL=超时或无数据
 */
int adx_port_uart_read_frame(uint8_t *out_buf,
                             uint16_t buf_size,
                             uint16_t *out_frame_len,
                             uint32_t timeout_ms);

/* ======================================================================== */
/*                          3. 临界区接口                                    */
/* ======================================================================== */

/**
 * @brief 进入临界区
 *         根据 adx_config.h 的配置：
 *           ADX_CRITICAL_USE_DISABLE_IRQ -> 关中断实现
 *           ADX_CRITICAL_USE_NONE        -> 空实现
 * @note 不可嵌套，必须成对使用
 */
void adx_port_enter_critical(void);

/**
 * @brief 退出临界区
 */
void adx_port_exit_critical(void);

/* ======================================================================== */
/*                          4. 互斥锁接口                                    */
/* ======================================================================== */

/**
 * @brief 创建互斥锁
 * @return 互斥锁句柄，NULL=创建失败
 */
adx_mutex_t adx_port_mutex_create(void);

/**
 * @brief 销毁互斥锁
 */
void adx_port_mutex_destroy(adx_mutex_t mutex);

/**
 * @brief 获取互斥锁(阻塞)
 * @param mutex      互斥锁句柄
 * @param timeout_ms 超时(ms)，ADX_WAIT_FOREVER=永久等待
 * @return ADX_OK=获取成功, ADX_FAIL=超时
 */
int adx_port_mutex_lock(adx_mutex_t mutex, uint32_t timeout_ms);

/**
 * @brief 释放互斥锁
 * @return ADX_OK / ADX_FAIL
 */
int adx_port_mutex_unlock(adx_mutex_t mutex);

#endif /* _ADX_PORT_H_ */
