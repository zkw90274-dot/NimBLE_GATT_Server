# 构建、烧录与踩过的坑

> 上级索引：[../CLAUDE.md](../CLAUDE.md)

## 1. 环境

| 项 | 值 |
|---|---|
| ESP-IDF | v5.4.3 @ `E:\Tools\esp32idf\esp32_5_4\v5.4.3\esp-idf` |
| IDF_TOOLS_PATH | `E:\Tools\esp32idf\esp32_5_4\mytools` |
| Python | `mytools\python_env\idf5.4_py3.14_env`（**注**：VS Code 扩展侧用的是 `idf5.4_py3.11_env`，两套并存） |
| 工具链 | xtensa-esp-elf `esp-14.2.0_20250730` |
| 串口 | COM3 |

`IDF_PATH` / `IDF_TOOLS_PATH` 已存在于用户级环境变量中。

## 2. 构建与烧录

```cmd
idf.py build
idf.py -p COM3 flash
idf.py -p COM3 monitor
```

查芯片与 Flash：

```cmd
python -m esptool --port COM3 flash_id
```

**从 Git Bash 调用时有个坑**：`export.bat` 开头会检查 `MSYSTEM`，一旦存在（Git Bash / MSYS 环境必有）就打印 `This .bat file is for Windows CMD.EXE shell only.` 然后 `goto :eof` 直接退出，**而且返回码是 0**。结果是后面 `idf.py` 找不到命令，报的错完全指不到真正原因。必须先把 `MSYSTEM` / `SHELL` / `TERM` 清掉再 call。

## 3. 分区表与 Flash

板子是 **16 MB Flash**，用的是自定义分区表 `partitions.csv`：

```
nvs,      data, nvs,     0x9000, 24K
phy_init, data, phy,     0xf000, 4K
factory,  app,  factory, 0x10000, 4M
```

`factory` 之后约 11.9 MB 故意留空，将来加 OTA 槽位或文件系统时不必动 app。

`nvs` 与 `factory` 的偏移（0x9000 / 0x10000）与 IDF 默认表一致，所以换表**不影响已有 NVS 内容**（BLE 绑定信息）。

相关配置在 `sdkconfig.defaults`：

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

## 4. 坑（全部实测踩过）

### 4.1 `set-target` 的永久自锁 ⭐ 最容易浪费时间

**现象**：

```
Failed to set target esp32s3: non zero exit code 2
Directory '...\build' doesn't seem to be a CMake build directory. Refusing to automatically delete files in this directory.
```

**根因**（源码级确认）：

- `idf.py` 每次运行都无条件创建 `build/log/`（`tools/idf_py_actions/tools.py:342`，`log_dir_name = 'log'` **硬编码，无开关**）
- `set-target` 硬绑定依赖 `fullclean`（`core_ext.py:606`，**无参数可绕**）
- `fullclean` 遇到"目录存在、非空、但**没有 `CMakeCache.txt`**"时直接抛 `FatalError`（`core_ext.py:188`）→ 退出码 2

**于是**：只要有任何一次 cmake 配置失败，`build/` 就留下"只有 `log/`、没有 CMakeCache.txt"的状态，**此后所有 `set-target` 永久失败**。而且报错信息**一个字都不提真正的原因**。

**解法**：删掉或改名 `build/` 再重来。

```cmd
rmdir /s /q build
idf.py set-target esp32s3
```

项目根有个 `set_target.bat` 封装了安全流程（清 MSYSTEM → 删 build → set-target → **按产物验证**），用法：

```cmd
set_target.bat esp32s3
```

### 4.2 只改 Flash 尺寸不会扩大 app 分区

IDF 默认分区表 `partitions_singleapp.csv` 把 `factory` 写死为 **`1M`**，与 `CONFIG_ESPTOOLPY_FLASHSIZE` 无关。把它改成 16MB 而不管分区表，app 分区仍然是 1 MB，14 MB 依然用不上。**必须配自定义分区表。**

### 4.3 `i2c.master: this port has not been initialized`（无害）

启动日志里的一条 ERROR：

```
E (352) i2c.master: this port has not been initialized, please initialize it first
I (362) i2c_bus: i2c0 bus inited
```

来源：`espressif__i2c_bus/i2c_bus_v2.c:104` 在创建总线前会先调 `i2c_master_get_bus_handle()` 探测"总线是否已存在"；IDF 的 `i2c_master.c:1176` 把"端口尚未被占用"这个**完全正常的情况**用 `ESP_LOGE` 打了出来。属组件的日志级别误用，**不在本工程代码里，无功能影响**。

### 4.4 venv 不匹配警告（无害）

```
WARNING: Python interpreter "e:\...\idf5.4_py3.11_env\Scripts\python.exe" used to start idf.py
         is not from installed venv "E:\...\idf5.4_py3.11_env"
```

`idf.py:123` 用的是**大小写敏感的字符串前缀比较**，而盘符大小写（`e:` vs `E:`）在两边不一致就误报。纯噪音。

## 5. 判定成败的原则

**不要看退出码，看产物和正面证据。** 本项目实测踩到过多次"失败还返回 0"：

| 场景 | 假绿表现 |
|---|---|
| `export.bat` 在 MSYS 下调起 | 直接放弃，**返回 0** |
| `idf.py build` 的 `$LASTEXITCODE` | 包装脚本里可能为 0 |

**判据**：

- 构建 → 看 `build/*.bin` 是否存在、尺寸、以及 ELF 里有没有预期符号
- 烧录 → 看正面证据 `Hash of data verified.` + `Leaving...`
- **不要 grep `Failed|错误` 这类关键字** —— 真实失败可能是别的语言或乱码，会漏判

## 6. 相关脚本

| 脚本 | 用途 |
|---|---|
| `set_target.bat` | 安全切换 target，绕开坑 4.1 |
