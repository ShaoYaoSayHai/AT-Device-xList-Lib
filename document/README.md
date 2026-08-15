# ADX (AT Device X) —— 通用AT模组通信库设计说明

> 作者：Zorian (1551769443@qq.com)
> 整理日期：2026-08-16
>
> 本仓库包含两部分：
>
> **一、原项目代码（参考，已 gitignore）**
> - `task_module_serial_comm.c / .h` —— 原模组串行通信底层任务
> - `task_mobile_monitor.c / .h` —— 原 WIFI/BLE 模组业务监控
>
> **二、ADX 通用库（本仓库核心产出）**
> - `source/adx_config.h` —— 配置中心：心跳周期、临界区策略、缓冲区大小、轮询节奏
> - `source/adx_port.h / .c` —— 硬件/RTOS 抽象层：串口、临界区、互斥锁、心跳（空实现，移植时填充）
> - `source/adx_at_engine.h / .c` —— AT 指令引擎核心：Map 表驱动 + 动态队列 + URC 全局分发
> - `source/adx_at_engine_usage_example.c` —— 使用示例
>
> ADX = **A**T **D**evice **X**（复合型组件），目标是做一个可移植、可复用的 AT 模组通信库。

---

## 一、整体架构概览

本模块采用「**底层通信引擎 + 上层业务监控**」分层设计：

```
+-------------------------------------------------------------+
|                  task_mobile_monitor                        |
|   (业务层：WIFI 状态机 / BLE 数据解析 / RSSI 查询)          |
|       - 状态机驱动：UNKNOWN -> NOT_CONNECTED -> BUSY ...     |
|       - AT 回调函数：解析 +STAINFO: / +WRSSI: / OK 等        |
|       - URC 回调函数：+EVENT:WIFI_GOT_IP / +DATA: 等         |
+--------------------------^----------------------------------+
                           | app_mobile_send_at_command()
                           | urc_register_callback()
+--------------------------|----------------------------------+
|                task_module_serial_comm                      |
|   (通信层：串行 AT 指令收发 + URC 全局轮询)                  |
|       - 接收窗口收集 collect_rx_window_frame()               |
|       - AT 状态机 IDLE->SEND->WAITING->RESP_OK/TIMEOUT      |
|       - URC 回调表 urc_receive_info_callback_map[]          |
+-------------------------------------------------------------+
                           | mobile_wifi_send_buffer()
                           | mobile_wifi_read_frame()
+--------------------------|----------------------------------+
|                bsp_mobile_wifi (硬件抽象)                   |
|        UART + DMA 收发，frame 队列                          |
+-------------------------------------------------------------+
```

**核心设计思路两条线**：

1. **串行 AT 指令窗口**：同一时刻只允许一条 AT 指令在途，通过状态机（IDLE→SEND→WAITING→RESP_OK/TIMEOUT）严格管控，避免指令并发冲突。每条指令携带「成功回调」与「超时回调」，响应到达时由底层引擎回调上层业务函数完成解析。

2. **URC 全局解析**：模组主动上报的 URC（ Unsolicited Result Code，如 `+EVENT:WIFI_GOT_IP`、`+DATA:` 等）通过注册式回调表统一分发，与 AT 指令响应通道独立，可异步处理模组的主动事件。

---

## 二、task_module_serial_comm —— 通信引擎

### 2.1 AT 指令状态机

定义见 `task_module_serial_comm.h:14-21`：

| 状态 | 含义 |
|------|------|
| `AT_COMMAND_STATE_IDLE` | 空闲，等待从发送队列取出下一条指令 |
| `AT_COMMAND_STATE_SEND` | 取到指令，正在通过 UART 发送 |
| `AT_COMMAND_STATE_WAITING` | 已发送，等待模组响应，期间调用回调解析 |
| `AT_COMMAND_STATE_RESP_OK` | 回调返回 0，响应已成功处理 |
| `AT_COMMAND_STATE_TIMEOUT` | 超过 `timeout_ms` 仍未收到有效响应 |

状态流转见 `task_module_serial_comm.c:168-243`，关键点：

- **IDLE**：调用 `app_mobile_receive_at_command_response()` 从消息队列取出一个 `thread_mobile_msg_t`，其中包含 `cmd`、`cmd_len`、`rx_callback`、`timeout_callback`、`timeout_ms`。
- **SEND**：清空接收缓冲区 `recv_info_reset()`，调用 `mobile_wifi_send_buffer()` 发送。
- **WAITING**：每轮循环用当前帧调用 `current_at_command_callback(buffer, len)`：
  - 返回 `0` → 进入 `RESP_OK`；
  - 返回 `-1` → 继续等待，同时检测 `(xTaskGetTickCount() - s_last_command_send_tick) >= timeout_ms` 是否超时。
- **TIMEOUT**：执行 `current_at_command_timeout_callback(cmd)`，复位状态。

> 这种「**带超时的回调轮询**」设计保证了：响应可以在多帧分片到达的情况下被持续尝试解析，只要任一帧命中即成功；超时则统一兜底。

### 2.2 接收窗口（RX Window）聚合

`collect_rx_window_frame()` (`task_module_serial_comm.c:62-133`) 是串口数据的唯一入口，其作用是把 DMA 分帧的零散数据**聚合成一个完整的响应窗口**：

1. 先以 10ms 超时读取第一帧，无数据则直接返回失败；
2. 记录 `last_recv_tick`，进入循环：在 `MODULE_SERIAL_COMM_RX_WINDOW_MS`（默认 320ms）时间窗内持续拼接后续帧；
3. 每收到一帧就更新 `last_recv_tick`，窗口从最后一次收帧开始重新计时；
4. 缓冲区满或窗口超时后返回。

这样上层看到的是「一段时间内模组的完整输出」，便于 `strstr` 匹配和 URC 识别。

### 2.3 URC 全局回调表

```c
static urc_msg_t urc_receive_info_callback_map[16]; // 最多 16 个 URC 注册项
```

- **注册**：`urc_register_callback(name, callback)` (`task_module_serial_comm.c:41-55`) 线性查找空槽写入。
- **分发**：`urc_map_polling(buffer, len)` (`task_module_serial_comm.c:260-277`) 遍历所有已注册项，依次调用回调；任一返回 `0` 即视为命中并停止。

**URC 与 AT 响应的分离原则**：
在主循环中（`task_module_serial_comm.c:152-162`），每一帧聚合数据**先走 URC 全局分发**，再进入 AT 状态机。这意味着即使当前没有在途 AT 指令，模组的主动上报（如 `+EVENT:WIFI_GOT_IP`）也能被及时处理。

---

## 三、task_mobile_monitor —— 业务监控层

### 3.1 WIFI 连接状态机

状态枚举见 `task_mobile_monitor.h:18-26`，流转图：

```
              AT 测试 OK
 UNKNOWN ──────────────► NOT_CONNECTED
    ▲                         │ 发送 AT+WJAP=ssid,pwd
    │                         ▼
    │                      IS_BUSY ────── (重试≥5) ─────► UNKNOWN
    │                         │ URC +EVENT:WIFI_GOT_IP
    │                         ▼
    │       +STAINFO:2      IP_LOST
    │ ◄───────────────────────┘
    │                         +STAINFO:3
    │                         ▼
    │                      CONNECTED ── (定期 STAINFO 巡检)
    │                         │ +STAINFO:4 / ERROR
    │                         ▼
    └───────────────────── FAILED
```

状态查询/设置均通过 `taskENTER_CRITICAL()` 临界区保护，见 `task_mobile_monitor.c:60-74`。

主循环 `task_mobile_monitor_running()` (`task_mobile_monitor.c:225-327`) 按当前状态分派不同动作：

| 状态 | 动作函数 | 说明 |
|------|----------|------|
| `UNKNOWN` | `mobile_at_command_test_run` | 发 `AT\r\n` 探测模组存活 |
| `NOT_CONNECTED` | `mobile_at_wjap_run` | 15s 一次发 `AT+WJAP=ssid,pwd` |
| `IS_BUSY` | `mobile_at_command_busy_run` | 15s 一次发 `AT+STAINFO?` 探测，重试 5 次回 UNKNOWN |
| `IP_LOST` / `FAILED` | `mobile_at_stainfo_check_run` | 5s 一次轮询 STAINFO |
| `CONNECTED` | `mobile_connected_status_check` | 30s 一次巡检 + RSSI 查询 + BCD 校时 |

### 3.2 AT 响应回调函数集

每个 AT 指令在发送时绑定一个 `rx_callback`，回调内部用 `strstr` 匹配关键字符串并推进状态机：

| 回调 | 触发指令 | 匹配关键字 | 动作 |
|------|----------|------------|------|
| `mobile_at_command_test_recv_callback` | `AT` | `OK\r\n` | 继续发 `AT+STAINFO?` 查状态 |
| `mobile_at_status_check` | `AT+STAINFO?` | `+STAINFO:0..4` | 通过 `prvParseStaInfoStatus` 直接写状态机 |
| `mobile_at_wifi_join_ap_callback` | `AT+WJAP=` | （无条件） | 置 `IS_BUSY` |
| `mobile_at_wrssi_callback` | `AT+WRSSI?` | `+WRSSI:` + `OK` | 解析 RSSI 并更新全局值 |
| `mobile_at_command_test_timeout_callback` | 通用超时 | — | 置 `UNKNOWN` |
| `mobile_at_command_wjap_timeout_callback` | WJAP 超时 | — | `retry_count++` |

`prvParseStaInfoStatus` (`task_mobile_monitor.c:407-441`) 是一个轻量解析器：只做一次 `strstr`，然后取 `+STAINFO:` 后第 9 个字符作为状态码，避免了重复扫描。

### 3.3 URC 回调函数集

业务层通过 `urc_register_callback()` 注册的 URC 处理函数：

| URC 回调 | 匹配串 | 作用 |
|----------|--------|------|
| `urc_mobile_wifi_connect_success_callback` | `+EVENT:WIFI_GOT_IP` | 置 `CONNECTED` |
| `urc_mobile_wifi_connect_busy_callback` | `[Busy]Cmd running` | 置 `IS_BUSY` |
| `urc_mobile_ble_connect_success_callback` | `+EVENT:BLE_CONNECT` | 发 `+++` 退出透传 |
| `urc_mobile_ble_data_recv_callback` | `+DATA:` | 提取数据并交给 `ble_controller_recv_command` |

`mobile_ble_extract_data` (`task_mobile_monitor.c:530-567`) 解析 `+DATA:<len>,<payload>` 格式，手工数字解析长度，并做容量与边界校验后 `memcpy` 到全局缓冲区 `g_mobile_ble_data_buffer`。

### 3.4 信号强度（RSSI）保护

RSSI 是跨任务共享资源，使用 FreeRTOS 互斥锁保护：

- `mobile_wrssi_info_init()` 创建互斥锁；
- `mobile_info_mutex_lock/unlock()` 包装 `xSemaphoreTake/Give`，等待 1000ms；
- `mobile_at_wrssi_callback` 写入、`get_mobile_wifi_rssi` 读取均经过锁。

### 3.5 重试与看门狗机制

- `retry_count`（临界区保护）：WJAP 超时累加，连接成功清零；超过 10/20 次回 UNKNOWN 并考虑重启模组。
- `is_busy_retry_count`：BUSY 状态下 STAINFO 探测累计，≥5 回 UNKNOWN。
- 状态变化时同步驱动 LED（蓝灯慢闪/常亮）与蜂鸣器（配网成功音）。

### 3.6 BLE 配网应答

`task_mobile_monitor_running` 主循环末尾调用 `ble_controller_wjap_check_timeout()`，根据当前状态向 BLE 上报配网结果（`BLE_WJAP_RESULT_SUCCESS/FAILED`），对应规约 6.2.10 的 `0x2101H` 异步应答。

---

## 四、关键设计要点总结

1. **分层解耦**：通信引擎只负责「收发 + 状态机 + URC 分发」，不关心业务语义；业务层只负责「状态机 + 解析」，不关心串口细节。两层通过 `app_mobile_send_at_command()` 与回调函数指针耦合。

2. **串行化保证**：AT 指令通过消息队列串行取出，状态机保证任一时刻只有一条指令在途，从根源上避免了模组对并发指令的响应错乱问题。

3. **回调驱动解析**：每条指令自带 `rx_callback` 与 `timeout_callback`，将「发送方」与「解析方」绑定，避免分散的 if-else 判断响应归属。

4. **URC 注册式分发**：URC 回调表是开放式的，新增一种 URC 只需注册一个回调函数，无需修改引擎代码，符合开闭原则。

5. **接收窗口聚合**：320ms 滑动窗口把 DMA 分帧拼成完整响应，兼顾了响应速度与完整性，对 `strstr` 类匹配友好。

6. **临界区 + 互斥锁双层保护**：状态机变量用 `taskENTER_CRITICAL`（轻量、短临界区），RSSI 用互斥锁（长临界区、可阻塞），按场景选用。

7. **超时兜底**：每条 AT 指令都有独立 `timeout_ms`，超时后统一进入 `TIMEOUT` 状态执行回调并复位，避免任务卡死在 WAITING。

---

## 五、可优化建议（仅记录，未实施）

- `urc_map_polling` 目前命中第一个即返回，若多个 URC 同时匹配同一帧会漏掉后续；可考虑全部遍历。
- `mobile_at_command_from_gt_callback` 中 `strstr((char *)buffer, (char *)buffer)` 是恒真表达式，疑似笔误。
- `retry_count_get() >= 10` 后紧接 `else if (retry_count_get() > 20)` 永远不会进入，逻辑可复核。
- URC 回调表大小 16，注释写「最多 12 个」，建议统一。
- `mobile_ble_extract_data` 的长度解析未做上限校验（数字位数过多会溢出 `uint16_t`），可加保护。

---

# 六、ADX 库架构（非阻塞轮询 + Map表驱动 + 动态队列）

> 对应文件：`source/adx_config.h` / `source/adx_port.h/.c` / `source/adx_at_engine.h/.c` / `source/adx_at_engine_usage_example.c`

## 6.1 设计目标

把原项目里「两个任务 + 散装 run 函数 + 硬编码 switch + 阻塞式 while 循环」的监控逻辑，抽象成一个**通用的可移植 AT 模组通信库**，具备：

1. **非阻塞轮询**：不创建任务，引擎核心提供 `adx_chain_reaction_polling()` 和 `adx_heartbeat()` 两个函数，由移植者驱动（裸机放 main 循环，RTOS 放任务里）。
2. **数据驱动调度**：监控逻辑用 Map 表配置，新增指令只需注册条目，不改引擎代码。
3. **真正可移植**：所有硬件/RTOS 依赖抽到 `adx_port` 层，引擎核心零硬件依赖，裸机/FreeRTOS/RT-Thread 通用。
4. **彻底解耦**：所有类型（回调、URC 条目等）在 adx 内部重定义，不依赖任何业务项目头文件。

## 6.2 核心设计：非阻塞轮询 + 心跳

### 6.2.1 两个核心驱动函数

```c
/* 心跳函数：提供时间基准，移植者需周期调用(如每1ms) */
void adx_heartbeat(void);                        /* 返回adx_tick_t，自增计数器 */

/* 链式反应主循环：非阻塞，移植者高频调用 */
void adx_chain_reaction_polling(void);
```

**为什么拆成两个？**
- `adx_heartbeat()` 维护一个 `volatile uint32_t` 计数器，每次调用自增1并返回新值，是引擎所有时间判断的基准。
- `adx_chain_reaction_polling()` 每次调用执行一轮引擎逻辑（收串口→URC→AT状态机），内部通过 `adx_tick_get_now()` 读取当前tick，用「本次值 - 上次值」差值判断时间流逝，**绝不阻塞**。

### 6.2.2 调用顺序（关键！）

任务模式下，调用顺序必须是：**polling → heartbeat → delay**

```c
while (1) {
    adx_chain_reaction_polling();              // 1. 读当前tick，推进引擎
    adx_heartbeat();                           // 2. 自增tick，为下一轮准备
    vTaskDelay(ADX_HEARTBEAT_PERIOD_MS);       // 3. 延时与心跳周期一致
}
```

为什么这个顺序？
- polling 先执行，读取的是上一轮 heartbeat 自增后的值
- heartbeat 后执行，自增 tick 为下一轮 polling 准备
- delay 时间应与 `ADX_HEARTBEAT_PERIOD_MS` 相等或相近

### 6.2.3 裸机 vs RTOS 调用方式对比

```c
/* === 裸机模式(心跳放中断) === */
void SysTick_Handler(void) { adx_heartbeat(); }   /* 1ms中断里调心跳 */

int main(void) {
    adx_at_engine_init();
    while (1) {
        adx_chain_reaction_polling();              /* 高频调轮询 */
        /* 其他业务... */
    }
}

/* === FreeRTOS 模式(心跳放任务里) === */
void adx_task(void *arg) {
    adx_at_engine_init();
    while (1) {
        adx_chain_reaction_polling();              /* 1. 推进引擎 */
        adx_heartbeat();                           /* 2. 心跳自增 */
        vTaskDelay(ADX_HEARTBEAT_PERIOD_MS);       /* 3. 延时1ms */
    }
}

/* === RT-Thread 模式(心跳放线程里) === */
void adx_thread(void *arg) {
    adx_at_engine_init();
    while (1) {
        adx_chain_reaction_polling();
        adx_heartbeat();
        rt_thread_mdelay(ADX_HEARTBEAT_PERIOD_MS);
    }
}
```

**关键**：引擎核心不关心自己跑在任务里还是裸机循环里，移植者负责调用 polling 和 heartbeat。

## 6.3 文件结构与职责

```
source/adx_config.h          配置中心
                      ├─ 心跳周期：ADX_HEARTBEAT_PERIOD_MS (默认1ms)
                      ├─ 临界区策略：ADX_CRITICAL_USE_DISABLE_IRQ / ADX_CRITICAL_USE_NONE
                      ├─ 缓冲区大小：ADX_CMD_BUFFER_SIZE / ADX_RX_WINDOW_BUFFER_SIZE / ...
                      └─ 轮询节奏：ADX_RESP_OK_COOLDOWN_MS / ADX_LOOP_INTERVAL_MS / ...

source/adx_port.h            硬件/RTOS抽象层接口声明
source/adx_port.c            心跳计数器(已实现) + 串口/临界区/互斥锁(空实现)
                      ├─ 心跳时间：adx_heartbeat(已实现,自增+返回) / adx_tick_get_now(已实现,读当前值) / adx_tick_from_ms(已实现)
                      ├─ 串口：uart_init / send / read_frame (空实现，移植时填)
                      ├─ 临界区：enter/exit_critical (按宏切换：关中断版/空实现版)
                      └─ 互斥锁：mutex_create / lock / unlock (空实现，移植时填)

source/adx_at_engine.h       引擎核心头文件(内部重定义所有类型)
                      ├─ 回调类型：adx_at_recv_cb_t / adx_at_timeout_cb_t / adx_urc_cb_t
                      ├─ 状态机：adx_at_state_t (IDLE/SEND/WAITING/RESP_OK/TIMEOUT)
                      ├─ 监控状态：adx_monitor_state_t (UNKNOWN/NOT_CONNECTED/IS_BUSY/...)
                      ├─ Map条目：adx_map_item_t (含 port_name)
                      ├─ 队列条目：adx_queue_item_t
                      └─ URC条目：adx_urc_entry_t

source/adx_at_engine.c       引擎核心实现(只调 adx_port 接口)
                      ├─ 监控Map管理：register / update_callback / scan_pick_oldest
                      ├─ 动态队列：enqueue / dequeue (环形缓冲)
                      ├─ URC表：register / polling
                      ├─ 接收窗口聚合：s_rx_window_step (非阻塞状态机)
                      ├─ AT状态机：adx_chain_reaction_polling 主循环
                      └─ 引擎初始化：adx_at_engine_init
```

**依赖关系（关键）**：
```
adx_at_engine.c ──► adx_at_engine.h ──► adx_config.h
                                     └► adx_port.h ──► adx_port.c (移植层)
                                                            │
                                                            ▼
                                                  裸机 / FreeRTOS / RT-Thread
```
引擎核心**绝不直接** include 任何 RTOS 头文件或业务头文件。

## 6.4 核心数据结构

### 6.4.1 监控Map条目（含 portName）

```c
typedef struct {
    const char *cmd;                        /* AT指令字符串 */
    const char *port_name;                  /* 端口名，区分同一指令在不同上下文的回调 */
    adx_monitor_state_t monitor_state;      /* 仅在此状态触发 */
    uint32_t interval_ms;                   /* 触发间隔 */
    adx_at_recv_cb_t rx_cb;                 /* 响应回调 */
    adx_at_timeout_cb_t timeout_cb;         /* 超时回调 */
    uint32_t timeout_ms;                    /* 指令超时 */
    /* ---- 运行时字段(引擎维护) ---- */
    adx_tick_t last_run_tick;               /* 上次执行tick */
    uint8_t is_used;                        /* 槽位启用 */
} adx_map_item_t;
```

**portName 的核心价值**：
> 同一条 AT 指令（如 `AT+STAINFO?`）在不同模组状态下语义不同，需要绑定不同的解析回调。
> 通过 `port_name` 字段区分，可以在 Map 中注册多条相同 `cmd` 但不同 `port_name` 的条目：

| cmd | port_name | monitor_state | interval_ms | rx_cb |
|-----|-----------|---------------|-------------|-------|
| `AT+STAINFO?\r\n` | `stainfo_busy_probe` | IS_BUSY | 5000 | busy 状态解析(看是否脱离busy) |
| `AT+STAINFO?\r\n` | `stainfo_connected_poll` | CONNECTED | 30000 | 巡检解析(看是否掉线) |
| `AT+STAINFO?\r\n` | `stainfo_iplost_retry` | IP_LOST | 5000 | 重试解析(看是否恢复) |

这样「同一指令、不同上下文、不同回调」的关系在表里一目了然，且支持运行时通过 `adx_at_map_update_callback(port_name, ...)` 替换回调。

### 6.4.2 动态队列条目

```c
typedef struct {
    char cmd[ADX_CMD_BUFFER_SIZE];
    uint16_t cmd_len;
    adx_at_recv_cb_t rx_cb;
    adx_at_timeout_cb_t timeout_cb;
    uint32_t timeout_ms;
} adx_queue_item_t;
```

开发者通过 `adx_at_enqueue()` 随时插入临时指令（HTTP 上传、MQTT 发布、业务 AT 等），优先于 Map 监控指令执行。

## 6.5 调度策略

### 6.5.1 Map扫描：全扫描取「最久未执行且到期」

```
s_map_scan_pick_oldest():
    now = adx_tick_get_now()
    cur_state = adx_at_monitor_state_get()
    for each item in s_monitor_map:
        if not item.is_used: continue
        if item.monitor_state != cur_state: continue   # 状态过滤
        elapsed = now - item.last_run_tick
        if item.last_run_tick == 0: elapsed = MAX       # 从未执行，最优先
        if elapsed < adx_tick_from_ms(item.interval_ms): continue  # 间隔未到期
        if elapsed > max_elapsed:
            max_elapsed = elapsed
            picked = item
    return picked
```

**优点**：
- 公平性：不依赖 Map 中的位置顺序，避免靠前项持续抢占。
- 状态过滤：只扫描当前状态下的条目，未激活状态的指令不参与调度。
- `last_run_tick=0`（从未执行）的条目优先级最高，保证初始化后能快速启动。

### 6.5.2 队列优先 + Map保底

```
IDLE 分支:
    if 队列非空:
        取队列指令执行
    else:
        扫Map找最久未执行到期项执行
```

**保底机制**：每处理完一条队列指令（RESP_OK/TIMEOUT → IDLE），下一轮立刻有机会扫 Map，避免 Map 指令被持续有数据的队列彻底饿死。实际效果是「队列指令穿插执行，Map 指令定期兜底」。

## 6.6 非阻塞轮询主循环

```
adx_chain_reaction_polling() 每次调用执行一轮:

    [0] 节奏控制(时间戳判断，不阻塞)
        if (now - last_poll) < interval: return  # 未到执行时间直接返回
        last_poll = now
        interval = RESP_OK ? ADX_RESP_OK_COOLDOWN_MS : ADX_LOOP_INTERVAL_MS

    [1] 收串口帧 + URC分发  (非阻塞状态机)
        s_rx_window_step()  # 每次推进一步，窗口超时才产出完整帧
        adx_at_urc_polling()

    [2] AT状态机 (每次推进一步)
        IDLE:
            if queue非空:  load queue item -> SEND
            else:          scan map -> SEND (若无到期项则保持IDLE)
        SEND:     -> WAITING
        WAITING:  调 rx_cb
                  返回OK -> RESP_OK
                  超时   -> TIMEOUT (时间戳判断)
        RESP_OK:  清理 -> IDLE
        TIMEOUT:  调 timeout_cb -> IDLE
```

**与原设计的区别**：
- 原设计：`while(1)` 阻塞循环 + `vTaskDelay` 延时，只能在 RTOS 任务里跑
- 新设计：非阻塞函数，每次调用推进一步，裸机/RTOS 通用，内部用时间戳控制节奏

**接收窗口聚合也改为非阻塞状态机**：
- 原设计：`collect_rx_window_frame()` 内部 `while` 循环阻塞读帧
- 新设计：`s_rx_window_step()` 每次调用读一帧，窗口超时才返回完整帧

## 6.7 可移植性设计

### 6.7.1 心跳时间基准

`adx_heartbeat()` 已在 `source/adx_port.c` 中实现，维护 `volatile uint32_t` 计数器：
```c
static volatile adx_tick_t s_heartbeat_tick = 0U;

adx_tick_t adx_heartbeat(void)      { s_heartbeat_tick++; return s_heartbeat_tick; }
adx_tick_t adx_tick_get_now(void)   { return s_heartbeat_tick; }  /* 引擎内部读这个 */
```

**职责分离**：
- `adx_heartbeat()`：移植者调用，自增计数器（任务模式放 polling 后，裸机放中断）
- `adx_tick_get_now()`：引擎内部调用，读取当前值（不自增）

**调用顺序（任务模式）**：
```
polling()  →  读 adx_tick_get_now()，用差值判断时间
heartbeat() →  自增计数器，为下一轮准备
delay()    →  等待 ADX_HEARTBEAT_PERIOD_MS
```

移植者只需保证 heartbeat 调用频率 = `ADX_HEARTBEAT_PERIOD_MS`（默认1ms）。

### 6.7.2 临界区可配置

在 `source/adx_config.h` 中二选一：
```c
#define ADX_CRITICAL_USE_DISABLE_IRQ  /* 关中断实现(默认，安全) */
/* #define ADX_CRITICAL_USE_NONE */   /* 空实现(确认无中断竞争时用) */
```

- 关中断版：移植者在 `source/adx_port.c` 用 `__disable_irq()/__enable_irq()` 实现
- 空实现版：`enter/exit_critical` 为空函数，适合确认中断不碰引擎数据的场景

### 6.7.3 port 层接口清单（精简为 11 个）

| 类别 | 接口 | 实现状态 |
|------|------|----------|
| 心跳 | `adx_heartbeat` | ✅ 已实现(自增+返回值) |
| 时间 | `adx_tick_get_now` | ✅ 已实现(读当前值，不自增) |
| 时间 | `adx_tick_from_ms` | ✅ 已实现(ms转tick) |
| 串口 | `adx_port_uart_init` | ⬜ 空实现，移植时填 |
| 串口 | `adx_port_uart_send` | ⬜ 空实现，移植时填 |
| 串口 | `adx_port_uart_read_frame` | ⬜ 空实现，移植时填 |
| 临界区 | `adx_port_enter_critical` | ⬜ 按宏切换(关中断/空) |
| 临界区 | `adx_port_exit_critical` | ⬜ 按宏切换(关中断/空) |
| 互斥锁 | `adx_port_mutex_create` | ⬜ 空实现，移植时填 |
| 互斥锁 | `adx_port_mutex_lock` | ⬜ 空实现，移植时填 |
| 互斥锁 | `adx_port_mutex_unlock` | ⬜ 空实现，移植时填 |

心跳和时间接口已有默认实现，移植时只需填充串口 + 临界区 + 互斥锁（共8个函数），引擎核心无需任何修改。

## 6.8 架构对比（原项目 vs ADX 库）

| 维度 | 原项目 | ADX 库 |
|------|--------|--------|
| 运行方式 | 阻塞式 while(1) 任务 | 非阻塞轮询函数 |
| 任务数量 | 2个（monitor + serial_comm） | 0个（移植者决定） |
| 时间基准 | xTaskGetTickCount (FreeRTOS) | adx_heartbeat (通用) |
| 裸机支持 | ❌ 不支持 | ✅ 支持 |
| 监控逻辑 | switch(state) + 散装 run 函数 | Map表数据驱动 |
| 新增监控指令 | 改主循环代码 | 注册一条Map项 |
| 同指令不同上下文 | 靠不同run函数隐式区分 | portName显式标识 |
| 临时指令插入 | 无统一入口 | `adx_at_enqueue()` |
| 调度公平性 | 靠switch顺序 | 最久未执行优先 |
| 接收窗口聚合 | 阻塞while循环 | 非阻塞状态机 |
| 硬件依赖 | 直接调 bsp | port 层抽象 |
| 业务耦合 | 依赖 app_mobile_msg.h 等 | 完全独立，内部重定义类型 |
| AT状态机语义 | IDLE→SEND→WAITING→RESP_OK/TIMEOUT | 原样保留 |
| 回调解析机制 | 响应窗口内回调 | 原样保留 |

## 6.9 使用方式速览

### 初始化 + 注册
```c
adx_at_engine_init();                      // 引擎初始化(调port uart_init)

adx_at_urc_register("wifi_got_ip", urc_cb); // 注册URC

adx_at_map_register(&(adx_map_item_t){     // 注册监控项
    .cmd = "AT\r\n",
    .port_name = "at_alive_probe",
    .monitor_state = ADX_MONITOR_STATE_UNKNOWN,
    .interval_ms = 3000,
    .rx_cb = at_test_recv_cb,
    .timeout_cb = at_timeout_cb,
    .timeout_ms = 420,
});
```

### 驱动引擎（关键！）
```c
/* === 任务模式(FreeRTOS/RT-Thread) === */
/* 调用顺序：polling → heartbeat → delay */
while (1) {
    adx_chain_reaction_polling();              /* 1. 读tick，推进引擎 */
    adx_heartbeat();                           /* 2. 自增tick */
    vTaskDelay(ADX_HEARTBEAT_PERIOD_MS);       /* 3. 延时与心跳周期一致 */
}

/* === 裸机模式(心跳放中断) === */
void SysTick_Handler(void) { adx_heartbeat(); }  /* 1ms中断调心跳 */
while (1) {
    adx_chain_reaction_polling();                /* 主循环调轮询 */
}
```

### 运行时插入临时指令
```c
adx_at_enqueue("AT+HTTPGET=...\r\n", http_cb, 5000, http_timeout_cb);
```

### 运行时替换回调
```c
adx_at_map_update_callback("at_alive_probe", new_rx_cb, NULL);
```

### URC回调推进状态机
```c
int urc_wifi_got_ip(const uint8_t *buf, uint16_t len) {
    if (strstr((char*)buf, "+EVENT:WIFI_GOT_IP")) {
        adx_at_monitor_state_set(ADX_MONITOR_STATE_CONNECTED);
        return ADX_OK;
    }
    return ADX_FAIL;
}
```

完整的注册和驱动示例见 `source/adx_at_engine_usage_example.c`，包含裸机/FreeRTOS/RT-Thread 三种模式的 main 循环。

## 6.10 移植步骤（到新项目）

1. 把 `source/adx_config.h` / `source/adx_port.h/.c` / `source/adx_at_engine.h/.c` 四个文件拷贝到目标项目。
2. 修改 `adx_config.h`：调整心跳周期、临界区策略、缓冲区大小、轮询节奏。
3. 实现 `adx_port.c`：填充串口3个函数 + 临界区2个函数 + 互斥锁3个函数（共8个，心跳/时间已默认实现）。`adx_port.c` 顶部有 FreeRTOS 参考注释。
4. 在定时器中断里调用 `adx_heartbeat()`（保证频率 = `ADX_HEARTBEAT_PERIOD_MS`）。
5. 在 main 循环或任务里调用 `adx_at_engine_init()` 初始化，然后循环调用 `adx_chain_reaction_polling()`。
6. 用 `adx_at_map_register()` 注册监控指令，用 `adx_at_enqueue()` 插入临时指令。
7. 引擎核心文件 `adx_at_engine.c` **不需要任何修改**。

## 6.11 开源协议

本项目采用 **Apache License 2.0** 协议开源。

- LICENSE 文件本地保留但**不提交到 git**（已在 `.gitignore` 中忽略）
- 使用者需自行从 [Apache 官网](https://www.apache.org/licenses/LICENSE-2.0) 获取完整协议文本
- 代码头部建议保留版权声明：
  ```
  Copyright 2026 Zorian
  Licensed under the Apache License, Version 2.0
  ```
