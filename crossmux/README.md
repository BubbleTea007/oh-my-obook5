# CrossMux on OBOOK5

这个目录是 **CrossMux 的 OBOOK5 移植层**，不是 CrossMux 源码副本。

- `upstream/`：CrossMux 上游，Git submodule，固定 commit，不直接修改
- `simulator/`：CrossPoint 桌面模拟器，Git submodule，仅作参考，不参与设备构建
- `port/`：OBOOK5 平台适配、Linux 后端和构建脚本
  - `port/shims/`：从 simulator 快照并适配的 Arduino/ESP-IDF/FreeRTOS shim
  - `port/obook5/`：OBOOK5 的显示、触摸和 main
  - `port/pio/`：PlatformIO 工程，只编译 upstream + port

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
  simulator/                CrossPoint simulator（submodule，仅参考）
  port/
    linux/                  通用 Linux fbdev / evdev / POSIX 后端
    shims/                  Arduino/ESP-IDF/FreeRTOS host shim 快照
    obook5/                 OBOOK5 显示、触摸、main
    pio/                    PlatformIO 工程（src 为生成目录）
    smoke/                  硬件层冒烟测试
    build/                  构建产物（忽略）
```

## 移植阶段

1. **硬件层验证**：fbdev 显示、evdev 触摸、POSIX 存储
2. **CrossMux native target**：upstream + `port/shims` + `port/obook5` 链接成 MIPS 静态程序
3. **HAL 接入**：Linux fbdev/evdev 后端替换 simulator 的 SDL 显示/输入
4. **阅读器功能**：书库、EPUB、翻页、目录、字体、进度
5. **可选联网**：OPDS、KOReader、OTA

当前阶段：完整二进制度已构建并安装到设备，等待交互验证。

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
- `port/shims/`：不依赖 SDL 的 Arduino/ESP-IDF/FreeRTOS shim
- `port/obook5/HalDisplay.cpp`：CrossMux 显示 HAL
- `port/obook5/HalGPIO.cpp`：CrossMux 输入 HAL
- `port/obook5/main.cpp`：setup/loop 入口
- `port/pio/`：完整链接 upstream 的 PlatformIO 工程

构建产物是 MIPS32r2 静态 ELF，运行时存储根目录为 `/user`。

## 构建

完整 CrossMux：

```bash
cd crossmux/port
./build_crossmux.sh
```

脚本会：

1. 生成 upstream 的 i18n / HTML / 用户手册头文件（都在 upstream 的 gitignore 内）
2. 生成 `port/pio/src` 符号链接树
3. 用 `port/toolchain/bin` 的 `gcc`/`g++` wrapper 调用 Zig
4. 输出 `port/build/crossmux` 和 `port/build/crossmux-app.tar.gz`

硬件冒烟测试仍然可用：

```bash
cd crossmux/port
./build_smoke.sh
```

默认使用 `zig c++` 交叉编译到 `mipsel-linux-musleabi`。可通过
`ZIG=/path/to/zig ./build_crossmux.sh` 指定 Zig。

## 上游

```bash
git submodule update --init crossmux/upstream crossmux/simulator
```

上游改动通过更新 submodule 引入；本地兼容层只在 `port/` 中维护。
