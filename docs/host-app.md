# 上位机（Windows）—— 技术选型、搭建流程与验证手册

> 上级索引：[../CLAUDE.md](../CLAUDE.md)
> 本文面向**上位机开发者**，覆盖：框架选型依据、已完成代码、从零复现步骤、**接下来必须做的验证**、单文件发布、已知坑。
> 协议权威定义仍在 [host-integration.md](host-integration.md)，本文不重复 12 字节布局。

---

## 0. 一句话概括

在固件仓库内 `host/` 目录开发 **.NET 8 WPF** 上位机：连接 `NimBLE_GATT`，订阅姿态特征，显示 **数值 + 实时曲线 + 3D 板体姿态**，支持**无硬件仿真模式**，最终打成 **Windows 单文件免安装 exe**。

---

## 1. 需求边界（已确认）

| 项 | 决定 |
|---|---|
| 平台 | 仅 Windows |
| UI | 数值 + 实时曲线 + 3D 姿态 |
| 交付 | **单文件 exe**（自包含，对方无需装 .NET） |
| 仿真 | **要**（无硬件可演示） |
| 数据率 | 当前固件 20 Hz，但**上位机必须抗住 ≥100 fps**（未来固件会提速），见 §2.5 与 §5.3.2 |
| 暂不做 | CSV 导出、LED 控制、多连接、断线自动重连 |

---

## 2. 技术选型（GitHub 实证，非臆测）

### 2.1 结论

| 优先级 | 栈 | 分发形态 |
|---|---|---|
| **首选** | **C# / .NET 8 WPF + `Windows.Devices.Bluetooth` + ScottPlot + Viewport3D** | `dotnet publish` 单文件自包含 exe |
| 次选 | Python 3.12 + bleak + PySide6 + pyqtgraph | PyInstaller **onedir** zip（不要 onefile） |
| 备选 | Tauri + btleplug | MSI/NSIS，体积小 |
| 不主交付 | Web Bluetooth 网页 | 无法 `file://` 双击；需 HTTPS |

### 2.2 关键参考项目

| 项目 | 为何相关 |
|---|---|
| [Madbol20/BlueMeter-Portable](https://github.com/Madbol20/BlueMeter-Portable) | **.NET 单文件自包含** CI 全流程，Release 文案即「双击、无需安装 .NET」 |
| [Shinar-of-Clark/esp32-3d-attitude-monitor](https://github.com/Shinar-of-Clark/esp32-3d-attitude-monitor) | 便携 `monitor_3d.exe` + **无硬件 Simulation** 的 UX 范本 |
| [uashogeschoolutrecht/MPTT-App](https://github.com/uashogeschoolutrecht/MPTT-App) | PySide6 + bleak 欧拉角 GUI（次选路径成熟度证明） |
| [EdwinFairchild/BLE-PyDex](https://github.com/EdwinFairchild/BLE-PyDex) | bleak 通知/绘图参考 |
| [Mirahelp/BLE-Toolkit](https://github.com/Mirahelp/BLE-Toolkit) | Avalonia/.NET Windows BLE 工具箱 |
| [atomic14/ESP32-LSM6DS3-Demo](https://github.com/atomic14/ESP32-LSM6DS3-Demo) / [jacedeno/firefly-imu-dashboard](https://github.com/jacedeno/firefly-imu-dashboard) | 3D 姿态交互范式 |
| [Serial-Studio/SerialStudio](https://github.com/Serial-Studio/SerialStudio) | **≥100 fps 的采集管线范本**：专用管线线程（绝不碰 GUI 线程）+ 无锁定长环 + 热路径零分配 + 显示刷新用独立定时器 + 丢包计数当金丝雀。§2.5 的形状照此抄 |
| [ScottPlot FAQ: Scatter vs Signal](https://scottplot.net/faq/scatter-vs-signal/) / [Live data](https://scottplot.net/faq/live-data/) | 高码率曲线该用 `Signal`/`SignalXY`（定长数组原地改），不是每帧 `Add.Scatter` 重建 |

### 2.3 为何不选 Python 主路径

- **分发**：PyInstaller `--onefile` 杀软误报 / `_MEI` 临时目录失败（Haylou Windows 工程实测改 onedir）；`--onedir` 又变成 zip 目录，不如单文件 exe 贴合已确认需求。
- **BLE**：bleak 在 Win11 + `pywin32` 存在挂死问题（[bleak#1555](https://github.com/hbldh/bleak/discussions/1555)）。
- 协议仅 12 字节 float×3，不需要 Python 科学计算栈。

### 2.4 架构（SOLID / 高内聚）

```
host/
├── NimBleImuHost.slnx
├── src/NimBleImuHost/          # WPF 宿主
│   ├── Protocol/               # 纯函数解码（可单测，零 UI 依赖）
│   │   ├── ProtocolConstants.cs
│   │   └── AttitudePacket.cs
│   ├── Ble/                    # 数据源抽象 + 采集管线
│   │   ├── IImuSource.cs       # IBleImuSource / IImuSource
│   │   ├── ImuSample.cs        # (Attitude, MonoTicks) —— 驱动边界打时间戳
│   │   ├── AttitudeRingBuffer.cs  # SPSC 无锁环 + 丢帧计数
│   │   ├── SimulatedImuSource.cs  # 可调速率（1–2000 Hz）压测源
│   │   └── WindowsBleImuSource.cs
│   ├── ViewModels/MainViewModel.cs
│   ├── Views/Attitude3DView.*  # 纯 WPF Viewport3D（不引第三方 3D 包）
│   └── MainWindow.*
├── tests/NimBleImuHost.Tests/
├── scripts/publish-portable.ps1
└── NuGet.Config                # 本机包缓存指向 host/.nuget
```

**设计决策**：
1. **协议层纯函数** —— `AttitudePacket.TryDecode` 只依赖 `System.Buffers.Binary`，便于写单测、不臆造协议外字段。
2. **`IImuSource` 抽象** —— UI 不感知 BLE/仿真差异，满足开闭原则。
3. **3D 用原生 Viewport3D** —— 避免 HelixToolkit 等原生库拖累单文件发布。
4. **图表用 ScottPlot.WPF** —— 成熟 WPF 曲线库；高码率下必须走 `SignalXY` 原地更新（见 §2.5），不能每帧重建 plottable。
5. **CCCD 必写** —— 与 `host-integration.md` §4.4 一致，否则静默无数据。

### 2.5 采集 / 渲染流水线（为 ≥100 fps 而做，参考 Serial Studio）

早期版本是「每个样本一次 `Dispatcher.BeginInvoke` + 一次 `plt.Clear()` 重画」。这个形状在 20 Hz 下没问题，
但样本率一上去就同时踩三个坑：dispatcher 队列本身成为瓶颈、每帧重新分配曲线数组、渲染次数与样本数成正比。
现在按 Serial Studio 的采集管线形状拆开：

```
BLE Notify 回调线程 / 仿真线程
        │  new ImuSample(packet, Stopwatch.GetTimestamp())   ← 时间戳在驱动边界打，渲染侧绝不重打
        ▼
MainViewModel.OnSample       只做两件事：Interlocked 计数 + ring.TryWrite()
        │                    零分配、零 marshal、零属性写入
        ▼
AttitudeRingBuffer (SPSC, 16384 槽, 2 的幂 + mask)
        │   满时拒绝最新样本并 DroppedCount++（可测量，不静默弯曲曲线）
        ▼
MainWindow 渲染 tick（DispatcherTimer，请求 8 ms → 实测 ~64 fps）
            ReadNewestInto(2048) → 追加进预分配数组 → SignalXY 原地改数据 → Plot.Refresh()
            数值读数 / 3D / 统计读数都只在这里更新一次
```

要点与取舍：

| 决策 | 做法 | 原因 |
|---|---|---|
| 时间戳 | `ImuSample(Attitude, long MonoTicks)`，`Stopwatch` 刻度 | `DateTimeOffset.UtcNow` 非单调（NTP 会跳），且差值粒度 15 ms 量级；曲线 x 轴用真实到达间隔累加 |
| 队列 | 自写 SPSC 环，无锁 | `System.Threading.Channels` 每样本一个对象分配；环是 24 B 槽位的定长数组 |
| 溢出策略 | 拒绝**最新**样本并计数 | 严格 SPSC 只有消费者能推进读标；`DroppedCount` 是金丝雀——它涨说明要减渲染成本，而不是加缓冲 |
| 落后消费者 | `ReadNewestInto` 取最新 N 个，跳过被覆盖的旧样本 | 旧样本是"已失效的显示数据"，不是丢失的遥测，故不计入丢帧 |
| 曲线 | `Add.SignalXY(xs, ys)` 建一次，之后只改数组内容 + `Data.MinimumIndex/MaximumIndex` | 稳态零分配；不再 `plt.Clear()`/`Add.Scatter`/`RemoveAt(0)` |
| 窗口 | 定长 1200 点，写满时一次性左移 1/4（摊销） | 20 Hz 看 60 s，500 Hz 看 2.4 s；示波器语义 |
| 重画条件 | 仅当本 tick 真的取到新样本 | 20 Hz 数据下渲染就是 20 fps，不做无意义重绘 |
| 统计 | 到达速率 / 渲染 fps / 丢帧 都显示在左侧「流水线」块 | 「抗得住」必须是屏幕上能读到的数字，不是注释里的断言 |

仿真源因此变成压测工具：`SimulatedImuSource.RateHz`（1–2000 Hz，UI 上 20/100/250/500 可选），
用 `Stopwatch` 绝对到期时间 + `Task.Delay` 粗睡 + `Thread.SpinWait` 收尾来定拍——
`Task.Delay` 在 Windows 上的地板约 15 ms，单靠它连 100 Hz 都定不准。

---

## 3. 已完成 vs 未完成（如实）

### 3.1 已落盘

| 文件 | 状态 |
|---|---|
| `Protocol/ProtocolConstants.cs` | ✅ UUID / 设备名 / 12B 契约常量 |
| `Protocol/AttitudePacket.cs` | ✅ LE float32 三元组编解码 |
| `Ble/IImuSource.cs` / `ImuSample.cs` | ✅ 抽象与样本模型（改为 `Stopwatch` 单调刻度） |
| `Ble/AttitudeRingBuffer.cs` | ✅ SPSC 无锁环 + 丢帧计数，单测覆盖顺序/溢出/守恒/吞吐 |
| `Ble/SimulatedImuSource.cs` | ✅ 速率可调 1–2000 Hz，`Stopwatch` 定拍（压测用） |
| `Ble/WindowsBleImuSource.cs` | ✅ 扫描 / 连接 / CCCD / Notify 解码（**仅编译通过，未真机实测**） |
| `ViewModels/MainViewModel.cs` | ✅ 生产侧只写环；读数与统计只在渲染 tick 更新 |
| `Views/Attitude3DView.xaml(.cs)` | ✅ 板体 + 姿态旋转 + 世界轴 |
| `MainWindow.xaml` | ✅ 左侧控制/数值/流水线统计 + 仿真速率选择 + 右侧 3D/曲线 |
| `MainWindow.xaml.cs` | ✅ 渲染 tick 拉取环形缓冲，`SignalXY` 预分配数组原地更新，roll/pitch 左轴 + yaw 右轴 |
| `NimBleImuHost.csproj` | ✅ TFM `net8.0-windows10.0.19041.0`，引用 `ScottPlot.WPF 5.1.59` |
| `tests/AttitudePacketTests.cs` | ✅ 14 个用例全绿（含真机 4 组 hex 向量回放） |
| `tests/AttitudeRingBufferTests.cs` | ✅ 10 个用例全绿（顺序、回绕、溢出计数、生产消费守恒、热路径吞吐） |
| `scripts/capture-ui.ps1` | ✅ 启动 + UIA 点击/选速率 + 窗口截图 + 读数冻结判定，无人值守 UI 验收 |

### 3.2 未完成

| 项 | 问题 | 优先级 |
|---|---|---|
| 真机 BLE | `WindowsBleImuSource` 未对 `dc:b4:d9:21:6a:fc` 联调（扫描 / CCCD / 速率 / 轴向） | P1 |
| 高速率真机链路 | 固件当前 20 Hz；要到 ≥100 fps 还需改 `CONFIG_IMU_NOTIFY_PERIOD_MS`、NimBLE 连接参数、DLE、Win11 `ThroughputOptimized`（需 TFM ≥ 10.0.22000.0）——**尚未动，等硬件在线再谈** | P1 |
| `scripts/publish-portable.ps1` | **尚未创建**，单文件 exe 未产出 | P1 |
| 干净虚拟机 | 未装 .NET 的系统双击验证 | P2 |

> 编译门禁（`dotnet build -c Release` 0 warning 0 error）、24 个单测、仿真模式 UI 与 100/500 Hz 压测已于 2026-09-23 通过，见 §5.3.1 与 §5.3.2。

---

## 4. 从零复现搭建流程

### 4.1 环境前提

| 项 | 值 |
|---|---|
| OS | Windows 10 1703+ / 11（需 BLE） |
| SDK | .NET SDK 8+（本机实测 **10.0.302** 可构建 net8 目标） |
| 工作目录 | `E:\Project\espidf_prj\NimBLE_GATT_Server\host` |
| 设备 | ESP32-S3 已烧录本仓库固件，MAC `dc:b4:d9:21:6a:fc`（勿写死到扫描逻辑） |

### 4.2 ⚠️ 本机 NuGet 前置修复（曾实测踩坑，2026-09-23 复查：本机已不再复现）

> 复查结论：`PROGRAMFILES` / `PROGRAMFILES(X86)` 现在都已存在，`dotnet restore` 直接通过。下面的判定与修复**保留作为排错手册**，另外注意 **Git Bash 里 `export` 不了带括号的变量名**，要用 PowerShell 设置。

**现象**：`dotnet restore` / `dotnet nuget locals` 报 `Value cannot be null. (Parameter 'path1')`，堆栈落在 `NuGetEnvironment.CalculateFolderPath` → `Path.Combine`。

**根因**（源码级）：Windows 上 NuGet 用 `PROGRAMFILES(X86)` / `PROGRAMFILES` 拼机器级配置目录；本机这两个环境变量为空，导致 `Path.Combine(null, "NuGet")`。

**解法**（当前进程或用户级环境变量）：

```powershell
if (-not [Environment]::GetEnvironmentVariable('PROGRAMFILES')) {
  [Environment]::SetEnvironmentVariable('PROGRAMFILES', 'C:\Program Files')
}
if (-not [Environment]::GetEnvironmentVariable('PROGRAMFILES(X86)')) {
  [Environment]::SetEnvironmentVariable('PROGRAMFILES(X86)', 'C:\Program Files (x86)')
}
```

验证应能看到：

```text
http-cache: ...
global-packages: ...
```

### 4.3 创建工程（已执行过的命令）

```powershell
cd E:\Project\espidf_prj\NimBLE_GATT_Server\host
dotnet new sln -n NimBleImuHost
dotnet new wpf  -n NimBleImuHost      -o src/NimBleImuHost      -f net8.0
dotnet new xunit -n NimBleImuHost.Tests -o tests/NimBleImuHost.Tests -f net8.0
dotnet sln add src/NimBleImuHost/NimBleImuHost.csproj tests/NimBleImuHost.Tests/NimBleImuHost.Tests.csproj
dotnet restore
```

本地包缓存见 `host/NuGet.Config`（`globalPackagesFolder` 指向 `host/.nuget/packages`）。

### 4.4 补齐工程配置（已完成，实测 `ScottPlot.WPF 5.1.59` 可还原）

`src/NimBleImuHost/NimBleImuHost.csproj` 目标形态：

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>WinExe</OutputType>
    <!-- 10.0.19041.0 才能引用 Windows.Devices.Bluetooth 投影 -->
    <TargetFramework>net8.0-windows10.0.19041.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <UseWPF>true</UseWPF>
    <RuntimeIdentifiers>win-x64</RuntimeIdentifiers>
    <AssemblyName>NimBleImuHost</AssemblyName>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="ScottPlot.WPF" Version="5.1.59" />
  </ItemGroup>
</Project>
```

> 版本号以 NuGet 实际可还原版本为准，**不要臆造**；若 `ScottPlot.WPF` 包名/命名空间与文档不一致，以包 README 为准改 `MainWindow.xaml` 中的 `clr-namespace`。
>
> 2026-09-23 实测：nuget.org 上 `ScottPlot.WPF` 有 `5.0.51 … 5.0.56`、`5.1.57 … 5.1.59`，本工程锁 `5.1.59`，`MainWindow.xaml` 里的 `clr-namespace:ScottPlot.WPF;assembly=ScottPlot.WPF` 无需改动。
>
> **测试工程 TFM 必须一起抬到 `net8.0-windows10.0.19041.0`** —— 否则 `ProjectReference` 到 WinExe 会报 NU1201（net8.0 与 windows TFM 不兼容）。

测试工程需 ProjectReference 指向 `src/NimBleImuHost`（或把 `Protocol` 抽成独立类库再引用——更干净）。

### 4.5 修 P0 代码缺陷（已完成）

1. **重写 `MainWindow.xaml.cs`**：删掉残缺行，按 ScottPlot 5.1.59 正确 API 画三条曲线。实测要点：
   - `plt.Legend.Alignment`（`Legend.Location` 在 5.1 已标记过时）
   - **yaw 单独放右轴**：`plt.Axes.AddRightAxis()` + `series.Axes.YAxis = ...`。否则 ±180° 的 yaw 会把 ±12° 的 roll/pitch 压成直线
   - x 轴用**真实到达时间差**累加（`host-integration.md` §4.2 要求），不是固定 0.05
   - 600 点环形缓冲，超出后 `RemoveAt(0)` 滑窗
2. **编译 WinRT**：`WindowsBleImuSource.cs` 依赖 `net8.0-windows10.0.19041.0`，改 TFM 后已过编译。
3. **事件线程**：`SampleForChart` 已 `Dispatcher.BeginInvoke`，3D/曲线只在 UI 线程刷新，避免跨线程碰 WPF。

---

## 5. 接下来步骤与验证清单（按顺序执行）

### 5.1 编译门禁

```powershell
# 每条长命令设置超时；失败看 MSBuild 输出，不要只看退出码（本项目历史有假绿）
cd E:\Project\espidf_prj\NimBLE_GATT_Server\host
dotnet build -c Release 2>&1 | Tee-Object build.log | Select-Object -Last 30
```

**通过判据**：
- `build/`（或 `src/NimBleImuHost/bin/Release/`）出现 `NimBleImuHost.dll` / `.exe`
- 0 error；warning 中不得出现 WinRT / XAML 缺失类型

### 5.2 单元测试门禁（协议层）

建议用例（`AttitudePacketTests`）：
1. **真机向量回放**（取自 `host-integration.md` §2.2）：

```text
A6 20 7F BF 78 F7 26 C0 34 7B 69 42
→ roll ≈ -0.9966  pitch ≈ -2.6089  yaw ≈ 58.3703
```

2. **长度拒绝**：11 / 13 字节必须 `TryDecode == false`
3. **Encode∘Decode 往返**：随机 float 误差 &lt; 1e-6

```powershell
dotnet test -c Release --logger "console;verbosity=normal" 2>&1 | Select-Object -Last 40
```

**通过判据**：全部 Test Passed；失败优先查字节序（必须是 **小端**）。

### 5.3 仿真模式 UI 验收（无硬件）

1. 启动 `dotnet run --project src/NimBleImuHost -c Debug`
2. 左侧选「仿真」→ 点「开始」
3. 检查（2026-09-23 全部通过，见 §5.3.1 / §5.3.2）：
   - [x] roll/pitch 数值约 ±12°/±8° 正弦变化，yaw 缓慢单调
   - [x] 「到达速率」≈ 20 Hz（仿真名义 50 ms）
   - [x] 「流水线」块：「渲染」fps 跟随数据率（无新样本不重画），「丢帧」恒为 0
   - [x] 「仿真速率」下拉 20/100/250/500 Hz 可切，运行中即时生效
   - [x] 3D 绿色板体随姿态转动，橙色「机头」标记方向合理
   - [x] 曲线三条轨迹无卡死、无 UI 冻结
   - [x] 「停止」后数值冻结（保留最后一帧供读数），再「开始」可恢复

### 5.3.1 仿真验收结果（2026-09-23，`scripts/capture-ui.ps1` 无人值守）

脚本会启动 exe、用 UIA 按名字点「开始 / 停止」、把窗口截到 `host/artifacts/ui/*.png` 并 dump 左侧数值。

```powershell
cd E:\Project\espidf_prj\NimBLE_GATT_Server\host
powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -Seconds 8 -ThenButtonName stop
```

| 检查项 | 实测 |
|---|---|
| 速率 | 19.1–19.3 Hz（仿真名义 52 ms → 19.2 Hz）✅ |
| roll / pitch | 幅值 ±12° / ±8° 正弦，与 `SimulatedImuSource` 一致 ✅ |
| yaw | 6°/s 线性斜坡，到 ±180° 折返（协议本身就是 ±180 卷绕，曲线呈锯齿属正常）✅ |
| 3D | 板体随 yaw 转动，机头标记方向与数值一致，无轴反 ✅ |
| 曲线 | 三条轨迹连续；600 点环形缓冲在 40 s 后正确滑窗（x 轴 8→41 s）✅ |
| 停止 / 恢复 | 点「停止」后两次读数完全相同（FROZEN），再点「开始」恢复 ✅ |
| 中文渲染 | WPF 侧正常；**ScottPlot 图内文字必须用 ASCII**，见 §6.7 ✅ |

> 本表是流水线改造前（20 Hz 单点验证）的记录，其中「600 点环形缓冲」已被 §2.5 的 1200 点定长数组 + `SignalXY` 取代；
> 20 Hz 下的复测见 §5.3.2 第一行，其余结论（幅值、yaw 折返、3D 轴向、停止/恢复）不变。

### 5.3.2 采集/渲染流水线压测（2026-09-23，无硬件，仿真源）

`capture-ui.ps1` 现在会先用键盘把「仿真速率」下拉框调到目标值，再点「开始」，最后 dump 左侧统计：

```powershell
cd E:\Project\espidf_prj\NimBLE_GATT_Server\host
powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -Seconds 5 -SimRate 500 -Tag sim500 -ThenButtonName stop
```

| 仿真速率 | 到达速率（实测） | 渲染 | 丢帧 | 结论 |
|---|---|---|---|---|
| 20 Hz | 19.4–19.7 Hz | 19.4–19.7 fps | 0 | 只在有新样本时重画，不做无意义帧 ✅ |
| 100 Hz | 99.6–100.1 Hz | 58.9–62.5 fps | 0 | **目标达成**：100 fps 数据全收，UI 满帧 ✅ |
| 500 Hz | 498.5–498.8 Hz | 64.5–64.6 fps | 0 | 5× 余量；曲线窗宽自动缩到 2.4 s（1200 点）✅ |

截图在 `host/artifacts/ui/`（`sim20c-*`、`sim100c-*`、`sim500b-*`）。500 Hz 那张可核对：x 轴只剩 2.4 s、
三条轨迹仍连续、yaw 右轴刻度合理、3D 与数值一致。

配套单测（`AttitudeRingBufferTests`，10 例）锁住的是流水线的语义，不依赖界面：

- 写入/读出顺序、跨边界回绕、空环读 0；
- 环满时**拒绝最新**并 `DroppedCount++`；
- 落后消费者只拿最新 N 个，被覆盖的旧样本**不**算丢帧；
- 生产/消费守恒：`读到的 + 丢帧 == 写入的`（50 万样本，单线程生产者）；
- 热路径吞吐 > 100 万样本/秒（本机实测远高于此），即 UI 线程不会卡在拷贝上。

**仍未证明的**：真机 BLE 侧的到达速率上限由链路层决定（连接间隔、NimBLE `ble_gap_update_params`、DLE、
Windows 栈），仿真压测只证明**上位机软件不是瓶颈**。见 §3.2「高速率真机链路」。

### 5.4 真机 BLE 验收

前置：固件已运行、串口可看到 `imu: roll ...`、手机/其它客户端已断开（**单连接**）。

1. 界面选「真实 BLE」→「扫描设备」
2. 列表应出现 `NimBLE_GATT`（**不要**依赖 MAC）
3. 「连接所选设备」→ 状态变为「已连接…等待通知」
4. 检查：
   - [ ] 有持续数据，「到达速率」18–20 Hz，「丢帧」恒为 0
   - [ ] 静止时 roll/pitch 稳定（固件实测约 -1° / -2.5° 量级，允许板差）
   - [ ] **yaw 标注「相对上电」**；静止漂移应 ~0.1°/min 量级
   - [ ] 扭动板子，3D 与数值一致、无严重轴反（若轴反：只改 `Attitude3DView.SetAttitude` 映射，**不要改协议**）
   - [ ] 断电重上：yaw 归零行为正确
   - [ ] 拔电池/走出范围：状态变 Faulted，UI 不假死

**排错**：连上无数值 → 查 CCCD；扫描不到 → 是否已被其它客户端占用（host-integration §4.6）。

### 5.5 单文件发布（交付物）

`scripts/publish-portable.ps1` 建议内容：

```powershell
$ErrorActionPreference = 'Stop'
# 修复 NuGet 环境（见 §4.2）
if (-not [Environment]::GetEnvironmentVariable('PROGRAMFILES')) {
  [Environment]::SetEnvironmentVariable('PROGRAMFILES', 'C:\Program Files')
}
if (-not [Environment]::GetEnvironmentVariable('PROGRAMFILES(X86)')) {
  [Environment]::SetEnvironmentVariable('PROGRAMFILES(X86)', 'C:\Program Files (x86)')
}

$proj = Join-Path $PSScriptRoot '..\src\NimBleImuHost\NimBleImuHost.csproj'
$out  = Join-Path $PSScriptRoot '..\publish'

dotnet publish $proj -c Release -r win-x64 --self-contained true `
  /p:PublishSingleFile=true `
  /p:IncludeNativeLibrariesForSelfExtract=true `
  /p:EnableCompressionInSingleFile=true `
  -o $out

Get-Item (Join-Path $out 'NimBleImuHost.exe') | Select-Object FullName, Length
Write-Output "发布完成。对方解压/下载后双击即可（无需安装 .NET）。"
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\publish-portable.ps1
```

**发布判据**：
- `publish/NimBleImuHost.exe` 存在且体积约 **30–80 MB**（自包含正常范围）
- 在**未装 .NET** 的干净 Win10/11 虚拟机双击可启动
- 仿真模式全流程可走通

**发布后必读**：未签名 exe 会触发 SmartScreen / Defender。交付说明写「更多信息 → 仍要运行」；正式渠道需代码签名（非本仓库范围）。

---

## 6. 跨组件的坑（上位机侧）

1. **NuGet `path1` null** —— 本机缺 `PROGRAMFILES`/`PROGRAMFILES(X86)`，见 §4.2。与 idf.py 无关。
2. **TFM 不够高无法用 WinRT BLE** —— 必须 `net8.0-windows10.0.19041.0`（或更高 10.0.x）。
3. **PyInstaller onefile 杀软误报**（若走次选 Python）—— 用 onedir；bleak 避免与 `pywin32` 同包。
4. **Mac 锁定** —— Windows BLE 地址随机化，扫描按名/服务 UUID（协议文档已强调）。
5. **单连接** —— 上位机独占设备；调试时先断开 nRF Connect / 手机 App。
6. **yaw 非绝对** —— UI 必须显式标注，避免被当成罗盘用。
7. **ScottPlot 图内文字不支持中文** —— 图表走 SkiaSharp，没有 WPF 那套 CJK 字体回退，标题里的中文会渲染成方块（`□□□□`）。图的标题/轴标签/图例保持 ASCII；界面中文交给 WPF 控件显示。
8. **无 BOM 的 UTF-8 `.ps1` 会被 Windows PowerShell 5.1 按 ANSI 解码** —— 注释里的中文变成乱码后，尾字节可能吃掉换行，**把下一行代码并进注释**。`capture-ui.ps1` 因此保持纯 ASCII，中文按钮名用码点拼（`[char]0x5F00 + [char]0x59CB` = 开始）。
9. **`GetWindowRect` 含 Win11 隐形边框** —— 截屏顶部会透出别的窗口内容，是截屏方法的瑕疵，不是应用 bug。要精确区域用 `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`。
10. **`DispatcherTimer` 会量化到系统时钟节拍（~15.6 ms）** —— 间隔写 16 ms 实测只有 **40 fps**（每两个节拍走一次），写 8 ms 才拿到 64 fps。别用「16 ms = 60 Hz」的直觉，用界面上的「渲染」读数实测。
11. **WPF `ComboBox` 在 UIA 里 `Current.Name` 是空的** —— 想脚本化选仿真速率，`ExpandCollapse` + 从桌面根找 `ListItem` 也不稳（弹层是独立顶层窗口）。可行做法：`SetFocus()` 后发 `{DOWN}`，闭合的下拉框会直接改选中项；真正的证据去看状态行里的「（500 Hz）」和「到达速率」。
12. **`SignalXY` 要求 X 严格递增** —— 两条样本可能落在同一个 `Stopwatch` 刻度上（尤其高码率），追加时若 `x <= lastX` 用 `double.BitIncrement(lastX)` 顶一格，否则曲线/命中测试行为不可预期。定长数组里未用到的尾部靠 `Data.MinimumIndex/MaximumIndex` 裁掉，别靠 `plt.Clear()`。
13. **仿真源的速率不是免费的** —— `Task.Delay` 地板 ~15.6 ms，只有当剩余时间超过一个节拍才睡，否则自旋等到期。早期版本阈值取 4 ms，结果 100 Hz 目标只跑到 96–99 Hz（睡过头）；改成 20 ms 阈值后实测 99.6–100.1 Hz。代价是 ≥100 Hz 时生成线程会占满一个核，这只影响压测模式，不影响真机。

---

## 7. 建议的代码目录职责（便于后续扩展）

| 扩展 | 落点 | 保持不变 |
|---|---|---|
| CSV 录制 | `Services/RecordingService.cs`（**订阅渲染 tick，不要去碰 `OnSample` 热路径**） | `Protocol/*` |
| LED 写 `0x1815` | `Ble/` 新特征客户端 | 姿态特征 UUID |
| 序列号/时间戳协议扩展 | **新特征 UUID**（勿扩 12B 载荷） | 现有解码容错 |
| 四元数显示 | 服务端加字段后 `Protocol` 加版本化解析 | 3D 视图接口 |
| 真机 ≥100 fps | 固件 `CONFIG_IMU_NOTIFY_PERIOD_MS` + NimBLE 连接参数更新 + DLE；上位机侧把 TFM 抬到 `10.0.22000.0` 后请求 `ThroughputOptimized` | 12B 载荷、`AttitudeRingBuffer`、`SignalXY` 渲染路径 |
| 更长回看窗口 | `MainWindow.DisplayPoints`（当前 1200）与环容量 16384 都是常量 | 采集侧零分配热路径 |

---

## 8. 验收定义（DoD）

- [x] `dotnet build -c Release` 0 error（实测 0 warning / 0 error）  
- [x] `dotnet test` 全绿（24 passed：协议 14 + 环形缓冲 10，含真机 hex 向量）  
- [x] 仿真模式 UI 三件套（数值/曲线/3D）流畅（见 §5.3.1）  
- [x] ≥100 fps 抗压：仿真 100 Hz 与 500 Hz 到达速率达标、丢帧恒为 0、渲染 ~60 fps（见 §5.3.2）  
- [ ] 真机 BLE 流 18–20 Hz，断开可恢复  
- [ ] `publish/NimBleImuHost.exe` 单文件在干净系统双击可运行  
- [x] 本文与 `host-integration.md` 无协议矛盾  

---

## 9. 下一步执行顺序（给下一次会话）

已完成（2026-09-23）：1 修 `MainWindow.xaml.cs` + ScottPlot API、2 csproj TFM + `ScottPlot.WPF 5.1.59`、3 编译门禁、4 协议单测、5 仿真 UI 验收、6 采集/渲染解耦流水线（SPSC 环 + 单调时间戳 + `SignalXY` 原地更新）+ 100/500 Hz 压测。

1. **真机 BLE 验收**（§5.4）—— 板子 MAC `dc:b4:d9:21:6a:fc`，**需要先上电**且无其它客户端占用；重点核对扫描到 `NimBLE_GATT`、CCCD 订阅后 18–20 Hz、轴向与 3D 是否一致，以及界面上「丢帧」是否恒为 0  
2. 写 `scripts/publish-portable.ps1` 并产出单文件（§5.5）  
3. 干净虚拟机双击验证  
4. 真机提速（可选，用户尚未授权动固件）：固件 50 → 100 fps 需要连接参数 + DLE 一起改，见 §7 最后一行  
5. 断线自动重连（本轮按决定**未做**，目前断开只把状态置为 Faulted）
