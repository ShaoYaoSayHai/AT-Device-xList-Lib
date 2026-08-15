/**
 * @file adx_at_engine.c
 * @author Zorian(1551769443@qq.com)
 * @brief  ADX (AT Device X) AT指令引擎核心实现
 *
 * 架构核心(非阻塞轮询式)：
 *   1. 统一轮询模式：不创建任务，由移植者调用 adx_chain_reaction_polling() 驱动
 *   2. 心跳时间基准：移植者在 polling 后调用 adx_heartbeat() 自增计数器
 *      引擎通过 adx_tick_get_now() 读取当前tick，用差值判断时间流逝
 *   3. 监控Map：全扫描取「最久未执行且到期」项触发，公平防饿死
 *   4. 动态队列：开发者插入的临时指令，优先于Map执行
 *   5. 调度策略：每处理1条队列指令，穿插1次Map检查(保底)
 *   6. AT状态机 IDLE->SEND->WAITING->RESP_OK/TIMEOUT
 *   7. 接收窗口聚合改为非阻塞状态机(原阻塞while循环不可用于裸机)
 *
 * 依赖关系：
 *   本文件只调用 adx_port.h 声明的接口，不直接依赖任何RTOS或硬件。
 *
 * @version 0.2
 * @date 2026-08-16
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "adx_at_engine.h"
#include <string.h>

/* ======================================================================== */
/*                          内部数据结构                                      */
/* ======================================================================== */

/* 接收窗口缓冲区 */
static uint8_t s_recv_window_buffer[ADX_RX_WINDOW_BUFFER_SIZE + 1U];

/* 监控Map表 */
static adx_map_item_t s_monitor_map[ADX_MAP_SIZE];

/* 动态队列(环形缓冲) */
static adx_queue_item_t s_dynamic_queue[ADX_QUEUE_SIZE];
static volatile uint8_t s_queue_head = 0U;
static volatile uint8_t s_queue_tail = 0U;
static volatile uint8_t s_queue_count = 0U;

/* URC回调表 */
static adx_urc_entry_t s_urc_table[ADX_URC_TABLE_SIZE];

/* AT状态机运行时字段 */
static adx_at_state_t s_at_state = ADX_AT_STATE_IDLE;
static adx_at_recv_cb_t s_current_rx_cb = NULL;
static adx_at_timeout_cb_t s_current_timeout_cb = NULL;
static char s_current_cmd[ADX_CMD_BUFFER_SIZE];
static uint16_t s_current_cmd_len = 0U;
static uint32_t s_current_timeout_ms = 0U;
static adx_tick_t s_last_send_tick = 0U;

/* 轮询节奏控制 */
static adx_tick_t s_last_poll_tick = 0U;

/* 模组监控状态(临界区保护) */
static volatile adx_monitor_state_t s_monitor_state = ADX_MONITOR_STATE_UNKNOWN;

/* ======================================================================== */
/*                       临界区保护的状态访问                                 */
/* ======================================================================== */

void adx_at_monitor_state_set(adx_monitor_state_t new_state)
{
    adx_port_enter_critical();
    s_monitor_state = new_state;
    adx_port_exit_critical();
}

adx_monitor_state_t adx_at_monitor_state_get(void)
{
    adx_monitor_state_t state;
    adx_port_enter_critical();
    state = s_monitor_state;
    adx_port_exit_critical();
    return state;
}

adx_at_state_t adx_at_engine_state_get(void)
{
    return s_at_state;
}

uint8_t adx_at_queue_count_get(void)
{
    uint8_t count;
    adx_port_enter_critical();
    count = s_queue_count;
    adx_port_exit_critical();
    return count;
}

/* ======================================================================== */
/*                       监控Map管理                                         */
/* ======================================================================== */

int adx_at_map_register(const adx_map_item_t *item)
{
    if (item == NULL || item->cmd == NULL || item->port_name == NULL)
    {
        return ADX_FAIL;
    }

    for (uint32_t i = 0U; i < ADX_MAP_SIZE; i++)
    {
        if (!s_monitor_map[i].is_used)
        {
            s_monitor_map[i].cmd = item->cmd;
            s_monitor_map[i].port_name = item->port_name;
            s_monitor_map[i].monitor_state = item->monitor_state;
            s_monitor_map[i].interval_ms = item->interval_ms;
            s_monitor_map[i].rx_cb = item->rx_cb;
            s_monitor_map[i].timeout_cb = item->timeout_cb;
            s_monitor_map[i].timeout_ms = item->timeout_ms;
            /* last_run_tick=0 表示从未执行，最久未执行优先级最高 */
            s_monitor_map[i].last_run_tick = 0U;
            s_monitor_map[i].is_used = 1U;
            return ADX_OK;
        }
    }
    return ADX_FAIL; /* 表满 */
}

int adx_at_map_update_callback(const char *port_name,
                               adx_at_recv_cb_t rx_cb,
                               adx_at_timeout_cb_t timeout_cb)
{
    if (port_name == NULL)
    {
        return ADX_FAIL;
    }

    for (uint32_t i = 0U; i < ADX_MAP_SIZE; i++)
    {
        if (s_monitor_map[i].is_used &&
            strncmp(s_monitor_map[i].port_name, port_name,
                    ADX_PORT_NAME_LEN) == 0)
        {
            if (rx_cb != NULL)
            {
                s_monitor_map[i].rx_cb = rx_cb;
            }
            if (timeout_cb != NULL)
            {
                s_monitor_map[i].timeout_cb = timeout_cb;
            }
            return ADX_OK;
        }
    }
    return ADX_FAIL; /* 未找到 */
}

/**
 * @brief 全扫描Map，找出「状态匹配 + 已到期 + 最久未执行」的条目
 *
 * 算法：
 *   1. 遍历所有 is_used 且 monitor_state == 当前状态 的条目
 *   2. 计算每个条目的「距上次执行时长」= now - last_run_tick
 *      (last_run_tick=0 表示从未执行，视为最大时长，最优先)
 *   3. 过滤掉 interval_ms 未到期的(即 elapsed < interval_ms)
 *   4. 在到期项中取 elapsed 最大的(即最久未执行)
 *
 * @param now        当前心跳tick值(由polling传入，避免重复调heartbeat)
 * @param out_item 输出选中的条目(浅拷贝)
 * @return ADX_OK=找到, ADX_FAIL=无到期项
 */
static int s_map_scan_pick_oldest(adx_tick_t now, adx_map_item_t *out_item)
{
    if (out_item == NULL)
    {
        return ADX_FAIL;
    }

    const adx_monitor_state_t cur_state = adx_at_monitor_state_get();

    int found = 0;
    adx_tick_t max_elapsed = 0U;
    uint32_t picked_idx = ADX_MAP_SIZE;

    for (uint32_t i = 0U; i < ADX_MAP_SIZE; i++)
    {
        if (!s_monitor_map[i].is_used)
        {
            continue;
        }
        /* 状态过滤：仅当前状态的条目参与 */
        if (s_monitor_map[i].monitor_state != cur_state)
        {
            continue;
        }

        /* 计算距上次执行的时长 */
        adx_tick_t elapsed;
        if (s_monitor_map[i].last_run_tick == 0U)
        {
            /* 从未执行过，视为最久(用最大值占位) */
            elapsed = (adx_tick_t)-1;
        }
        else
        {
            elapsed = now - s_monitor_map[i].last_run_tick;
        }

        /* 间隔过滤：未到期跳过 */
        if (s_monitor_map[i].interval_ms > 0U)
        {
            adx_tick_t interval_tick = adx_tick_from_ms(s_monitor_map[i].interval_ms);
            if (elapsed < interval_tick)
            {
                continue;
            }
        }

        /* 取最久未执行 */
        if (elapsed > max_elapsed)
        {
            max_elapsed = elapsed;
            picked_idx = i;
            found = 1;
        }
    }

    if (!found)
    {
        return ADX_FAIL;
    }

    /* 浅拷贝选中条目 */
    *out_item = s_monitor_map[picked_idx];
    /* 记录本次执行tick(直接写回原表) */
    s_monitor_map[picked_idx].last_run_tick = now;
    return ADX_OK;
}

/* ======================================================================== */
/*                       动态队列管理(环形缓冲)                              */
/* ======================================================================== */

int adx_at_enqueue(const char *cmd,
                   adx_at_recv_cb_t rx_cb,
                   uint32_t timeout_ms,
                   adx_at_timeout_cb_t timeout_cb)
{
    if (cmd == NULL)
    {
        return ADX_FAIL;
    }

    adx_port_enter_critical();
    if (s_queue_count >= ADX_QUEUE_SIZE)
    {
        adx_port_exit_critical();
        return ADX_FAIL; /* 队列满 */
    }

    adx_queue_item_t *slot = &s_dynamic_queue[s_queue_tail];
    uint16_t cmd_len = (uint16_t)strlen(cmd);
    if (cmd_len >= ADX_CMD_BUFFER_SIZE)
    {
        cmd_len = ADX_CMD_BUFFER_SIZE - 1U;
    }
    memcpy(slot->cmd, cmd, cmd_len);
    slot->cmd[cmd_len] = '\0';
    slot->cmd_len = cmd_len;
    slot->rx_cb = rx_cb;
    slot->timeout_cb = timeout_cb;
    slot->timeout_ms = timeout_ms;

    s_queue_tail = (uint8_t)((s_queue_tail + 1U) % ADX_QUEUE_SIZE);
    s_queue_count++;
    adx_port_exit_critical();
    return ADX_OK;
}

/**
 * @brief 从队列取出一条指令(空则返回失败)
 */
static int s_queue_dequeue(adx_queue_item_t *out)
{
    if (out == NULL)
    {
        return ADX_FAIL;
    }

    adx_port_enter_critical();
    if (s_queue_count == 0U)
    {
        adx_port_exit_critical();
        return ADX_FAIL; /* 队列空 */
    }

    *out = s_dynamic_queue[s_queue_head];
    s_queue_head = (uint8_t)((s_queue_head + 1U) % ADX_QUEUE_SIZE);
    s_queue_count--;
    adx_port_exit_critical();
    return ADX_OK;
}

/* ======================================================================== */
/*                       URC全局回调表                                       */
/* ======================================================================== */

int adx_at_urc_register(const char *name, adx_urc_cb_t callback)
{
    if (name == NULL || callback == NULL)
    {
        return ADX_FAIL;
    }

    for (uint32_t i = 0U; i < ADX_URC_TABLE_SIZE; i++)
    {
        if (!s_urc_table[i].is_used)
        {
            s_urc_table[i].is_used = 1U;
            strncpy(s_urc_table[i].name, name, ADX_URC_NAME_LEN - 1U);
            s_urc_table[i].name[ADX_URC_NAME_LEN - 1U] = '\0';
            s_urc_table[i].urc_cb = callback;
            return ADX_OK;
        }
    }
    return ADX_FAIL; /* 表满 */
}

int adx_at_urc_polling(const uint8_t *buffer, uint16_t len)
{
    if (buffer == NULL || len == 0U)
    {
        return ADX_FAIL;
    }

    for (uint32_t i = 0U; i < ADX_URC_TABLE_SIZE; i++)
    {
        if (s_urc_table[i].is_used)
        {
            int ret = s_urc_table[i].urc_cb(buffer, len);
            if (ret == ADX_OK)
            {
                return ADX_OK; /* 命中即返回 */
            }
        }
    }
    return ADX_FAIL;
}

/* ======================================================================== */
/*                       接收窗口聚合(非阻塞状态机)                          */
/* ======================================================================== */
/*
 * 原设计是阻塞式 while 循环读帧，裸机下不可用。
 * 改造为非阻塞状态机：每次 polling 调用推进一步。
 *
 * 状态：
 *   RX_WIN_IDLE     : 空闲，尝试读第一帧
 *   RX_WIN_COLLECTING : 已有首帧，窗口内继续拼接后续帧
 *
 * 配合 s_rx_win_last_recv_tick 和 s_rx_win_total_len 实现。
 */

typedef enum
{
    RX_WIN_IDLE = 0U,
    RX_WIN_COLLECTING,
} rx_win_state_t;

static rx_win_state_t s_rx_win_state = RX_WIN_IDLE;
static adx_tick_t s_rx_win_last_recv_tick = 0U;
static uint16_t s_rx_win_total_len = 0U;
static uint16_t s_rx_frame_len = 0U; /* 最近一次 polling 产出的完整帧长度 */

static void s_recv_buffer_reset(void)
{
    s_recv_window_buffer[0U] = '\0';
    s_rx_win_total_len = 0U;
    s_rx_frame_len = 0U;
}

/**
 * @brief 非阻塞接收窗口聚合(每次调用推进一步)
 *
 * @param now          当前心跳tick值(由polling传入)
 * @param out_frame_len 输出本次产出的完整帧长度(仅当返回ADX_OK时有效)
 * @return ADX_OK=本帧聚合完成可用, ADX_FAIL=正在聚合中/无数据
 *
 * 工作流程：
 *   IDLE: 非阻塞读一帧，读到则进入 COLLECTING，记录tick
 *   COLLECTING: 继续非阻塞读帧拼接；
 *               若窗口超时(ADX_RX_WINDOW_MS内无新帧)则判定聚合完成返回ADX_OK并回IDLE
 */
static int s_rx_window_step(adx_tick_t now, uint16_t *out_frame_len)
{
    if (out_frame_len == NULL)
    {
        return ADX_FAIL;
    }
    *out_frame_len = 0U;

    switch (s_rx_win_state)
    {
    case RX_WIN_IDLE:
    {
        /* 非阻塞尝试读第一帧 */
        uint16_t frame_len = 0U;
        if (adx_port_uart_read_frame(s_recv_window_buffer,
                                     ADX_RX_WINDOW_BUFFER_SIZE,
                                     &frame_len,
                                     0U) != ADX_OK || frame_len == 0U)
        {
            return ADX_FAIL; /* 无数据 */
        }

        s_rx_win_total_len = frame_len;
        s_recv_window_buffer[s_rx_win_total_len] = '\0';
        s_rx_win_last_recv_tick = now;
        s_rx_win_state = RX_WIN_COLLECTING;

        /* 继续落到 COLLECTING 分支尝试拼接(不return，下滚) */
        /* fallthrough */
    }
    /* fallthrough 故意不带break，让首帧后立即尝试拼接下一帧 */

    case RX_WIN_COLLECTING:
    {
        /* 尝试非阻塞读后续帧拼接 */
        uint16_t frame_len = 0U;
        uint16_t remaining = (uint16_t)(ADX_RX_WINDOW_BUFFER_SIZE - s_rx_win_total_len);

        if (remaining > 0U)
        {
            if (adx_port_uart_read_frame(&s_recv_window_buffer[s_rx_win_total_len],
                                         remaining,
                                         &frame_len,
                                         0U) == ADX_OK && frame_len > 0U)
            {
                s_rx_win_total_len = (uint16_t)(s_rx_win_total_len + frame_len);
                s_recv_window_buffer[s_rx_win_total_len] = '\0';
                s_rx_win_last_recv_tick = now;
            }
        }

        /* 判断窗口是否超时(距上次收帧超过 ADX_RX_WINDOW_MS) */
        adx_tick_t elapsed = now - s_rx_win_last_recv_tick;
        if (elapsed >= adx_tick_from_ms(ADX_RX_WINDOW_MS))
        {
            /* 窗口超时，聚合完成 */
            *out_frame_len = s_rx_win_total_len;
            s_rx_win_state = RX_WIN_IDLE;
            s_rx_win_total_len = 0U;
            return ADX_OK;
        }

        /* 窗口未超时，继续聚合中 */
        return ADX_FAIL;
    }

    default:
        s_rx_win_state = RX_WIN_IDLE;
        return ADX_FAIL;
    }
}

/* ======================================================================== */
/*                       指令加载与执行                                      */
/* ======================================================================== */

/**
 * @brief 把一条指令加载到AT状态机入口，进入SEND状态
 *        统一了Map条目和Queue条目的执行路径
 * @param now 当前心跳tick值(由polling传入)
 */
static void s_load_command(adx_tick_t now, const char *cmd, uint16_t cmd_len,
                           adx_at_recv_cb_t rx_cb,
                           adx_at_timeout_cb_t timeout_cb,
                           uint32_t timeout_ms)
{
    if (cmd_len >= ADX_CMD_BUFFER_SIZE)
    {
        cmd_len = ADX_CMD_BUFFER_SIZE - 1U;
    }
    memcpy(s_current_cmd, cmd, cmd_len);
    s_current_cmd[cmd_len] = '\0';
    s_current_cmd_len = cmd_len;

    s_current_rx_cb = rx_cb;
    s_current_timeout_cb = timeout_cb;
    s_current_timeout_ms = timeout_ms;

    s_recv_buffer_reset();
    (void)adx_port_uart_send((const uint8_t *)cmd, cmd_len);

    s_last_send_tick = now;
    s_at_state = ADX_AT_STATE_SEND;
}

/* ======================================================================== */
/*                       引擎初始化                                          */
/* ======================================================================== */

int adx_at_engine_init(void)
{
    /* static 变量零初始化，状态机复位即可 */
    s_at_state = ADX_AT_STATE_IDLE;
    s_current_rx_cb = NULL;
    s_current_timeout_cb = NULL;
    s_current_cmd_len = 0U;
    s_last_send_tick = 0U;

    s_rx_win_state = RX_WIN_IDLE;
    s_rx_win_total_len = 0U;
    s_rx_frame_len = 0U;

    s_last_poll_tick = 0U;

    /* port层UART初始化 */
    return adx_port_uart_init();
}

/* ======================================================================== */
/*                       链式反应主循环(非阻塞)                              */
/* ======================================================================== */
/*
 * 每次调用执行一轮：收串口帧 → URC分发 → AT状态机推进一步
 *
 * 节奏控制(非阻塞)：
 *   - RESP_OK 后冷却 ADX_RESP_OK_COOLDOWN_MS，让模组喘息
 *   - 其他状态最小间隔 ADX_LOOP_INTERVAL_MS
 *   - 用时间戳判断，不阻塞，移植者可高频调用
 *
 * 「队列优先+map保底」体现在 IDLE 分支：
 *   每次IDLE先看队列，队列空才扫map；
 *   但一旦队列指令处理完回到IDLE，下一轮立刻有机会扫map，
 *   保证map指令不会因队列持续有数据而彻底饿死。
 */
void adx_chain_reaction_polling(void)
{
    /* 读取当前心跳tick值(引擎只读不自增，自增由移植者调heartbeat完成) */
    adx_tick_t now = adx_tick_get_now();

    /* 节奏控制：距上次执行不足间隔则跳过 */
    adx_tick_t interval = adx_tick_from_ms(
        (s_at_state == ADX_AT_STATE_RESP_OK) ? ADX_RESP_OK_COOLDOWN_MS : ADX_LOOP_INTERVAL_MS);
    if ((now - s_last_poll_tick) < interval)
    {
        return;
    }
    s_last_poll_tick = now;

    /* ============== [1] 串口接收 + URC分发 ============== */
    uint16_t frame_len = 0U;
    if (s_rx_window_step(now, &frame_len) == ADX_OK)
    {
        if (frame_len > 0U)
        {
            s_rx_frame_len = frame_len;
            (void)adx_at_urc_polling(s_recv_window_buffer, frame_len);
        }
    }

    /* ============== [2] AT状态机 ============== */
    switch (s_at_state)
    {
    case ADX_AT_STATE_IDLE:
    {
        /* 优先取队列指令 */
        adx_queue_item_t q_item;
        if (s_queue_dequeue(&q_item) == ADX_OK)
        {
            s_load_command(now, q_item.cmd, q_item.cmd_len,
                           q_item.rx_cb, q_item.timeout_cb, q_item.timeout_ms);
            break;
        }
        /* 队列空，扫Map找最久未执行的到期项 */
        adx_map_item_t m_item;
        if (s_map_scan_pick_oldest(now, &m_item) == ADX_OK)
        {
            uint16_t cmd_len = (uint16_t)strlen(m_item.cmd);
            s_load_command(now, m_item.cmd, cmd_len,
                           m_item.rx_cb, m_item.timeout_cb, m_item.timeout_ms);
        }
        /* 都没有则保持IDLE */
        break;
    }

    case ADX_AT_STATE_SEND:
    {
        /* 发送动作在 s_load_command 中已完成，直接转等待 */
        s_at_state = ADX_AT_STATE_WAITING;
        break;
    }

    case ADX_AT_STATE_WAITING:
    {
        int wait_ret = ADX_FAIL;
        if (s_current_rx_cb != NULL && s_rx_frame_len > 0U)
        {
            wait_ret = s_current_rx_cb(s_recv_window_buffer, s_rx_frame_len);
        }
        if (wait_ret == ADX_OK)
        {
            /* 回调成功，清理并复位 */
            s_current_rx_cb = NULL;
            s_at_state = ADX_AT_STATE_RESP_OK;
            s_rx_frame_len = 0U; /* 消费掉本次帧 */
        }
        else if ((now - s_last_send_tick) >=
                 adx_tick_from_ms(s_current_timeout_ms))
        {
            s_at_state = ADX_AT_STATE_TIMEOUT;
        }
        /* 否则继续等待下一帧 */
        break;
    }

    case ADX_AT_STATE_RESP_OK:
    {
        s_recv_buffer_reset();
        s_current_rx_cb = NULL;
        s_current_timeout_cb = NULL;
        s_at_state = ADX_AT_STATE_IDLE;
        break;
    }

    case ADX_AT_STATE_TIMEOUT:
    {
        if (s_current_timeout_cb != NULL)
        {
            s_current_timeout_cb(s_current_cmd);
        }
        s_recv_buffer_reset();
        s_current_rx_cb = NULL;
        s_current_timeout_cb = NULL;
        s_at_state = ADX_AT_STATE_IDLE;
        break;
    }

    default:
        s_at_state = ADX_AT_STATE_IDLE;
        break;
    }
}
