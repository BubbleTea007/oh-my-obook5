# OBOOK5 platform port

这里放 CrossMux 的 OBOOK5 平台适配代码。

当前先提供与 CrossMux 无关的 Linux 硬件层：

- `linux/fbdev.*`：`/dev/fb0` 打开、1-bit 到 16/32-bit 转换、`FBIOPAN_DISPLAY`
- `linux/evdev.*`：`/dev/input/event1` 触摸读取和坐标归一化
- `linux/paths.h`：`/user/books`、`/user/reader` 约定
- `smoke/smoke_test.cpp`：不依赖 CrossMux 的显示/触摸冒烟程序

后续在同一个 `port/` 下增加：

- CrossMux `HalDisplay` / `HalGPIO` 的 Linux 实现
- simulator shim 的 MIPS/Linux 构建入口
- 上游源码的 include 覆盖和少量 stub

不要修改 `crossmux/upstream/`。

## 注意

`smoke_test` 会直接占用 `/dev/fb0` 和 `/dev/input/event1`，只能在 Qtopia/workshop
停止或专用测试环境中运行，不要在日常使用界面打开时直接启动。
