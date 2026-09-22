# NimBLE_GATT_Server

ESP32-S3 上的 BLE GATT 服务端：读取板载 **BMI270 六轴 IMU**，解算姿态角，通过 GATT 特征以 20 Hz 通知客户端。

当前状态：**已在真机验证**（2026-09-22，芯片 ESP32-S3 QFN56 v0.2，MAC `dc:b4:d9:21:6a:fc`）。

---

## ⚠️ 本项目的文档约定

> **改动任何组件后，必须同步更新 `docs/` 下对应的那份文档，并在下方"文档地图"里补上新条目。**
>
> - **新增组件** → 新建 `docs/<组件名>.md`，从本文件链接过去
> - **修改已有组件** → 改对应的那份 `docs/<组件名>.md`
> - 本文件只放**索引、速查表、跨组件的坑**。**不要把实现细节堆在这里** —— 那是 `docs/` 的职责，堆在这里等于毁掉渐进式披露。

## 文档地图

| 文档 | 内容 | 什么时候读 |
|---|---|---|
| [docs/imu.md](docs/imu.md) | BMI270 驱动、姿态解算、中断采样、零偏标定 | 改 IMU 引脚 / 量程 / 算法 / 采样方式时 |
| [docs/ble-interface.md](docs/ble-interface.md) | GATT 服务与特征、UUID、载荷格式、订阅流程 | 对接客户端、改 BLE 接口时 |
| [docs/build-and-flash.md](docs/build-and-flash.md) | 构建、烧录、分区表、踩过的坑 | 构建或烧录出问题时**先看这里** |

## 速查

| 项 | 值 |
|---|---|
| 目标芯片 | ESP32-S3 (QFN56) v0.2，16 MB Flash / 8 MB PSRAM |
| ESP-IDF | v5.4.3 @ `E:\Tools\esp32idf\esp32_5_4\v5.4.3\esp-idf` |
| 串口 | COM3 |
| IMU I2C | SCL=GPIO1 / SDA=GPIO2 / 100 kHz / **内部上拉**（板无外部上拉） |
| IMU 中断 | BMI270 INT1 → GPIO5（data-ready，非锁存，高有效） |
| BLE 上报 | 20 Hz（`CONFIG_IMU_NOTIFY_PERIOD_MS=50`） |
| app 分区 | 4 MB（`partitions.csv`），当前占用约 14% |

```cmd
idf.py build
idf.py -p COM3 flash
```

## 跨组件的坑（按踩到的顺序）

1. **`set-target` 的自锁** —— idf.py 每次运行都会建 `build/log/`。只要有一次 cmake 配置失败，`build/` 就会留下"有内容但没有 `CMakeCache.txt`"的状态，此后**所有** `set-target` 都永久失败，报"不是 CMake build directory"。**解法：删掉或改名 `build/` 再重来。** 详见 [docs/build-and-flash.md](docs/build-and-flash.md)。

2. **只改 `CONFIG_ESPTOOLPY_FLASHSIZE` 不会扩大 app 分区** —— IDF 默认分区表把 `factory` 硬编码成 `1M`，与 Flash 尺寸无关。必须配 `partitions.csv` 才能真正用上多余空间。

3. **`E i2c.master: this port has not been initialized`** 是 `espressif/i2c_bus` 组件把正常的总线探测结果用 ERROR 级别打了出来（`i2c_bus_v2.c:104`），**无害**。

4. **`idf.py` 报 venv 不匹配** 是它用大小写敏感比较路径导致的误报（`e:\` vs `E:\`），**无害**。

## 代码结构

```
main/
├── main.c                    应用入口：IMU 任务、心跳任务、NimBLE 启动
├── include/
│   ├── common.h              公共头（TAG、NimBLE/FreeRTOS/ESP 头汇总）
│   ├── imu.h                 IMU 对外接口：初始化、采样、姿态、中断
│   ├── gatt_svc.h            GATT 对外接口（含 send_imu_notification）
│   ├── gap.h / heart_rate.h / led.h
└── src/
    ├── imu.c                 BMI270 驱动 + 姿态解算（核心，见 docs/imu.md）
    ├── gatt_svc.c            GATT 服务定义与访问回调
    ├── gap.c                 广播与连接管理
    ├── heart_rate_mock.c     心跳服务（**mock 数据**，非真实传感器）
    └── led.c                 WS2812

partitions.csv                自定义分区表（4 MB app）
set_target.bat                换 target 的安全脚本，绕开坑 #1
sdkconfig.defaults            Flash 尺寸、分区表选择等持久化配置
```
