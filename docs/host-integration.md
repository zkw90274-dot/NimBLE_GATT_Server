# 上位机对接指南 —— BLE 客户端开发

> 上级索引：[../CLAUDE.md](../CLAUDE.md)
> 本文是**面向上位机（PC / 手机端）开发者**的对接文档，**发送格式以本文为唯一权威定义**。
> 服务端内部实现见 [ble-interface.md](ble-interface.md)，引脚与解算算法见 [imu.md](imu.md)。

## 0. 一句话概括

设备 `NimBLE_GATT` 提供一个 BLE 特征，推送姿态角 **roll / pitch / yaw（单位：度）**，每包固定 **12 字节**，实测 **约 70 Hz**（速率由协商出的连接间隔决定，见 §4.2）。订阅即可，无需配对。

---

## 1. 目标标识

| 项 | 值 |
|---|---|
| 设备名 | `NimBLE_GATT` |
| 服务 UUID | `f0a1b2c3-d4e5-4f60-8a9b-000000000001` |
| **特征 UUID** | `f0a1b2c3-d4e5-4f60-8a9b-000000000002` |
| 特征属性 | **READ + NOTIFY**（**没有 WRITE**，上位机不能下发） |
| 配对/加密 | **无**。直接连接，无 PIN、无绑定 |

**扫描建议**：按**设备名**或**服务 UUID** 过滤。**不要硬编码 MAC 地址** —— Windows 上 BLE 地址是随机化的，每次可能不同（实测同一块板子在不同时刻可能枚举出不同地址）。

> ⚠️ 在 Windows"设置 → 蓝牙和其他设备 → 添加设备"里是**连不上的** —— 那是经典蓝牙（BR/EDR）的配对流程，对 BLE GATT 设备无效。必须用 BLE API（如 Python 的 `bleak`、Windows 的 WinRT `BluetoothLEAdvertisementWatcher`、Android 的 `BluetoothGatt`）。

---

## 2. 发送格式（唯一权威定义）

### 2.1 布局

固定 **12 字节**，3 个 **IEEE-754 binary32**，**小端序（little-endian）**，单位**度**。

| 偏移 | 长度 | 类型 | 字段 | 单位 | 范围 | 说明 |
|---|---|---|---|---|---|---|
| 0 | 4 | float32 LE | `roll` | ° | -180 .. +180 | 横滚，绝对角 |
| 4 | 4 | float32 LE | `pitch` | ° | -90 .. +90 | 俯仰，绝对角 |
| 8 | 4 | float32 LE | `yaw` | ° | -180 .. +180 | 偏航，**相对开机时刻，会漂移** |

**没有**校验和、帧序号、时间戳 —— 帧定界完全依赖 BLE 层（一个通知就是一帧）。

### 2.2 真实报文测试向量

以下是从真机抓取的原始字节（2026-09-22 实测），可直接用来验证你的解码实现：

```
READ  raw: 27 BD 45 BF 36 AD 21 C0 FA 7A 69 42
      → roll = -0.7724   pitch = -2.5262   yaw = 58.3701

NOTIFY len=12  FB 85 4E BF CB 23 1C C0 37 7A 69 42
           → roll = -0.8067   pitch = -2.4397   yaw = 58.3694

NOTIFY len=12  A6 20 7F BF 78 F7 26 C0 34 7B 69 42
           → roll = -0.9966   pitch = -2.6089   yaw = 58.3703

NOTIFY len=12  BD FB 3C BF 8C D5 1D C0 EE 7B 69 42
           → roll = -0.7382   pitch = -2.4662   yaw = 58.3710
```

**手工验算示例**（确认字节序没理解错）：`A6 20 7F BF` 按小端拼成 32 位是 `0xBF7F20A6`：

```
符号位 = 1            → 负数
阶码   = 0x7E = 126   → 2^(126-127) = 0.5
尾数   = 0x7F20A6 / 0x800000 = 0.99326
值     = -(1 + 0.99326) × 0.5 = -0.99663     ← 与上表 roll 一致 ✓
```

### 2.3 解码参考实现

**Python**

```python
import struct

roll, pitch, yaw = struct.unpack("<fff", data)   # data 必须是 12 字节
```

**C / C++**

```c
#include <stdint.h>
#include <string.h>

static float f32le(const uint8_t *p) {
    uint32_t bits = (uint32_t)p[0]
                  | ((uint32_t)p[1] << 8)
                  | ((uint32_t)p[2] << 16)
                  | ((uint32_t)p[3] << 24);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/* data 长度必须是 12 */
float roll  = f32le(&data[0]);
float pitch = f32le(&data[4]);
float yaw   = f32le(&data[8]);
```

**C# / Unity**

```csharp
using System;
using System.Buffers.Binary;

float roll  = BinaryPrimitives.ReadSingleLittleEndian(data.AsSpan(0));
float pitch = BinaryPrimitives.ReadSingleLittleEndian(data.AsSpan(4));
float yaw   = BinaryPrimitives.ReadSingleLittleEndian(data.AsSpan(8));
```

> 注意：`BitConverter.ToSingle` 是**机器字节序**相关的。x86/ARM 上虽然也是小端，但用 `BinaryPrimitives.ReadSingleLittleEndian` 更明确、跨平台安全。

---

## 3. 连接流程

```
1. 扫描        找 NimBLE_GATT（或按服务 UUID 过滤）
2. 连接
3. (可选) MTU 协商    默认 MTU 23 就够（23-3=20 ≥ 12），无需特意请求
4. 写 CCCD     订阅特征 UUID ...0002 —— 不订阅则完全收不到数据
5. 接收通知    每包 12 字节；速率取决于协商出的连接间隔（实测 22 ms 时约 70 Hz）
6. 断开 / 重连
```

**关于连接间隔**：设备会在连接建立后**主动请求**一个较短的间隔（15–30 ms），并保持 `latency = 0`。最终值由主机决定 —— 实测这台 Windows 给了 22 ms，也见过它给 13.75 ms 或 60 ms。若你的上位机希望更快，需要在主机侧（而不是设备侧）提出更短间隔的请求。

**CCCD** 是 BLE 自动为 NOTIFY/INDICATE 特征生成的描述符（UUID `00002902-...`）。用 bleak 时 `start_notify()` 会自动写它；用原生 API（Android `setCharacteristicNotification` + 手动写 CCCD）时需要自己处理。

---

## 4. ⚠️ 必读注意事项

### 4.1 `yaw` 不是绝对方位（最重要）

BMI270 是**六轴、无磁力计**器件，航向角在原理上不可观测。这里的 `yaw` 是**陀螺仪 Z 轴角速率的积分**，性质如下：

| 特性 | 说明 |
|---|---|
| 参考点 | **开机时刻归零**。设备重启 → 参考丢失，重新从 0 开始 |
| 漂移 | 实测**约 0.1°/min**（静止、且开机时板子静止的前提下） |
| 用途 | 只能当**相对转角**用（"从上电起转过了多少度"） |
| **不能用于** | 罗盘航向、绝对朝向、任何需要绝对方位的场合 |

开机时的静置零偏标定是漂移率的关键：**设备上电后约 0.5 秒内必须保持静止**，否则基准就偏了（偏 0.05 dps 就是 3°/min）。

若你的应用需要绝对 yaw，**硬件上必须加磁力计**，软件层面无解。

### 4.2 速率由连接间隔决定，不严格等间隔

- **实测 71.6 Hz**（19.98 s 窗口收到 1431 包）。速率**不是固件里的某个固定值**，而是由协商出的 BLE 连接间隔决定的
- 一次连接事件可以承载**多个**通知：22 ms 间隔下实测每事件约 **1.58 包**
- 间隔随协商结果变化 —— 同一台主机在不同时刻可能给 13.75 / 22 / 60 ms，速率随之变化
- 所以：**不要假设固定速率，更不要用固定 dt 做积分或微分**，一律用**接收时间戳**算实际间隔

### 4.3 用的是 NOTIFY，不是 INDICATE

BLE 的 NOTIFY **无确认**。链路质量差时理论上可能**静默丢包**，表现为数值跳变或时间间隔拉长，**不会报错**。要可靠性就得改用 INDICATE —— 那是服务端要改的事，目前不是。

**但实测表现要好得多**：当服务端以高于链路能力的速率调用 `ble_gatts_notify()` 时，本机 NimBLE **反压**（调用被阻塞／排队），而不是把数据丢掉。实测固件尝试 71.5 Hz、主机收到 71.6 Hz —— **零丢包**。

也就是说负载过重时你看到的是**速率下降**，而不是数据空洞。上位机仍建议做跳变检测，但不必按"随时可能丢一半"来设计。

### 4.4 必须先订阅

不写 CCCD 就**完全收不到数据**，而且不会有任何错误提示。这是新手最常见的"连上了但没数据"的原因。

### 4.5 READ 也能取值，但语义不同

特征支持 READ，但返回的是**服务端最近一次解算的缓存值**，不是现场重读传感器。区别：

- **NOTIFY**：常规使用路径，持续数据流
- **READ**：适合"刚连上、先要一个初始值"
- 设备启动后极短时间内（第一个样本产生前）READ 会返回 ATT 错误，上位机应容忍并重试

### 4.6 单连接

服务端同一时刻**只接受一个 BLE 客户端**。第二个连接会失败或导致先前的断开。多上位机同时接需要改服务端。

### 4.7 pitch 的奇异点

`pitch` 由 `atan2` 求得，范围只有 ±90°。**当设备接近垂直（pitch → ±90°）时，roll 会失去意义并剧烈跳变**（欧拉角的万向节死锁）。若上位机做 3D 姿态显示，这一区域需要特殊处理，或改用四元数（服务端目前只输出欧拉角）。

### 4.8 载荷长度要校验

虽然当前固定 12 字节，但**解码前请校验长度**。协议将来若扩展（例如加时间戳或四元数），硬编码偏移会静默读出垃圾值而不是报错。

### 4.9 单位是度，不是弧度

`roll` / `pitch` / `yaw` 全是**度**。做 3D 旋转矩阵时记得转弧度（`rad = deg * π / 180`）。

---

## 5. 设备上的其他服务（避免混淆）

| 服务 | 说明 |
|---|---|
| `0000180D` Heart Rate | ⚠️ **数据是随机数 mock**，与硬件无关，别拿它当真实传感器 |
| `00001815` Automation IO | 特征 `00001525-1212-efde-1523-785feabcd123`，**写 1 字节**控制板载 WS2812：非 0 亮，0 灭 |
| `00001800` / `00001801` | GAP / GATT，标准服务，忽略即可 |

---

## 6. 最小可运行示例（Python）

```python
import asyncio
import struct

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "NimBLE_GATT"
IMU_CHR = "f0a1b2c3-d4e5-4f60-8a9b-000000000002"


def on_notify(_char, data: bytearray) -> None:
    if len(data) != 12:          # 见 4.8
        return
    roll, pitch, yaw = struct.unpack("<fff", data)
    print(f"roll={roll:+7.2f}  pitch={pitch:+7.2f}  yaw={yaw:+7.2f}")


async def main() -> None:
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=20.0)
    if device is None:
        print("device not found")
        return

    async with BleakClient(device, timeout=20.0) as client:
        await client.start_notify(IMU_CHR, on_notify)   # 自动写 CCCD
        await asyncio.sleep(10.0)
        await client.stop_notify(IMU_CHR)


asyncio.run(main())
```

**依赖**：`pip install bleak`。Windows 上建议用 **Python 3.12/3.13** —— bleak 依赖一组 `winrt` 二进制包，过新的 Python（如 3.14）可能还没有对应轮子。

---

## 7. 排错速查

| 症状 | 原因 |
|---|---|
| 连上了但收不到数据 | 没写 CCCD 订阅（见 4.4） |
| 扫描不到设备 | 设备已被别的客户端占用（见 4.6）；或蓝牙未开；或设备没在广播 |
| Windows"添加蓝牙设备"里找不到 | 那是经典蓝牙流程，BLE 用不了（见 §1） |
| READ 返回错误 | 设备刚启动，还没有第一个样本；重试即可（见 4.5） |
| 数值跳变 / 间隔忽长忽短 | NOTIFY 无确认，静默丢包（见 4.3） |
| yaw 自己慢慢增大 | 正常漂移，见 4.1；不是故障 |
| yaw 明显不对 | 开机时板子在动，零偏标定失准 —— 让设备静止后重新上电 |
| MAC 地址每次都变 | Windows BLE 地址随机化，**按名字或服务 UUID 找设备** |
| 3D 模型姿态乱跳 | 接近 pitch = ±90° 的万向节死锁（见 4.7） |

---

## 8. 实测基准数据

供上位机开发时对照（2026-09-23，ESP32-S3 板静止平放）：

| 项 | 实测值 |
|---|---|
| 通知速率 | **71.6 Hz**（19.98 s 窗，1431 包）；短窗 72.0 Hz（6 s 窗，434 包） |
| 丢包 | **0** —— 固件尝试 71.5 Hz、主机收到 71.6 Hz，差值在统计噪声内 |
| 连接间隔 | 22 ms（`conn_itvl=18`，NimBLE 以 1.25 ms 为单位） |
| 每连接事件通知数 | ≈ 1.58 |
| 包长 | 全部 12 字节 |
| roll | ≈ -0.9°，噪声 ±0.5° |
| pitch | ≈ -2.5°，噪声 ±0.4° |
| yaw | 30 s 内变化 0.05°，即 **≈0.1°/min** |
| 从扫描到收到首包 | 约 2–3 s（含扫描窗口） |

> **历史对照**：提速前（限速 50 ms、`gap.c` 未主动协商连接参数）为 **18.2–18.3 Hz**、连接间隔 60 ms、`latency=3`。
> 2026-09-23 改写连接参数请求后提升到上表数值，提升约 **3.9 倍**。
