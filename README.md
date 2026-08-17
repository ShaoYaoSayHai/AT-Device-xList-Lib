# ADX (AT Device X) —— 通用 AT 模组通信库

> 版本：0.2 ｜ 更新日期：2026-08-16
> 协议：Apache License 2.0

ADX = **A**T **D**evice **X**，一个**可移植、可复用、非阻塞**的 AT 模组通信库。
适用于裸机 / FreeRTOS / RT-Thread 等各类嵌入式环境，统一管理 AT 指令的收发、超时、URC 上报与监控调度。

---

## 目录

- [一、特性](#一特性)
- [二、仓库结构](#二仓库结构)
- [三、架构概览](#三架构概览)
- [四、核心设计](#四核心设计)
- [五、核心 API](#五核心-api)
- [六、移植指南](#六移植指南)
- [七、使用示例](#七使用示例)
- [八、配置项参考](#八配置项参考)
- [九、开源协议](#九开源协议)

---

## 一、特性

- **非阻塞轮询**：引擎核心不创建任务、不阻塞，由移植者驱动 `adx_chain_reaction_polling()`，裸机/RTOS 通用。
- **Map 表驱动调度**：监控指令以「AT 指令 + 状态过滤 + 触发间隔 + portName + 回调」形式注册，新增指令只改数据不改代码。
- **动态队列**：通过 `adx_at_enqueue()` 随时插入临时指令（HTTP / MQTT / 业务 AT），队列优先于 Map 执行，且 Map 不会被饿死。
- **AT 状态机**：`IDLE → SEND → WAITING → RESP_OK / TIMEOUT`，任一时刻只有一条指令在途，自带超时兜底。
- **URC 全局分发**：模组主动上报通过注册式回调表统一处理，与 AT 响应通道独立。
- **接收窗口聚合**：把 DMA 分帧的零散数据在时间窗内拼成完整响应，对 `strstr` 类匹配友好。
- **可移植抽象层**：所有硬件/RTOS 依赖集中到 `adx_port` 层，引擎核心零硬件依赖。
- **彻底解耦**：所有类型在 adx 内部重定义，不依赖任何业务项目头文件。

---

## 二、仓库结构

```
AT-Device-xList-Lib/
├── source/
│   ├── adx_config.h                   配置中心（心跳/临界区/缓冲区/轮询节奏）
│   ├── adx_port.h                     硬件/RTOS 抽象层接口声明
│   ├── adx_port.c                     抽象层实现（心跳已实现，其余为空实现/桩）
│   ├── adx_at_engine.h                引擎核心头文件（类型 + API）
│   ├── adx_at_engine.c                引擎核心实现（Map/队列/URC/状态机）
│   └── adx_at_engine_usage_example.c  使用示例（裸机/FreeRTOS/RT-Thread）
├── document/
│   └── README.md                      设计文档
├── LICENSE                            Apache License 2.0
├── README.md                          本文件
└── .gitignore
```

> 原项目参考代码（`task_module_serial_comm.*` / `task_mobile_monitor.*`）已在 `.gitignore` 中忽略，仅作设计溯源用，不属于本库。

---

## 三、架构概览

```
+--------------------------------------------------------------------+
|                      业务层（使用者实现）                            |
|   - 注册监控 Map 项：adx_at_map_register()                          |
|   - 注册 URC 回调 ：adx_at_urc_register()                           |
|   - 插入临时指令 ：adx_at_enqueue()                                 |
|   - 在回调中推进状态机：adx_at_monitor_state_set()                   |
+---------------------------------^----------------------------------+
                                  | 回调函数指针
+---------------------------------|----------------------------------+
|              adx_at_engine.c  （引擎核心，零硬件依赖）               |
|     ┌──────────────┬──────────────┬──────────────┬─────────────┐   |
|     │ 监控Map扫描   │ 动态队列      │ URC回调表    │ AT状态机     │   |
|     │ (最久未执行)  │ (环形缓冲)    │ (注册式分发) │ IDLE→...→OK │   |
|     └──────────────┴──────────────┴──────────────┴─────────────┘   |
|                  接收窗口聚合 s_rx_window_step()（非阻塞状态机）      |
+---------------------------------^----------------------------------+
                                  | 只调 adx_port 接口
+---------------------------------|----------------------------------+
|              adx_port.c  （硬件/RTOS 抽象层，移植时填充）            |
|     心跳(已实现) │ 串口 │ 临界区 │ 互斥锁                           |
+---------------------------------^----------------------------------+
                                  |
                       裸机 / FreeRTOS / RT-Thread
```

**依赖关系**：

```
adx_at_engine.c ──► adx_at_engine.h ──► adx_config.h
                                     └► adx_port.h ──► adx_port.c (移植层)
```

引擎核心**绝不直接** include 任何 RTOS 头文件或业务头文件。

---

## 四、核心设计

### 4.1 非阻塞轮询 + 心跳

引擎核心提供两个驱动函数，由移植者调用：

```c
/* 心跳函数：提供时间基准，移植者需周期调用（如每 1ms）*/
adx_tick_t adx_heartbeat(void);

/* 链式反应主循环：非阻塞，移植者高频调用 */
void      adx_chain_reaction_polling(void);
```

- `adx_heartbeat()` 维护一个 `volatile uint32_t` 计数器，每次调用自增 1，是引擎所有时间判断的基准。
- `adx_chain_reaction_polling()` 每次调用执行一轮引擎逻辑（收串口 → URC → AT 状态机），内部通过 `adx_tick_get_now()` 读取当前 tick，用差值判断时间流逝，**绝不阻塞**。

**任务模式调用顺序（关键）**：`polling → heartbeat → delay`

```c
while (1) {
    adx_chain_reaction_polling();              /* 1. 读当前 tick，推进引擎 */
    adx_heartbeat();                           /* 2. 自增 tick，为下一轮准备 */
    vTaskDelay(ADX_HEARTBEAT_PERIOD_MS);       /* 3. 延时与心跳周期一致 */
}
```

- polling 先执行，读取上一轮 heartbeat 自增后的值；
- heartbeat 后执行，自增 tick 为下一轮 polling 准备；
- delay 时间应与 `ADX_HEARTBEAT_PERIOD_MS` 相等或相近。

### 4.2 监控 Map：数据驱动调度

每条监控指令注册为一个 `adx_map_item_t`：

```c
typedef struct {
    const char           *cmd;             /* AT 指令字符串（含\r\n）*/
    const char           *port_name;       /* 端口名，区分同一指令在不同上下文的回调 */
    adx_monitor_state_t   monitor_state;   /* 仅在此状态触发 */
    uint32_t              interval_ms;     /* 触发间隔 */
    adx_at_recv_cb_t      rx_cb;           /* 响应回调 */
    adx_at_timeout_cb_t   timeout_cb;      /* 超时回调 */
    uint32_t              timeout_ms;      /* 指令超时 */
    /* ---- 运行时字段（引擎维护）---- */
    adx_tick_t            last_run_tick;   /* 上次执行 tick */
    uint8_t               is_used;         /* 槽位启用 */
} adx_map_item_t;
```

**扫描算法**：全扫描取「状态匹配 + 已到期 + 最久未执行」的条目触发。

```
s_map_scan_pick_oldest():
    now       = adx_tick_get_now()
    cur_state = adx_at_monitor_state_get()
    for each item in s_monitor_map:
        if not item.is_used:                          continue   # 跳过空槽
        if item.monitor_state != cur_state:           continue   # 状态过滤
        elapsed = now - item.last_run_tick
        if item.last_run_tick == 0: elapsed = MAX                # 从未执行，最优先
        if elapsed < tick_from_ms(item.interval_ms):  continue   # 间隔未到期
        if elapsed > max_elapsed: picked = item                 # 取最久未执行
    return picked
```

优点：

- **公平性**：不依赖 Map 中的位置顺序，避免靠前项持续抢占。
- **状态过滤**：只扫描当前状态下的条目，未激活状态的指令不参与调度。
- **冷启动**：`last_run_tick=0`（从未执行）的条目优先级最高，保证初始化后快速启动。

### 4.3 portName：同一指令、不同上下文、不同回调

同一条 AT 指令（如 `AT+STAINFO?`）在不同模组状态下语义不同，需要绑定不同的解析回调。通过 `port_name` 字段区分：

| cmd               | port_name                | monitor_state | interval_ms | rx_cb                 |
| ----------------- | ------------------------ | ------------- | ----------- | --------------------- |
| `AT+STAINFO?\r\n` | `stainfo_busy_probe`     | IS_BUSY       | 5000        | busy 状态解析（看是否脱离 busy） |
| `AT+STAINFO?\r\n` | `stainfo_connected_poll` | CONNECTED     | 30000       | 巡检解析（看是否掉线）           |
| `AT+STAINFO?\r\n` | `stainfo_iplost_retry`   | IP_LOST       | 5000        | 重试解析（看是否恢复）           |

运行时可通过 `adx_at_map_update_callback(port_name, ...)` 替换回调。

### 4.4 动态队列：临时指令优先 + Map 保底

```
IDLE 分支:
    if 队列非空:  取队列指令执行
    else:         扫 Map 找最久未执行到期项执行
```

- 队列指令（HTTP 上传、MQTT 发布、业务 AT 等）优先于 Map 监控指令执行。
- **保底机制**：每处理完一条队列指令（RESP_OK/TIMEOUT → IDLE），下一轮立刻有机会扫 Map，避免 Map 指令被持续有数据的队列彻底饿死。实际效果是「队列指令穿插执行，Map 指令定期兜底」。

### 4.5 AT 状态机

```
IDLE ──取到指令──► SEND ──发送完成──► WAITING ──rx_cb返回OK──► RESP_OK ──► IDLE
                                     │
                     └── 超时 timeout_ms ──► TIMEOUT ──调 timeout_cb──► IDLE
```

- **WAITING** 期间每轮用当前聚合帧调用 `rx_cb`：返回 `ADX_OK` → `RESP_OK`；返回 `ADX_FAIL` → 继续等待，同时检测超时。
- 这种「带超时的回调轮询」保证响应可在多帧分片到达时被持续尝试解析，任一帧命中即成功；超时则统一兜底。

### 4.6 URC 全局分发

模组主动上报（如 `+EVENT:WIFI_GOT_IP`、`+DATA:` 等）通过注册式回调表统一分发。每一帧聚合数据**先走 URC 全局分发**，再进入 AT 状态机，即使当前没有在途 AT 指令，模组主动事件也能被及时处理。

### 4.7 接收窗口聚合（非阻塞状态机）

把 DMA 分帧的零散数据在 `ADX_RX_WINDOW_MS`（默认 320ms）时间窗内拼成完整响应。采用非阻塞状态机实现（`s_rx_window_step()`），每次 polling 推进一步，窗口超时才产出完整帧，裸机可用。

---

## 五、核心 API

### 引擎生命周期

| API                            | 说明                               |
| ------------------------------ | -------------------------------- |
| `adx_at_engine_init()`         | 引擎初始化（内部调 port 层 UART 初始化，复位状态机） |
| `adx_chain_reaction_polling()` | 链式反应主循环（非阻塞，高频调用）                |

### 监控 Map

| API                                                        | 说明                             |
| ---------------------------------------------------------- | ------------------------------ |
| `adx_at_map_register(item)`                                | 注册一个监控条目                       |
| `adx_at_map_update_callback(port_name, rx_cb, timeout_cb)` | 按 port_name 更新回调（传 NULL 表示不修改） |

### 动态队列

| API                                                  | 说明           |
| ---------------------------------------------------- | ------------ |
| `adx_at_enqueue(cmd, rx_cb, timeout_ms, timeout_cb)` | 插入临时指令（队列优先） |
| `adx_at_queue_count_get()`                           | 查询待处理数量（调试用） |

### 状态机访问

| API                                   | 说明                 |
| ------------------------------------- | ------------------ |
| `adx_at_monitor_state_set(new_state)` | 设置当前模组监控状态（线程安全）   |
| `adx_at_monitor_state_get()`          | 获取当前模组监控状态（线程安全）   |
| `adx_at_engine_state_get()`           | 获取 AT 状态机当前状态（调试用） |

### URC

| API                                   | 说明               |
| ------------------------------------- | ---------------- |
| `adx_at_urc_register(name, callback)` | 注册 URC 回调        |
| `adx_at_urc_polling(buffer, len)`     | URC 全局分发（引擎内部调用） |

### 回调类型

```c
typedef int (*adx_at_recv_cb_t)   (uint8_t *buffer, uint16_t len);  /* 返回 ADX_OK=解析成功 */
typedef int (*adx_at_timeout_cb_t)(char *cmd);                       /* 返回 ADX_OK=已处理   */
typedef int (*adx_urc_cb_t)       (const uint8_t *buffer, uint16_t len);
```

---

## 六、移植指南

### 6.1 port 层接口清单

| 类别  | 接口                         | 实现状态           |
| --- | -------------------------- | -------------- |
| 心跳  | `adx_heartbeat`            | 已实现（自增 + 返回值）  |
| 时间  | `adx_tick_get_now`         | 已实现（读当前值，不自增）  |
| 时间  | `adx_tick_from_ms`         | 已实现（ms 转 tick） |
| 串口  | `adx_port_uart_init`       | 空实现，移植时填       |
| 串口  | `adx_port_uart_send`       | 空实现，移植时填       |
| 串口  | `adx_port_uart_read_frame` | 空实现，移植时填       |
| 临界区 | `adx_port_enter_critical`  | 按宏切换（关中断 / 空）  |
| 临界区 | `adx_port_exit_critical`   | 按宏切换（关中断 / 空）  |
| 互斥锁 | `adx_port_mutex_create`    | 空实现，移植时填       |
| 互斥锁 | `adx_port_mutex_destroy`   | 空实现，移植时填       |
| 互斥锁 | `adx_port_mutex_lock`      | 空实现，移植时填       |
| 互斥锁 | `adx_port_mutex_unlock`    | 空实现，移植时填       |

心跳和时间接口已有默认实现，移植时只需填充**串口 3 个 + 临界区 2 个 + 互斥锁 4 个**（共 9 个函数），引擎核心无需任何修改。`adx_port.c` 顶部有 FreeRTOS 参考注释。

### 6.2 临界区策略

在 `adx_config.h` 中二选一：

```c
#define ADX_CRITICAL_USE_DISABLE_IRQ  /* 关中断实现（默认，安全）*/
/* #define ADX_CRITICAL_USE_NONE */   /* 空实现（确认无中断竞争时用）*/
```

- 关中断版：移植者在 `adx_port.c` 用 `__disable_irq()/__enable_irq()` 或 `taskENTER_CRITICAL()/taskEXIT_CRITICAL()` 实现。
- 空实现版：`enter/exit_critical` 为空函数，适合确认中断不碰引擎数据的场景。

### 6.3 移植步骤

1. 把 `source/` 下的 6 个文件拷贝到目标项目（`adx_config.h` / `adx_port.h/.c` / `adx_at_engine.h/.c`，示例文件可选）。
2. 修改 `adx_config.h`：调整心跳周期、临界区策略、缓冲区大小、轮询节奏。
3. 实现 `adx_port.c`：填充串口 3 个 + 临界区 2 个 + 互斥锁 4 个函数（心跳/时间已默认实现）。
4. 在定时器中断或任务里周期调用 `adx_heartbeat()`，保证频率 = `ADX_HEARTBEAT_PERIOD_MS`。
5. 在 main 循环或任务里调用 `adx_at_engine_init()` 初始化，然后循环调用 `adx_chain_reaction_polling()`。
6. 用 `adx_at_map_register()` 注册监控指令，用 `adx_at_urc_register()` 注册 URC，用 `adx_at_enqueue()` 插入临时指令。
7. 引擎核心文件 `adx_at_engine.c` **不需要任何修改**。

### 6.4 三种运行模式对比

```c
/* === 裸机模式（心跳放中断）=== */
void SysTick_Handler(void) { adx_heartbeat(); }   /* 1ms 中断调心跳 */

int main(void) {
    adx_at_engine_init();
    while (1) {
        adx_chain_reaction_polling();              /* 主循环高频调轮询 */
        /* 其他业务... */
    }
}

/* === FreeRTOS 模式（心跳放任务里）=== */
static void adx_task(void *arg) {
    adx_at_engine_init();
    while (1) {
        adx_chain_reaction_polling();              /* 1. 推进引擎 */
        adx_heartbeat();                           /* 2. 心跳自增 */
        vTaskDelay(pdMS_TO_TICKS(ADX_HEARTBEAT_PERIOD_MS));  /* 3. 延时 */
    }
}

/* === RT-Thread 模式（心跳放线程里）=== */
static void adx_thread(void *arg) {
    adx_at_engine_init();
    while (1) {
        adx_chain_reaction_polling();
        adx_heartbeat();
        rt_thread_mdelay(ADX_HEARTBEAT_PERIOD_MS);
    }
}
```

引擎核心不关心自己跑在任务里还是裸机循环里，移植者负责调用 polling 和 heartbeat。

---

## 七、使用示例

### 7.1 初始化 + 注册

```c
adx_at_engine_init();                       /* 引擎初始化 */

adx_at_urc_register("wifi_got_ip", urc_cb); /* 注册 URC */

adx_at_map_register(&(adx_map_item_t){     /* 注册监控项 */
    .cmd           = "AT\r\n",
    .port_name     = "at_alive_probe",
    .monitor_state = ADX_MONITOR_STATE_UNKNOWN,
    .interval_ms   = 3000,
    .rx_cb         = at_test_recv_cb,
    .timeout_cb    = at_timeout_cb,
    .timeout_ms    = 420,
});
```

### 7.2 驱动引擎

```c
/* 任务模式：调用顺序 polling → heartbeat → delay */
while (1) {
    adx_chain_reaction_polling();
    adx_heartbeat();
    vTaskDelay(ADX_HEARTBEAT_PERIOD_MS);
}

/* 裸机模式：心跳放中断，主循环调轮询 */
void SysTick_Handler(void) { adx_heartbeat(); }
while (1) { adx_chain_reaction_polling(); }
```

### 7.3 运行时插入临时指令

```c
adx_at_enqueue("AT+HTTPGET=http://api.example.com/time\r\n",
               http_cb, 5000, http_timeout_cb);
```

### 7.4 运行时替换回调

```c
adx_at_map_update_callback("at_alive_probe", new_rx_cb, NULL);
```

### 7.5 URC 回调推进状态机

```c
int urc_wifi_got_ip(const uint8_t *buf, uint16_t len) {
    (void)len;
    if (strstr((char *)buf, "+EVENT:WIFI_GOT_IP")) {
        adx_at_monitor_state_set(ADX_MONITOR_STATE_CONNECTED);
        return ADX_OK;
    }
    return ADX_FAIL;
}
```

完整的注册和驱动示例见 `source/adx_at_engine_usage_example.c`，包含裸机 / FreeRTOS / RT-Thread 三种模式的 main 循环。

---

## 八、配置项参考

所有配置项均在 `adx_config.h` 中，均支持外部覆盖（`#ifndef` 保护）。

| 配置项                            | 默认值    | 说明                                        |
| ------------------------------ | ------ | ----------------------------------------- |
| `ADX_HEARTBEAT_PERIOD_MS`      | `1`    | 心跳周期（ms），移植者调用 `adx_heartbeat()` 的频率需与此一致 |
| `ADX_CRITICAL_USE_DISABLE_IRQ` | 启用     | 临界区策略：关中断实现（默认，安全）                        |
| `ADX_CRITICAL_USE_NONE`        | 未启用    | 临界区策略：空实现（确认无中断竞争时用）                      |
| `ADX_CMD_BUFFER_SIZE`          | `256`  | AT 指令发送缓冲区大小（单条指令最大长度，含 `\r\n` 和 `\0`）    |
| `ADX_RX_WINDOW_BUFFER_SIZE`    | `1024` | AT 响应接收窗口缓冲区大小（一次聚合的最大数据量）                |
| `ADX_RX_WINDOW_MS`             | `320`  | 接收窗口时间（ms），无新帧则认为本次响应聚合完成                 |
| `ADX_MAP_SIZE`                 | `32`   | 监控 Map 最大条目数                              |
| `ADX_QUEUE_SIZE`               | `16`   | 动态队列最大条目数                                 |
| `ADX_URC_TABLE_SIZE`           | `16`   | URC 回调表最大条目数                              |
| `ADX_PORT_NAME_LEN`            | `32`   | portName 最大长度（含 `\0`）                     |
| `ADX_URC_NAME_LEN`             | `24`   | URC 注册名称最大长度（含 `\0`）                      |
| `ADX_RESP_OK_COOLDOWN_MS`      | `50`   | RESP_OK 后冷却时间，让模组喘息后再发下一条                 |
| `ADX_LOOP_INTERVAL_MS`         | `5`    | 非冷却状态下的最小轮询间隔（0 = 每次都跑）                   |
| `ADX_UART_READ_TIMEOUT_MS`     | `10`   | 串口读帧默认超时（ms）                              |
| `ADX_MUTEX_WAIT_TIMEOUT_MS`    | `1000` | 互斥锁等待超时（ms）                               |

> 这些参数不是延时，而是时间戳判断。移植者仍需高频调用 `polling()`，引擎内部会自动按这些参数控制实际执行节奏。

---

## 九、开源协议

本项目采用 **Apache License 2.0** 协议开源，完整协议文本见 [LICENSE](LICENSE) 或 [Apache 官网](https://www.apache.org/licenses/LICENSE-2.0)。

代码头部建议保留版权声明：

```
Copyright 2026 Zorian (1551769443@qq.com)
Licensed under the Apache License, Version 2.0
```
