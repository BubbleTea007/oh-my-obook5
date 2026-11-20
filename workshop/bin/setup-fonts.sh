#!/bin/sh
# Expose the OBOOK5 system fonts to CrossMux.
#
# CrossMux scans /.fonts (resolved to /user/.fonts) for .cpfont families and,
# on PSRAM-capable builds, loose .ttf/.otf/.ttc files.  The stock system keeps
# its CJK fonts in /user/.字体 and the Qtopia font directory, so link them in
# without copying multi-megabyte files.

set -u

FONTS=/user/.fonts

# A legacy direct symlink would make the links below land inside /user/.字体.
if [ -L "$FONTS" ]; then
    rm -f "$FONTS"
fi
if [ -e "$FONTS" ] && [ ! -d "$FONTS" ]; then
    exit 0
fi
mkdir -p "$FONTS" || exit 0

link_font() {
    [ -f "$1" ] || return 0
    ln -sf "$1" "$FONTS/$2"
}

QT=/opt/opt/image/opt/Qtopia/lib/fonts

link_font "/user/.字体/LXGWWenKaiGBScreen.ttf" "LXGWWenKai.ttf"
link_font "/user/.字体/LXGWWenKaiMonoGBScreen.ttf" "LXGWWenKaiMono.ttf"
link_font "/user/.字体/Xiaolai-Regular.ttf" "Xiaolai.ttf"

link_font "$QT/NotoSansCJK-Regular.ttf" "NotoSansCJK.ttf"
link_font "$QT/SourceHanSerifCN-Light.ttf" "SourceHanSerifCN.ttf"
link_font "$QT/DroidSansFallback.ttf" "DroidSansFallback.ttf"
link_font "$QT/wqy-zenhei.ttc" "WenQuanYiZenHei.ttc"
link_font "$QT/UKaiCN.ttf" "UKaiCN.ttf"
link_font "$QT/xiaxingkai.ttf" "Xiaxingkai.ttf"
link_font "$QT/AlibabaPuHuiTi-3-45-Light.ttf" "AlibabaPuHuiTi-Light.ttf"
link_font "$QT/AlibabaPuHuiTi-3-75-SemiBold.ttf" "AlibabaPuHuiTi-SemiBold.ttf"
link_font "$QT/DroidSans.ttf" "DroidSans.ttf"
link_font "$QT/DejaVuSans.ttf" "DejaVuSans.ttf"
link_font "$QT/DejaVuSerif.ttf" "DejaVuSerif.ttf"

exit 0
