# OBOOK5 platform port

这里放 CrossMux 的 OBOOK5 平台适配代码。设备构建只使用
`crossmux/upstream` + `crossmux/port`，`crossmux/simulator` 只作参考。

## 目录

- `linux/fbdev.*`：`/dev/fb0` 打开、1-bit 到 16/32-bit 转换、`FBIOPAN_DISPLAY`
- `linux/evdev.*`：`/dev/input/event1` 触摸读取和坐标归一化
- `linux/openssl/`：musl 目标缺少 OpenSSL 头文件时使用的 SHA-1/SHA-256/MD5
- `linux/paths.h`：`/user/books`、`/user/reader` 约定
- `shims/`：从 simulator 快照的 Arduino/ESP-IDF/FreeRTOS 主机 shim
- `obook5/HalDisplay.cpp`：800x480 1-bit framebuffer → `/dev/fb0`
- `obook5/HalGPIO.cpp`：evdev 触摸 → CrossMux touch API
- `obook5/main.cpp`：setup/loop、存储根 `/user`、信号退出
- `pio/`：PlatformIO 工程；`src/` 由 `prepare.py` 生成
- `smoke/smoke_test.cpp`：不依赖 CrossMux 的显示/触摸冒烟程序

## 构建

```bash
./build_crossmux.sh
```

输出：

```text
build/crossmux                  MIPS32r2 静态 ELF
build/app/crossmux/             workshop app 目录
build/crossmux-app.tar.gz       可直接解压到 /user/workshop/app
```

`build_objects.sh` 只做 HAL 接口编译检查；`build_smoke.sh` 生成硬件冒烟程序。

## 注意

不要在 Qtopia/workshop 正常显示时直接运行冒烟测试或阅读器；它们会直接占用
`/dev/fb0` 和 `/dev/input/event1`。通过 workshop 启动 CrossMux 时，
`workshop.sh` 已经会切到 e-ink mode 0 并暂停 `qpe`。
