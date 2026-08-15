/**
 * @file adx_at_engine_usage_example.c
 * @author Zorian(1551769443@qq.com)
 * @brief  ADX AT指令引擎使用示例
 *         演示如何用新引擎替换原 task_mobile_monitor 中散装的 mobile_at_xxx_run()
 *         本文件仅作示例，不参与编译(可在Makefile中排除)
 * @version 0.1
 * @date 2026-08-16
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "adx_at_engine.h"
#include <string.h>

/* ======================================================================== */
/*                  示例1: 响应回调函数(从原代码平移)                        */
/* ======================================================================== */

/* AT探测回调：收到OK推进状态机 */
static int example_at_test_recv_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((char *)buffer, "OK\r\n") != NULL)
    {
        adx_at_monitor_state_set(ADX_MONITOR_STATE_NOT_CONNECTED);
        return ADX_OK;
    }
    return ADX_FAIL;
}

static int example_at_timeout_cb(char *cmd)
{
    (void)cmd;
    /* 超时回UNKNOWN状态重试 */
    adx_at_monitor_state_set(ADX_MONITOR_STATE_UNKNOWN);
    return ADX_OK;
}

/**
 * @brief AT+STAINFO? 在 IS_BUSY 状态下的解析
 *        port_name = "stainfo_busy_probe"
 */
static int example_stainfo_busy_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    const char *pos = strstr((char *)buffer, "+STAINFO:");
    if (pos != NULL)
    {
        char code = *(pos + 9U);
        switch (code)
        {
        case '3':
            adx_at_monitor_state_set(ADX_MONITOR_STATE_CONNECTED);
            return ADX_OK;
        case '4':
            adx_at_monitor_state_set(ADX_MONITOR_STATE_FAILED);
            return ADX_OK;
        default:
            return ADX_OK; /* 0/1/2 状态保持 */
        }
    }
    return ADX_FAIL;
}

/**
 * @brief AT+STAINFO? 在 CONNECTED 状态下的巡检解析
 *        port_name = "stainfo_connected_poll"
 *        注意：同一AT指令，不同port_name，不同回调！
 */
static int example_stainfo_connected_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    const char *pos = strstr((char *)buffer, "+STAINFO:");
    if (pos != NULL)
    {
        char code = *(pos + 9U);
        if (code != '3')
        {
            /* 巡检发现掉线 */
            adx_at_monitor_state_set(ADX_MONITOR_STATE_IP_LOST);
        }
        return ADX_OK;
    }
    return ADX_FAIL;
}

static int example_wrssi_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((char *)buffer, "+WRSSI:") != NULL &&
        strstr((char *)buffer, "OK") != NULL)
    {
        /* 实际项目中此处解析RSSI并更新全局变量 */
        return ADX_OK;
    }
    return ADX_FAIL;
}

/* ======================================================================== */
/*                  示例2: URC回调注册(从原代码平移)                         */
/* ======================================================================== */

static int example_urc_wifi_got_ip(const uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "+EVENT:WIFI_GOT_IP") != NULL)
    {
        adx_at_monitor_state_set(ADX_MONITOR_STATE_CONNECTED);
        return ADX_OK;
    }
    return ADX_FAIL;
}

static int example_urc_ble_connect(const uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "+EVENT:BLE_CONNECT") != NULL)
    {
        /* 退出透传模式：往动态队列插入+++指令 */
        adx_at_enqueue("+++", NULL, 1000U, NULL);
        return ADX_OK;
    }
    return ADX_FAIL;
}

/* ======================================================================== */
/*                  示例3: 监控Map注册(替代原状态机switch)                   */
/* ======================================================================== */

/**
 * @brief 初始化AT引擎并注册所有监控项
 *
 * 对比原设计：
 *   原来在 task_mobile_monitor_running() 里用 switch(state) 分派
 *   mobile_at_command_test_run / mobile_at_wjap_run / mobile_at_command_busy_run 等
 *   现在全部改成「Map项注册」，引擎自动按「状态匹配+最久未执行」调度
 */
void example_at_engine_init(void)
{
    /* 1. 引擎初始化(内部调用port层完成UART初始化，不创建任务) */
    adx_at_engine_init();

    /* 2. 注册URC全局回调 */
    adx_at_urc_register("wifi_got_ip", example_urc_wifi_got_ip);
    adx_at_urc_register("ble_connect", example_urc_ble_connect);

    /* 3. 注册监控Map条目 */
    /* 注意 port_name 的作用：AT+STAINFO? 在不同状态下注册多次，绑定不同回调 */

    /* UNKNOWN状态：发AT探测模组存活 */
    const adx_map_item_t item_at_test = {
        .cmd = "AT\r\n",
        .port_name = "at_alive_probe",
        .monitor_state = ADX_MONITOR_STATE_UNKNOWN,
        .interval_ms = 3000U,
        .rx_cb = example_at_test_recv_cb,
        .timeout_cb = example_at_timeout_cb,
        .timeout_ms = 420U,
    };
    adx_at_map_register(&item_at_test);

    /* IS_BUSY状态：5秒一次AT+STAINFO? 探测是否脱离busy */
    const adx_map_item_t item_stainfo_busy = {
        .cmd = "AT+STAINFO?\r\n",
        .port_name = "stainfo_busy_probe",
        .monitor_state = ADX_MONITOR_STATE_IS_BUSY,
        .interval_ms = 5000U,
        .rx_cb = example_stainfo_busy_cb,
        .timeout_cb = example_at_timeout_cb,
        .timeout_ms = 5000U,
    };
    adx_at_map_register(&item_stainfo_busy);

    /* CONNECTED状态：30秒一次AT+STAINFO? 巡检 */
    /* 同一指令，不同port_name，不同回调！ */
    const adx_map_item_t item_stainfo_conn = {
        .cmd = "AT+STAINFO?\r\n",
        .port_name = "stainfo_connected_poll",
        .monitor_state = ADX_MONITOR_STATE_CONNECTED,
        .interval_ms = 30000U,
        .rx_cb = example_stainfo_connected_cb,
        .timeout_cb = example_at_timeout_cb,
        .timeout_ms = 500U,
    };
    adx_at_map_register(&item_stainfo_conn);

    /* CONNECTED状态：30秒一次RSSI查询 */
    const adx_map_item_t item_wrssi = {
        .cmd = "AT+WRSSI?\r\n",
        .port_name = "wrssi_connected_poll",
        .monitor_state = ADX_MONITOR_STATE_CONNECTED,
        .interval_ms = 30000U,
        .rx_cb = example_wrssi_cb,
        .timeout_cb = example_at_timeout_cb,
        .timeout_ms = 5000U,
    };
    adx_at_map_register(&item_wrssi);

    /* IP_LOST状态：5秒一次AT+STAINFO? 复查 */
    const adx_map_item_t item_stainfo_iplost = {
        .cmd = "AT+STAINFO?\r\n",
        .port_name = "stainfo_iplost_retry",
        .monitor_state = ADX_MONITOR_STATE_IP_LOST,
        .interval_ms = 5000U,
        .rx_cb = example_stainfo_busy_cb, /* 复用解析逻辑 */
        .timeout_cb = example_at_timeout_cb,
        .timeout_ms = 500U,
    };
    adx_at_map_register(&item_stainfo_iplost);
}

/* ======================================================================== */
/*                  示例4: 开发者运行时插入临时指令                          */
/* ======================================================================== */

/**
 * @brief 演示业务代码如何往队列里塞临时AT指令
 *        比如HTTP上传、MQTT发布等，优先于监控指令执行
 */
void example_business_send_http(void)
{
    /* 这条指令会插队到监控Map指令之前执行 */
    adx_at_enqueue("AT+HTTPGET=http://api.example.com/time\r\n",
                   NULL,                /* 可不设响应回调，让URC处理 */
                   5000U,
                   example_at_timeout_cb);
}

/**
 * @brief 运行时替换某个port_name的回调
 *        比如配网模式切换后，需要改变AT+WJAP的解析逻辑
 */
void example_switch_callback(void)
{
    adx_at_map_update_callback("at_alive_probe",
                               example_at_test_recv_cb,
                               NULL);
}

/* ======================================================================== */
/*                  示例5: 查询引擎状态(调试用)                              */
/* ======================================================================== */

void example_debug_print_status(void)
{
    adx_at_state_t state = adx_at_engine_state_get();
    uint8_t queue_cnt = adx_at_queue_count_get();
    adx_monitor_state_t monitor = adx_at_monitor_state_get();
    (void)state;
    (void)queue_cnt;
    (void)monitor;
    /* 实际项目中这里打印到调试串口 */
}

/* ======================================================================== */
/*        示例6: 裸机模式 main 循环(关键！展示如何驱动引擎)                  */
/* ======================================================================== */
/*
 * 裸机模式下，移植者需要：
 *   1. 在硬件定时器中断(如SysTick)里每1ms调用一次 adx_heartbeat()
 *   2. 在main的while(1)里高频调用 adx_chain_reaction_polling()
 *
 * 引擎内部是非阻塞的，高频调用是安全的。
 */

/* 假设这是STM32的SysTick中断处理函数 */
void SysTick_Handler(void)
{
    /* 心跳函数，每1ms调用一次(ADX_HEARTBEAT_PERIOD_MS=1) */
    adx_heartbeat();
}

/* 裸机main函数示例 */
void baremetal_main(void)
{
    /* 系统初始化 */
    /* HAL_Init(); */
    /* SystemClock_Config(); */
    /* MX_GPIO_Init(); */
    /* MX_USART1_UART_Init(); */
    /* SysTick_Config(SystemCoreClock / 1000); */ /* 1ms中断 */

    /* ADX引擎初始化 + 注册监控项 */
    example_at_engine_init();

    /* 主循环 */
    while (1)
    {
        /* 链式反应轮询，驱动整个引擎(非阻塞)
         * 引擎内部读 adx_tick_get_now() 获取时间，不调heartbeat */
        adx_chain_reaction_polling();

        /* 这里可以处理其他业务 */
        /* ... */
    }
}

/* ======================================================================== */
/*        示例7: FreeRTOS模式 任务函数(展示如何驱动引擎)                     */
/* ======================================================================== */
/*
 * FreeRTOS模式下，移植者需要：
 *   1. 创建一个任务运行引擎
 *   2. 在任务里按顺序调用：polling() → heartbeat() → vTaskDelay()
 *   3. delay 时间应与 ADX_HEARTBEAT_PERIOD_MS 相等或相近
 *
 * 调用顺序很关键：
 *   polling 先执行，读取当前tick值推进引擎
 *   heartbeat 后执行，自增tick为下一轮做准备
 *   vTaskDelay 让出CPU，等待下一轮
 */
#include "FreeRTOS.h"
#include "task.h"

static void adx_engine_task(void *arg)
{
    (void)arg;

    /* 引擎初始化 + 注册监控项 */
    example_at_engine_init();

    while (1)
    {
        /* 1. 链式反应轮询(读当前tick，推进状态机) */
        adx_chain_reaction_polling();

        /* 2. 心跳自增(为下一轮polling提供时间基准) */
        adx_heartbeat();

        /* 3. 延时与心跳周期一致(ADX_HEARTBEAT_PERIOD_MS=1ms) */
        vTaskDelay(pdMS_TO_TICKS(ADX_HEARTBEAT_PERIOD_MS));
    }
}

void freertos_main(void)
{
    /* HAL_Init(); */
    /* SystemClock_Config(); */
    /* MX_USART1_UART_Init(); */

    xTaskCreate(adx_engine_task, "AdxEngine", 1024, NULL, 5, NULL);
    vTaskStartScheduler();
}

/* ======================================================================== */
/*        示例8: RT-Thread模式 线程函数(展示如何驱动引擎)                    */
/* ======================================================================== */
/*
 * RT-Thread模式下，调用顺序与FreeRTOS一致：
 *   polling() → heartbeat() → rt_thread_mdelay()
 */
#ifdef RT_USING_RTTHREAD
#include <rtthread.h>

static void adx_engine_thread_entry(void *arg)
{
    (void)arg;

    example_at_engine_init();

    while (1)
    {
        adx_chain_reaction_polling();
        adx_heartbeat();
        rt_thread_mdelay(ADX_HEARTBEAT_PERIOD_MS);
    }
}

void rtthread_main(void)
{
    rt_thread_t tid = rt_thread_create("AdxEngine",
                                        adx_engine_thread_entry,
                                        RT_NULL,
                                        1024,
                                        5,
                                        10);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
    }
}
#endif
