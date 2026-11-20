#!/bin/sh

MODE_FILE=/sys/bus/spi/devices/spi1.0/eink_mode
SAVE_MODE=/tmp/workshop_saved_eink_mode
LOCK_FILE=/tmp/workshop.pid

# Never allow two Workshop instances to own the framebuffer.  A previous
# instance killed with SIGKILL leaves a stale PID file; only a live PID blocks
# startup.
if [ -s "$LOCK_FILE" ]; then
    OTHER=$(cat "$LOCK_FILE" 2>/dev/null)
    if [ -n "$OTHER" ] && kill -0 "$OTHER" 2>/dev/null; then
        echo "workshop already running (pid $OTHER)" >&2
        exit 0
    fi
fi
echo $$ > "$LOCK_FILE"

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

trap 'restore_mode; [ -n "$QPE" ] && kill -CONT $QPE; rm -f "$LOCK_FILE"' EXIT INT TERM

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

# Drop stale app processes left by a killed launcher or an older restart path.
for p in $(ps | /bin/busybox awk '$0 ~ /\/app\/crossmux\/bin\/crossmux$/ {print $1}'); do
    kill -TERM "$p" 2>/dev/null
done
for p in $(ps | /bin/busybox awk '$0 ~ /\/app\/debug\/bin\/debug$/ {print $1}'); do
    kill -TERM "$p" 2>/dev/null
done
sleep 1
for p in $(ps | /bin/busybox awk '$0 ~ /\/app\/crossmux\/bin\/crossmux$/ {print $1}'); do
    kill -KILL "$p" 2>/dev/null
done
for p in $(ps | /bin/busybox awk '$0 ~ /\/app\/debug\/bin\/debug$/ {print $1}'); do
    kill -KILL "$p" 2>/dev/null
done

# Keep the debug shell available while other workshop apps are in the
# foreground.  debugd binds 8098/8099 and daemonizes; if the Debug app still
# owns the ports it waits until that app exits.
DEBUGD=/user/workshop/app/debug/bin/debugd
if [ -x "$DEBUGD" ]; then
    "$DEBUGD" --wait >/dev/null 2>&1 &
fi

/user/workshop/bin/workshop
