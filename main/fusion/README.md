# Fusion AHRS（vendored 第三方库）

来源：**https://github.com/xioTechnologies/Fusion** —— 作者 **Seb Madgwick**（即 Madgwick 滤波器的作者）。

本目录的文件**逐字节取自** `E:\Project\espidf_prj\Fusion\main\`（用户本机的旧项目），
**未做任何修改**，文件名和内容都保持上游原样，以便日后与上游新版本直接 `diff` 升级。

## 只取了这几份

| 文件 | 用途 |
|---|---|
| `FusionAhrs.c/h` | AHRS 主体。**`FusionAhrsUpdateNoMagnetometer()` 正是本项目需要的** —— BMI270 是无磁力计的六轴器件 |
| `FusionBias.c/h` | 陀螺零偏的**静止检测 + 运行时估计**。优于本项目原先"开机一次性 64 样本标定"，因为它能跟踪温漂 |
| `FusionMath.h` | 向量 / 四元数运算；`FusionQuaternionToEuler()` 也在这里（`static inline`） |
| `FusionConvention.h` | 坐标系约定，本项目用 `FusionConventionNwu` |

**故意没取**：

- `FusionCompass.c/h` —— `FusionAhrs.c` 对它**零引用**，且本项目无磁力计
- 伞头 `Fusion.h` —— 它 `#include` 了 `FusionCompass.h`，取进来反而要连带 Compass

## 构建接入

`main/CMakeLists.txt` 里单独 glob 本目录，并把 `fusion` 加进 `INCLUDE_DIRS`
（上游 `.c` 用的是同目录 `#include "FusionAhrs.h"`，所以头文件必须和源文件放在一起，
不能按本项目"头进 include/"的惯例拆开）。

## ⚠️ 许可证 —— 对外分发前必须处理

上游 xioTechnologies/Fusion 采用 **MIT**。但**本机那份拷贝没有 LICENSE 文件**
（`E:\Project\espidf_prj\Fusion\` 下只有源码和中文笔记），源码里仅有 `@author Seb Madgwick` 头。

**若要把本项目对外分发**（包括 `host/` 出的单文件 exe、或公开仓库），应当：

1. 从上游仓库取 MIT 许可证全文，放到本目录（例如 `LICENSE-Fusion.txt`）
2. 保留各文件顶部的 `@author` 署名（目前已原样保留，未删改）

本目录文件头未添加任何本项目自己的版权声明 —— 它们的著作权属于原作者。

## 集成位置

调用点在 `main/src/imu.c`：`imu_read_attitude()`。采样数据流向见 [../../docs/imu.md](../../docs/imu.md)。
