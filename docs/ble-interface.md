# BLE 接口 —— GATT 服务与载荷格式

> 上级索引：[../CLAUDE.md](../CLAUDE.md)

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

固定 **12 字节**，3 个小端 IEEE-754 float32，单位**度**：

| 偏移 | 类型 | 含义 | 范围 |
|---|---|---|---|
| 0 | float32 LE | `roll` | -180 .. +180 |
| 4 | float32 LE | `pitch` | -90 .. +90 |
| 8 | float32 LE | `yaw` | -180 .. +180，**相对开机时刻，会漂移** |

> ⚠️ **yaw 不是绝对方位**。BMI270 无磁力计，航向角不可观测，这里的 yaw 是陀螺仪 Z 轴积分。详见 [imu.md](imu.md) §4。

> 历史：这个特征**曾经**是 6×int16 原始计数（acc XYZ + gyr XYZ）。改成欧拉角后，原始计数不再通过 BLE 暴露；如果将来需要，用法是 `imu_read()` 拿 `imu_sample_t`，换算常数在 `imu.h`。

### 订阅与通知

1. 客户端写 CCCD 订阅该特征 → 服务端记录订阅状态并打日志 `imu notifications enabled`
2. 服务端以 **20 Hz**（`CONFIG_IMU_NOTIFY_PERIOD_MS = 50`）推送通知
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

## 4. 连接参数

`main/src/gap.c` 负责广播与连接管理：广播间隔 500–510 ms；连接建立后会向对端发起连接参数更新请求（含 latency 3）。若客户端侧通知出现堆积，先看这里的 `conn_itvl` 与对端的协商结果。

## 5. 代码位置

| 文件 | 内容 |
|---|---|
| `main/src/gatt_svc.c` | 三个服务的定义、特征访问回调、订阅回调、`send_imu_notification()` |
| `main/include/gatt_svc.h` | 对外声明 |
| `main/src/gap.c` | 广播、连接事件、MTU 协商 |
| `main/src/imu.c` | 姿态数据来源（见 [imu.md](imu.md)） |

**改 BLE 接口时注意**：`imu_chr_access()` 的读回调同时服务两种路径 —— 客户端主动 READ，以及 `ble_gatts_notify()` 内部取值的 READ。改动时两条路径都会受影响。
