#!/bin/sh

MODE_FILE=/sys/bus/spi/devices/spi1.0/eink_mode
SAVE_MODE=/tmp/workshop_saved_eink_mode

# Keep the cached system version in sync with the installed app layer.
if [ -f /opt/image/opt/Qtopia/home/version ]; then
    cp -f /opt/image/opt/Qtopia/home/version /home/version 2>/dev/null
    cp -f /opt/image/opt/Qtopia/home/upgrade.xml /home/upgrade.xml 2>/dev/null
fi

restore_mode() {
    if [ -s "$SAVE_MODE" ]; then
        MODE=$(cat "$SAVE_MODE")
        if [ -x /opt/script/change_refresh_mode.sh ]; then
            /opt/script/change_refresh_mode.sh "$MODE" >/dev/null 2>&1
        else
            echo "$MODE" > "$MODE_FILE" 2>/dev/null
        fi
        rm -f "$SAVE_MODE"
    fi
}

QPE=$(pidof qpe)
[ -z "$QPE" ] && QPE=$(ps | /bin/busybox awk '/\[qpe\]/{print $1; exit}')

trap 'restore_mode; [ -n "$QPE" ] && kill -CONT $QPE' EXIT INT TERM

# Workshop is a direct-framebuffer 1-bit app; leave Qtopia's clarity/gray4
# mode before taking over the panel, then restore it after workshop exits.
if [ -r "$MODE_FILE" ]; then
    cat "$MODE_FILE" > "$SAVE_MODE"
    if [ -x /opt/script/change_refresh_mode.sh ]; then
        /opt/script/change_refresh_mode.sh 0 >/dev/null 2>&1
    else
        echo 0 > "$MODE_FILE"
    fi
    sleep 1
    MODE=$(cat "$MODE_FILE" 2>/dev/null)
    if [ "$MODE" != "0" ]; then
        echo "workshop: cannot leave clarity mode (mode=$MODE)" >&2
        exit 1
    fi
fi

[ -n "$QPE" ] && kill -STOP $QPE

# Expose the OBOOK5 system TTFs to CrossMux through /.fonts.
FONT_SETUP=/user/workshop/bin/setup-fonts.sh
if [ -x "$FONT_SETUP" ]; then
    "$FONT_SETUP" >/dev/null 2>&1
fi

# Keep the debug shell available while other workshop apps are in the
# foreground.  debugd binds 8098/8099 and daemonizes; if the Debug app still
# owns the ports it waits until that app exits.
DEBUGD=/user/workshop/app/debug/bin/debugd
if [ -x "$DEBUGD" ]; then
    "$DEBUGD" --wait >/dev/null 2>&1 &
fi

/user/workshop/bin/workshop
