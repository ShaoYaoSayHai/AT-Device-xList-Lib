/**
 * @file adx_port.c
 * @author Zorian(1551769443@qq.com)
 * @brief  ADX (AT Device X) 硬件/RTOS抽象层实现
 *
 * 本文件为移植层，心跳/时间接口已有默认实现，其余函数为空实现/桩实现。
 * 移植时根据目标平台用对应API填充各函数即可。
 *
 * 心跳机制：
 *   adx_heartbeat() 内部维护 volatile uint32_t 计数器，每次调用自增1。
 *   移植者需保证调用频率 = ADX_HEARTBEAT_PERIOD_MS。
 *   adx_port_get_tick() 直接返回该计数器，引擎所有时间判断基于此。
 *
 * @version 0.2
 * @date 2026-08-16
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "adx_port.h"

/* ======================================================================== */
/*                    1. 心跳与时间接口(已有默认实现)                        */
/* ======================================================================== */
/*
 * 心跳计数器：volatile 保证中断与主循环都能安全读取
 * 移植者无需修改此处，只需保证周期性调用 adx_heartbeat() 即可
 *
 * 用法：移植者在 polling() 之后调用 heartbeat()，配合一个与
 *       ADX_HEARTBEAT_PERIOD_MS 相等的延时(vTaskDelay/rt_thread_mdelay)
 *
 *   while (1) {
 *       adx_chain_reaction_polling();   // 推进引擎(读当前tick)
 *       adx_heartbeat();                // 心跳自增(更新tick)
 *       vTaskDelay(ADX_HEARTBEAT_PERIOD_MS);  // 延时与心跳周期一致
 *   }
 *
 * 裸机中断模式下，heartbeat() 放 SysTick 中断里，polling() 放 main 循环里。
 */
static volatile adx_tick_t s_heartbeat_tick = 0U;

adx_tick_t adx_heartbeat(void)
{
    s_heartbeat_tick++;
    return s_heartbeat_tick;
}

adx_tick_t adx_tick_get_now(void)
{
    return s_heartbeat_tick;
}

adx_tick_t adx_tick_from_ms(uint32_t ms)
{
    /* 根据 ADX_HEARTBEAT_PERIOD_MS 将毫秒换算为 tick 数 */
    return (adx_tick_t)(ms / ADX_HEARTBEAT_PERIOD_MS);
}

/* ======================================================================== */
/*                2. 串口收发(空实现，移植时填充)                            */
/* ======================================================================== */
/*
 * 移植参考(FreeRTOS + STM32 HAL):
 *
 * int adx_port_uart_init(void) {
 *     // HAL_UART_Receive_DMA(&huart1, rx_buf, sizeof(rx_buf));
 *     // __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
 *     return ADX_OK;
 * }
 *
 * int adx_port_uart_send(const uint8_t *data, uint16_t len) {
 *     // HAL_UART_Transmit(&huart1, data, len, 100);
 *     return ADX_OK;
 * }
 *
 * int adx_port_uart_read_frame(uint8_t *out_buf, uint16_t buf_size,
 *                              uint16_t *out_frame_len, uint32_t timeout_ms) {
 *     // 从DMA空闲中断填充的环形缓冲区取一帧
 *     // *out_frame_len = frame_len;
 *     return ADX_OK;
 * }
 */

int adx_port_uart_init(void)
{
    /* TODO: 移植时实现UART+DMA初始化 */
    return ADX_OK;
}

int adx_port_uart_send(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    /* TODO: 移植时实现串口发送 */
    return ADX_OK;
}

int adx_port_uart_read_frame(uint8_t *out_buf,
                             uint16_t buf_size,
                             uint16_t *out_frame_len,
                             uint32_t timeout_ms)
{
    (void)out_buf;
    (void)buf_size;
    (void)timeout_ms;
    if (out_frame_len != NULL)
    {
        *out_frame_len = 0U;
    }
    /* TODO: 移植时实现串口帧读取 */
    return ADX_FAIL;
}

/* ======================================================================== */
/*                3. 临界区(根据配置自动选择实现)                            */
/* ======================================================================== */

#if defined(ADX_CRITICAL_USE_DISABLE_IRQ)

/* ---- 关中断实现(默认，安全) ---- */
/*
 * 移植参考(STM32):
 *   void adx_port_enter_critical(void) { __disable_irq(); }
 *   void adx_port_exit_critical(void)  { __enable_irq(); }
 *
 * 移植参考(FreeRTOS):
 *   void adx_port_enter_critical(void) { taskENTER_CRITICAL(); }
 *   void adx_port_exit_critical(void)  { taskEXIT_CRITICAL(); }
 */
void adx_port_enter_critical(void)
{
    /* TODO: 移植时实现关中断，如 __disable_irq(); */
}

void adx_port_exit_critical(void)
{
    /* TODO: 移植时实现开中断，如 __enable_irq(); */
}

#elif defined(ADX_CRITICAL_USE_NONE)

/* ---- 空实现(确认无中断竞争时使用) ---- */
void adx_port_enter_critical(void) {}
void adx_port_exit_critical(void)  {}

#endif

/* ======================================================================== */
/*                4. 互斥锁(空实现，移植时填充)                              */
/* ======================================================================== */
/*
 * 移植参考(FreeRTOS):
 *   adx_mutex_t adx_port_mutex_create(void) {
 *       return (adx_mutex_t)xSemaphoreCreateMutex();
 *   }
 *   int adx_port_mutex_lock(adx_mutex_t m, uint32_t timeout_ms) {
 *       return (xSemaphoreTake((SemaphoreHandle_t)m, pdMS_TO_TICKS(timeout_ms)) == pdPASS)
 *              ? ADX_OK : ADX_FAIL;
 *   }
 *   int adx_port_mutex_unlock(adx_mutex_t m) {
 *       return (xSemaphoreGive((SemaphoreHandle_t)m) == pdPASS) ? ADX_OK : ADX_FAIL;
 *   }
 *
 * 裸机无互斥锁时：临界区已足够保护，互斥锁可直接返回成功。
 */

adx_mutex_t adx_port_mutex_create(void)
{
    /* TODO: 移植时实现互斥锁创建 */
    return NULL;
}

void adx_port_mutex_destroy(adx_mutex_t mutex)
{
    (void)mutex;
    /* TODO: 移植时实现互斥锁销毁 */
}

int adx_port_mutex_lock(adx_mutex_t mutex, uint32_t timeout_ms)
{
    (void)mutex;
    (void)timeout_ms;
    /* TODO: 移植时实现互斥锁获取 */
    return ADX_OK;
}

int adx_port_mutex_unlock(adx_mutex_t mutex)
{
    (void)mutex;
    /* TODO: 移植时实现互斥锁释放 */
    return ADX_OK;
}
