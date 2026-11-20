# CrossMux on OBOOK5

这个目录是 **CrossMux 的 OBOOK5 移植层**，不是 CrossMux 源码副本。

- `upstream/`：CrossMux 上游，Git submodule，固定 commit，不直接修改
- `simulator/`：CrossPoint 桌面模拟器，Git submodule，作为 HAL/shim 参考
- `port/`：OBOOK5 平台适配、Linux 后端和构建脚本

目标：

```text
CrossMux UI / 阅读核心 / FreeInkBook
        ↓
FreeInk HAL 接口
        ↓
Linux 通用后端（fbdev / evdev / POSIX）
        ↓
OBOOK5 必要的 e-ink 刷新适配
```

## 原则

- 优先使用 Linux 通用接口：`/dev/fb0`、`/dev/input/event1`、POSIX 文件、pthread
- OBOOK5 专属逻辑单独隔离，不污染通用后端
- 不修改 `upstream/` 源码；通过 include 覆盖、编译开关、HAL 实现和少量 stub 接入
- 上游更新时只更新 submodule，移植改动集中在 `port/`

## 目录

```text
crossmux/
  upstream/                 CrossMux 上游（submodule）
  simulator/                CrossPoint simulator（submodule）
  port/
    linux/                  通用 Linux fbdev / evdev / POSIX 后端
    smoke/                  硬件层冒烟测试
    build/                  构建脚本和中间产物（忽略）
```

## 移植阶段

1. **硬件层验证**：fbdev 显示、evdev 触摸、POSIX 存储
2. **CrossMux native target**：用 simulator 的 Arduino/ESP-IDF/FreeRTOS shim 编译 CrossMux
3. **HAL 接入**：用 Linux 后端替换 simulator 的 SDL 显示/输入
4. **阅读器功能**：书库、EPUB、翻页、目录、字体、进度
5. **可选联网**：OPDS、KOReader、OTA

当前阶段：硬件层验证骨架。

## OBOOK5 实测参数

通过设备 8099 shell 确认：

```text
屏幕物理分辨率   800x480
framebuffer      800x960 virtual, 16bpp, stride 1600
framebuffer 驱动 spi_fb
墨水屏驱动       qingyue426_eink
触摸             fts_ts, /dev/input/event1
DRM              /dev/dri 不存在，无 KMSDRM
rootfs           glibc 2.29
内存             87MB total
```

CrossMux 选 `FREEINK_DEVICE_X4PRO`（800x480、touch、frontlight 能力），
`GfxRenderer` 使用 Portrait 旋转；OBOOK5 专属刷新只放在平台层。

## 当前版本

- `port/linux/fbdev.*`：通用 Linux framebuffer 后端
- `port/linux/evdev.*`：通用 Linux evdev 触摸
- `port/obook5/HalDisplay.cpp`：CrossMux 显示 HAL
- `port/obook5/HalGPIO.cpp`：CrossMux 输入 HAL
- `port/obook5/main.cpp`：setup/loop 入口
- 三个 HAL 文件已用 MIPS 目标编译成对象文件

完整 CrossMux 链接和 UI 运行尚未接入。

## 构建

硬件冒烟测试：

```bash
cd crossmux/port
./build_smoke.sh
```

默认使用 `zig c++` 交叉编译到 `mipsel-linux-musleabi`。可通过
`ZIG=/path/to/zig ./build_smoke.sh` 指定 Zig。

## 上游

```bash
git submodule update --init crossmux/upstream crossmux/simulator
```

上游改动通过更新 submodule 引入；本地兼容层只在 `port/` 中维护。
