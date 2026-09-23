# IMU —— BMI270 驱动与姿态解算

> 上级索引：[../CLAUDE.md](../CLAUDE.md)

## 1. 硬件与引脚

引脚定义的**唯一事实来源**是 `E:\xiangmu\ESP-Spot Lite\main\board_pins.h`。本工程把它落成 Kconfig 项（值一一对应，改的时候两边都要动）：

| board_pins.h | 值 | 本工程 Kconfig | 用途 |
|---|---|---|---|
| `PIN_I2C_SCL` | GPIO1 | `CONFIG_IMU_I2C_SCL_GPIO=1` | I2C 时钟 |
| `PIN_I2C_SDA` | GPIO2 | `CONFIG_IMU_I2C_SDA_GPIO=2` | I2C 数据 |
| `I2C_BUS_SPEED_HZ` | `100 * 1000` | `CONFIG_IMU_I2C_FREQ_HZ=100000` | 速率 |
| `PIN_IMU_INT` | GPIO5 | `CONFIG_IMU_INT_GPIO=5` | data-ready 中断 |
| `BMI270_I2C_ADDR` | `0x68` | 组件默认 `BMI270_I2C_ADDRESS` | 器件地址（ADO 接 GND） |

**为什么是 100 kHz**：原理图上 SCL/SDA **没有任何外部上拉电阻**，全靠模块自带；`board_pins.h` 记录了 ESP-Spot Lite 自检时"100 kHz + 内部上拉，稳妥优先"的结论。代码里 `sda_pullup_en` / `scl_pullup_en` 都使能了内部上拉。确认外部上拉存在后才可提到 400 kHz。

I2C 总线实例为 `I2C_NUM_0`，由 `espressif/i2c_bus` 组件创建（**硬件** I2C，不是软 I2C）。本工程只有 BMI270 挂在这条总线上，无总线共享问题。

## 2. 驱动路线

```
espressif/bmi270_sensor  ^0.1.0  (实际 0.1.2)
espressif/i2c_bus        ^1.5.0  (实际 1.5.2)
```

`bmi270_sensor` 内部打包了 Bosch 配置固件的**预编译二进制**，并自行完成 8 KB 上传时序 —— 所以本工程**不需要内嵌配置数组**。

**配置固件用的是 `bmi270_toy_config_file`（TOY 集）**。BMI270 同一时刻只能加载一份配置固件，组件里共提供三份（标准 / CIRCLE / TOY），9 个官方示例中 7 个走 TOY。选 TOY 的理由：够用（六轴数据 + 将来若要做 shake/tap/push 等特征都在这一集里），且与同类板级工程一致。

> ⚠️ 若将来要改用 **CIRCLE 集**（`bmi270_circle_config_file` + `bmi270_circle_*` API，可做 multi_tap / circle_gesture），**TOY 集的特征会全部失效**，因为它们不是同一份固件。两集的 API 前缀也不同，不能混用。

**不要** `#include <driver/i2c.h>` —— `i2c_bus.h` 会按 IDF 版本自己选驱动头，并自带一份 `i2c_config_t` 定义，手动引入旧头会冲突（`conflicting declaration 'typedef struct i2c_config_t i2c_config_t'`）。`driver/gpio.h` 无此问题，可以正常引入。

## 3. 量程、ODR 与换算

配置在 `imu_configure()`（`main/src/imu.c`）：

| 传感器 | ODR | 量程 | 其他 |
|---|---|---|---|
| 加速度计 | 200 Hz | ±4 g | `BMI2_ACC_NORMAL_AVG4`，`BMI2_PERF_OPT_MODE` |
| 陀螺仪 | 200 Hz | ±500 dps | `BMI2_GYR_NORMAL_MODE`，`BMI2_POWER_OPT_MODE` |

对应换算常数定义在 `main/include/imu.h`，**与量程强耦合，改量程必须同步改它们**：

```c
#define IMU_ACC_LSB_PER_G   (8192.0f)   /* 2^15 / 4    */
#define IMU_GYR_LSB_PER_DPS (65.536f)   /* 2^15 / 500  */
```

> `bmi2_set_adv_power_save(BMI2_DISABLE, ...)` 是**必需**的：开着高级省电会停掉采样通路，典型症状是"寄存器读写全部正常，但三轴恒为 0"。

## 4. 姿态解算

### roll / pitch —— 绝对角，来自重力矢量

公式**原样取自** `E:\xiangmu\ESP-Spot Lite\main\test_imu.c` 的 `test_imu_test_attitude()`，不是自己推的：

```c
roll  = atan2f(acc_y,  acc_z)                   * RAD_TO_DEG;
pitch = atan2f(-acc_x, sqrtf(acc_y*acc_y + acc_z*acc_z)) * RAD_TO_DEG;
```

`RAD_TO_DEG = 57.29578f`。静止时加速度模长应 ≈ 1.00 g —— 这是同时验证量程与换算是否正确的判据。

### yaw —— 相对角，会漂移 ⚠️

**BMI270 是 6 轴无磁力计器件，航向角在原理上不可观测**（没有地磁或其它外部参考）。参考工程因此**根本没有输出 yaw**。

本工程按需求补上了 yaw，但它的性质必须清楚：

```
yaw = ∫ (gyr_z - bias) dt        开机时归零，bias 为开机静置标定值
```

- 这是**相对转角**，不是绝对方位。秒级到分钟级可用，会缓慢漂移
- **不要**用在需要绝对方位的场合（罗盘航向、绝对朝向）
- 要绝对 yaw 必须加磁力计（BMM150 / QMC5883 等），那是另一件事

积分步长做了保护：`dt` 为 0（首次调用）或 `> 0.5 s`（长时间停顿，如调试器 halt、任务被阻塞）时**跳过本次积分**，避免把一大段时间一次性乘进去造成跳变。结果回卷到 `-180°..+180°`。

### 零偏标定

`imu_calibrate_gyro_bias()`：开机后取 **64 个样本、每 5 ms 一个（约 0.32 s）** 求平均，得到陀螺仪 Z 轴零速率偏置。

**必要性**：BMI270 数据手册的零速率偏置在 ±1 dps 量级，而 yaw 是积分量，残留偏置会**线性累积** —— 不标定就是约 **60°/分钟** 的漂移。

标定值超过 `IMU_GYRO_BIAS_WARN_DPS = 5.0` 会打 WARN，提示"开机时板子可能在动"。**因此开机后约 0.5 s 内板子必须静止**，否则 yaw 基准是错的。

## 5. 采样与上报架构

```
BMI270 (200 Hz ODR)
   │  INT1 脉冲
   ▼
GPIO5 上升沿 ──► ISR（只做一件事：vTaskNotifyGiveFromISR）
   │
   ▼
IMU 任务 ──► imu_read_attitude()  ← 每个 data-ready 采样一次，yaw 积分跑在 200 Hz
   │
   ├──► ESP_LOGI（每 IMU_LOG_DIVIDER 包一条；打出来的仍是真正发出去的包）
   └──► send_imu_notification()   ← 交给协议栈，实际速率由连接间隔决定
```

**为什么采样和上报要解耦**：传感器出数 200 Hz，BLE 承载不了；但 yaw 是积分量，采样越快积分越紧。所以取样的节拍跟着传感器，radio 的节拍单独控制。

### 5.1 上报速率由谁决定

三层，越往下越硬：

| 层 | 位置 | 当前值 | 作用 |
|---|---|---|---|
| 采样节拍 | `imu_configure()` 的 ODR | 200 Hz | 取数频率，决定 yaw 积分精度 |
| 限速器 | `CONFIG_IMU_NOTIFY_PERIOD_MS` | 10 ms | **只是"最早何时可发"**，不等于实际速率 |
| **连接间隔** | `gap.c` 的 `ble_gap_upd_params` | 请求 15–30 ms | **真正的上限** —— 通知只能在连接事件上发出 |

⚠️ **只调 `CONFIG_IMU_NOTIFY_PERIOD_MS` 而不动连接参数是无效的。** 把限速设到 10 ms（目标 100 Hz）后，实际速率仍被连接间隔压住（实测值见 [host-integration.md](host-integration.md) §8）—— 因为链路跟不上时，协议栈会**反压** `ble_gatts_notify()`，而不是丢弃数据。

**这意味着零丢包，代价是速率上不去时表现为"慢"而不是"空洞"**，对上位机是好事。

连接间隔的最终值由**主机**裁定，设备只能请求；具体代码与 `latency` 为什么必须是 0，见 [ble-interface.md](ble-interface.md) §4。

**中断配置**：`BMI2_DRDY_INT` 映射到 `BMI2_INT1`，电气属性为**推挽 / 高有效 / 非锁存**（非锁存 = 每个样本一个脉冲，而不是需要读状态寄存器去清的电平）。因此 GPIO 侧配的是 `GPIO_INTR_POSEDGE` —— **这两处必须一致，否则收不到中断**。

**ISR 里不做任何事**：不碰 I2C、不打日志、不调任何可能阻塞的 API，只发一个任务通知。

**降级设计**：任务用 `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_IMU_NOTIFY_PERIOD_MS))` 等待，**带超时**。所以中断不可用（GPIO 配错、线路断、`imu_start_data_ready()` 失败）时，采样不会停死，只是掉到 `CONFIG_IMU_NOTIFY_PERIOD_MS` 一次（当前 10 ms）。

## 6. 怎么验证它真的在工作

普通"能出数"不足以说明问题，因为**中断断线时的外部表现和正常时一模一样**（超时兜底照样出数）。所以 `imu.c` 里有一个 ISR 计数器：

```c
uint32_t imu_get_data_ready_count(void);
```

`main.c` 在武装中断后等 1 秒打印一次：

```
I (2173) imu: sampling paced by data-ready, INT1 on GPIO5
I (3183) imu: 203 data-ready interrupts in the first second
```

**203 ≈ 配置的 200 Hz ODR**，即中断确实在触发。以后改中断相关代码，**必须重新看这一行**。

真机实测参考值（2026-09-22，静止平放）：

| 量 | 值 |
|---|---|
| 零偏标定 | 0.041 dps |
| roll | ≈ -1.0°（噪声 ±0.12°） |
| pitch | ≈ -2.8°（噪声 ±0.13°） |
| yaw | ≈ 0.00°（静止 2 s 内无可见漂移） |
| 上报间隔 | 50/60 ms 交替（限速器对齐 5 ms 就绪网格的必然结果） |

## 7. 接口与文件

| 文件 | 职责 |
|---|---|
| `main/include/imu.h` | 对外接口：`imu_init` / `imu_start_data_ready` / `imu_read` / `imu_read_attitude` / `imu_get_latest_attitude` / `imu_get_data_ready_count`，以及 `imu_sample_t`、`imu_attitude_t` 与换算常数 |
| `main/src/imu.c` | I2C 建立、BMI270 上电、量程配置、零偏标定、DRDY 中断、姿态解算 |
| `main/main.c` | `imu_task`：等中断 → 采样 → 限速上报 |

**线程安全**：`imu_read_attitude()` 在 IMU 任务里写缓存，GATT 读回调在 NimBLE 任务里读缓存，12 字节结构体赋值非原子，用 `portMUX_TYPE` 自旋锁保护。`yaw` 积分状态（`s_yaw` / `s_last_us`）只被 IMU 任务碰，无需加锁。

**GATT 读回调返回的是缓存值**而非现场读取 —— 这样"日志里看到的"和"通知发出去的"是同一个数。

## 8. 已知限制

- yaw 非绝对（见 §4），无磁力计无法解决
- 开机后 ~0.5 s 需保持静止，否则零偏标定有偏
- 目前只输出姿态角；若要 shake / tap / push 等手势特征，TOY 集里有对应 API（`bmi270_enable_toy_*`），但**需要重新确认 INT1 映射**（特征中断与 data-ready 共用 INT1，`INT1_MAP_FEAT` 与 `INT1_MAP_DATA` 是不同寄存器）
