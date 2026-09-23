# BLE 接口 —— 服务端实现视角

> 上级索引：[../CLAUDE.md](../CLAUDE.md)
> 📌 **报文格式（发送格式）的权威定义在 [host-integration.md](host-integration.md)** —— 那份面向客户端开发者。
> 本文讲**服务端代码怎么写**，不重复协议细节。

设备名：`NimBLE_GATT`（`main/include/common.h` 的 `DEVICE_NAME`）。
服务端角色：广播 → 可连接 → 提供下面三个服务。

## 1. IMU 姿态服务（本项目新增，主要接口）

```
服务 UUID  f0a1b2c3-d4e5-4f60-8a9b-000000000001
特征 UUID  f0a1b2c3-d4e5-4f60-8a9b-000000000002
属性       READ | NOTIFY
```

**这两个是自定义 128 位 UUID，不是 Bluetooth SIG 分配的标准 UUID** —— 有意避开标准 16 位空间，没有占用任何已分配值。客户端需要硬编码它们。

### 载荷格式

👉 **见 [host-integration.md](host-integration.md) §2** —— 那是对外契约的唯一权威定义，此处不再重复，以免两边各改一半。

服务端侧实现要点：

- 序列化在 `gatt_svc.c` 的 `imu_attitude_to_le()` / `put_f32_le()`，把 `imu_attitude_t` 的三个 float 按小端写进 12 字节
- `put_f32_le()` 用 `memcpy` 取 float 的**位模式**，不是数值转换 —— 线上是 IEEE-754，必须原样搬运
- 历史：这个特征**曾经**是 6×int16 原始计数（acc XYZ + gyr XYZ）。改成欧拉角后原始计数不再通过 BLE 暴露；将来若需要，用 `imu_read()` 拿 `imu_sample_t`，换算常数在 `imu.h`

### 订阅与通知

1. 客户端写 CCCD 订阅该特征 → 服务端记录订阅状态并打日志 `imu notifications enabled`
2. 服务端推送通知 —— **实际速率由 BLE 连接间隔决定**（见 §4），不是固件里的常量。限速器与采样逻辑见 [imu.md](imu.md) §5.1，线上实测速率见 [host-integration.md](host-integration.md) §8
3. **未订阅时不推送**，`send_imu_notification()` 直接返回，无副作用

通知的取值来自**缓存**（IMU 任务最后一次解算的结果），不是回调里现场读传感器 —— 这样"串口日志里的值"和"发出去的值"是同一个数，排查时不会出现对不上的情况。

若在第一个样本产生前就被读取，特征返回 `BLE_ATT_ERR_UNLIKELY` 并打一条 `imu characteristic read before the first attitude`（正常启动后 200 ms 内就不会再出现）。

## 2. 心跳服务（**mock，非真实传感器**）

```
服务  0x180D  Heart Rate
特征  0x2A37   Heart Rate Measurement    READ | INDICATE
```

数据来自 `main/src/heart_rate_mock.c`，是 `60 + rand()%21` 的**随机数**，与硬件无关。这是 ESP-IDF 原示例自带的内容，本项目**未改动**。若不需要，删掉服务定义与 `heart_rate_task` 即可（见 `gatt_svc.c` 的服务表）。

## 3. Automation IO 服务（原示例自带）

```
服务  0x1815  Automation IO
特征  00001525-1212-efde-1523-785feabcd123    WRITE
```

写 1 字节：非 0 点亮 WS2812，0 熄灭。LED 引脚见 `CONFIG_BLINK_GPIO`。

## 4. 连接参数 ⭐ 推送速率的上限在这里，不在 IMU 代码里

`main/src/gap.c` 负责广播与连接管理：广播间隔 500–510 ms。

连接建立后设备会**主动请求**一组连接参数，而不是把主机提议的间隔原样回读（原实现在这里把主机的 60 ms 抄进上下界，等于放弃了协商）：

```c
struct ble_gap_upd_params params = {
    .itvl_min = 12,             /* 15 ms —— NimBLE 以 1.25 ms 为单位 */
    .itvl_max = 24,             /* 30 ms */
    .latency = 0,
    .supervision_timeout = 400, /* 4 s */
};
```

| 参数 | 为什么是这个值 |
|---|---|
| `itvl_min` / `itvl_max` | 一个通知**只能在连接事件上发出**，所以连接间隔就是推送速率的硬上限。请求得越短，能推得越快 |
| **`latency = 0`** | `latency` 允许从设备跳过连接事件来省电，会把**有效**间隔拉长到 `(1+latency) × itvl`。原代码是 3，配 60 ms 间隔时最坏 240 ms —— 对持续推流的设备正好相反，**必须为 0** |
| `supervision_timeout = 400` | 4 s。规范要求它 ≥ `(1+latency) × itvl_max × 2`，latency=0 时轻易满足 |

**最终值由主机裁定，设备只能请求。** 实测这台 Windows 在 15–30 ms 的请求区间内给了 22 ms，也见过它给 13.75 ms 或 60 ms。

**排错入口**：串口搜 `conn_itvl=`，该数值单位是 **1.25 ms**。`BLE_GAP_EVENT_CONNECT` 与 `BLE_GAP_EVENT_CONN_UPDATE` 两个事件都会打印。

> 想改推送速率，**先改这里**。只调 `CONFIG_IMU_NOTIFY_PERIOD_MS` 是无效的 —— 链路跟不上时协议栈会反压 `ble_gatts_notify()`，实测见 [host-integration.md](host-integration.md) §8。

## 5. 代码位置

| 文件 | 内容 |
|---|---|
| `main/src/gatt_svc.c` | 三个服务的定义、特征访问回调、订阅回调、`send_imu_notification()` |
| `main/include/gatt_svc.h` | 对外声明 |
| `main/src/gap.c` | 广播、连接事件、MTU 协商 |
| `main/src/imu.c` | 姿态数据来源（见 [imu.md](imu.md)） |

**改 BLE 接口时注意**：`imu_chr_access()` 的读回调同时服务两种路径 —— 客户端主动 READ，以及 `ble_gatts_notify()` 内部取值的 READ。改动时两条路径都会受影响。
