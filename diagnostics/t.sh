#!/bin/sh
# ============================================================================
#  MAZDA CONNECT ALL-IN-ONE AUTOMATED INSTALLER (FW 74.00.324 ADR)
#  Argon Audited & Clean Baseline (Fixes Crash-Loop & HUD Strobe)
# ============================================================================

hwclock --hctosys 2>/dev/null || true

# --- 1. Remount RW & Vo hieu hoa Hardware Watchdog ---
mount -o rw,remount / 2>/dev/null
mount -o rw,remount /tmp/mnt/data_persist 2>/dev/null
mount -o rw,remount /data_persist 2>/dev/null
rm -f /data_persist/splim /data_persist/nav /data_persist/.splim.tmp /data_persist/.nav.tmp 2>/dev/null
rm -f /tmp/splim /tmp/nav /tmp/splim_over 2>/dev/null
echo 1 > /sys/class/gpio/Watchdog\ Disable/value 2>/dev/null

# --- 2. Thong bao am thanh bat dau ---
dbus-send --address=unix:path=/tmp/dbus_service_socket --type=method_call --dest=com.jci.vbs.navi /com/jci/vbs/navi com.jci.vbs.navi.PlayAudioBeep uint32:1 2>/dev/null || true

# --- 3. Tim thu muc USB thuc te tren CMU ---
USB_DIR=""
for d in /tmp/mnt/sda1 /mnt/sda1 /tmp/mnt/sdb1 /mnt/sdb1 /tmp/mnt/sdc1 /mnt/sdc1; do
    if [ -f "$d/libpatch-blmjcicarplay.so" ]; then
        USB_DIR="$d"
        break
    fi
done

if [ -z "$USB_DIR" ]; then
    SELF_DIR=$(dirname "$(readlink -f "$0")" 2>/dev/null)
    if [ -f "$SELF_DIR/libpatch-blmjcicarplay.so" ]; then
        USB_DIR="$SELF_DIR"
    else
        USB_DIR="/tmp/mnt/sda1"
    fi
fi

# --- 4. Mo khoa Cam ung man hinh (Touchscreen While Driving) ---
/jci/scripts/set_speed_restriction_config.sh disable 2>/dev/null || true
/jci/scripts/set_lvds_speed_restriction_config.sh disable 2>/dev/null || true
COMMON_JS="/jci/gui/common/js/Common.js"
if [ -f "$COMMON_JS" ]; then
    cp -p "$COMMON_JS" "${COMMON_JS}.bak_touch" 2>/dev/null || true
    sed -i 's/"Global.AtSpeed" : this._AtSpeedMsgHandler.bind(this),/\/\/ MZD_TOUCH "Global.AtSpeed" : this._AtSpeedMsgHandler.bind(this),/' "$COMMON_JS" 2>/dev/null || true
fi

# --- 5. Cai dat Thu vien CarPlay HUD & Listener VietMap Live ---
MOD_DIR="/data_persist/cp-hud-mod"
mkdir -p "$MOD_DIR" 2>/dev/null

SO_SRC="$USB_DIR/libpatch-blmjcicarplay.so"
LISTENER_SRC="$USB_DIR/cmu_splim_listener"

if [ -f "$SO_SRC" ]; then
    cp -f "$SO_SRC" "$MOD_DIR/libpatch-blmjcicarplay.so" 2>/dev/null || true
    chmod 0644 "$MOD_DIR/libpatch-blmjcicarplay.so"
fi

if [ -f "$LISTENER_SRC" ]; then
    cp -f "$LISTENER_SRC" "$MOD_DIR/cmu_splim_listener" 2>/dev/null || true
    chmod 0755 "$MOD_DIR/cmu_splim_listener"
    cp -f "$LISTENER_SRC" "$MOD_DIR/splim_udpd" 2>/dev/null || true
    chmod 0755 "$MOD_DIR/splim_udpd"
fi

# Tao script tu dong khoi dong lai daemon (cho D-Bus socket san sang)
cat << 'STARTEOF' > /data_persist/splim_udpd_start.sh
#!/bin/sh
killall cmu_splim_listener splim_udpd 2>/dev/null || true
COUNT=0
while [ ! -S /tmp/dbus_service_socket ] && [ $COUNT -lt 15 ]; do
    sleep 1
    COUNT=$((COUNT + 1))
done
if [ -x /data_persist/cp-hud-mod/cmu_splim_listener ]; then
    nohup /data_persist/cp-hud-mod/cmu_splim_listener >/dev/null 2>&1 &
fi
STARTEOF
chmod 0755 /data_persist/splim_udpd_start.sh

# Khoi dong listener ngay lap tuc
killall -9 cmu_splim_listener splim_udpd 2>/dev/null || true
if [ -x /data_persist/cp-hud-mod/cmu_splim_listener ]; then
    nohup /data_persist/cp-hud-mod/cmu_splim_listener >/dev/null 2>&1 &
fi

# --- 5b. Tao manifest phien ban de collector 5SE doc qua /diag/all ---
BUILD_INFO="$MOD_DIR/build-info.txt"
{
    echo "bundle=Mazda_CX5_HUD_v3.7_FIXED"
    echo "installed_at=$(date)"
    echo "firmware=$(grep -E -m 1 'VERSION|version|FW' /jci/version.ini 2>/dev/null || true)"
    echo "listener_md5=$(md5sum "$MOD_DIR/cmu_splim_listener" 2>/dev/null || true)"
    echo "libpatch_md5=$(md5sum "$MOD_DIR/libpatch-blmjcicarplay.so" 2>/dev/null || true)"
    echo "udp_port=8888"
    echo "diag_port=8889"
    echo "speed_baseline_raw=10000"
    echo "speed_normalization=raw<=10000:unavailable; raw>10000:(raw-10000)/100"
    echo "nav_idle_guard=active_to_inactive_only"
    echo "cmu_log_clock_sync=5se_SYNC_epoch_ms_plus_cmu_monotonic_ms"
    echo "cmu_log_format=seq,cmu_mono_ms,epoch_ms,epoch_source,utc,payload"
    echo "cmu_system_clock_correction=invalid_or_over_6h_delta_only"
    echo "cmu_raw_logs_preserved=yes"
} > "$BUILD_INFO" 2>/dev/null || true
chmod 0644 "$BUILD_INFO" 2>/dev/null || true

# --- 6. QUY TAC SINGLE-CONF STRICT (Commit 5d2ff05 - Chong Crash-Loop CarPlay) ---
SO_TOKEN="libpatch-blmjcicarplay"
PRELOAD_LINE='            <environ_var env_name="LD_PRELOAD" env_value="/data_persist/cp-hud-mod/libpatch-blmjcicarplay.so"/>'
LDPATH_LINE='            <environ_var env_name="LD_LIBRARY_PATH" env_value="/jci/lib:/usr/lib"/>'

# Xac dinh ro: Xe co WCP hay khong
if [ -f /jci/sm/sm_WCP.conf ]; then
    TARGET_CONF="/jci/sm/sm_WCP.conf"
    CLEAN_CONF="/jci/sm/sm.conf"
else
    TARGET_CONF="/jci/sm/sm.conf"
    CLEAN_CONF=""
fi

# TAY SACH LD_PRELOAD khoi file khong duoc phep patch de triet tieu Crash-Loop
if [ -n "$CLEAN_CONF" ] && [ -f "$CLEAN_CONF" ]; then
    if grep -q "$SO_TOKEN" "$CLEAN_CONF"; then
        BAK="/data_persist/$(basename "$CLEAN_CONF").bak_precphud"
        if [ -s "$BAK" ] && ! grep -q "$SO_TOKEN" "$BAK"; then
            cp -a "$BAK" "$CLEAN_CONF"
        else
            grep -v "$SO_TOKEN" "$CLEAN_CONF" | grep -v 'LD_LIBRARY_PATH.*jci/lib:/usr/lib' > /tmp/cphud.clean && cp /tmp/cphud.clean "$CLEAN_CONF"
            rm -f /tmp/cphud.clean
        fi
    fi
fi

# PATCH DUY NHAT 1 FILE TARGET_CONF
if [ -f "$TARGET_CONF" ]; then
    BAK="/data_persist/$(basename "$TARGET_CONF").bak_precphud"
    [ -f "$BAK" ] || cp -a "$TARGET_CONF" "$BAK" 2>/dev/null || true

    if ! grep -q "$SO_TOKEN" "$TARGET_CONF"; then
        awk -v pl="$PRELOAD_LINE" -v lp="$LDPATH_LINE" '
            /name="jciCARPLAY"/ { print; print pl; print lp; next } 1
        ' "$TARGET_CONF" > /tmp/cphud.new
        old=$(wc -l < "$TARGET_CONF"); new=$(wc -l < /tmp/cphud.new)
        if [ $((new)) -eq $((old+2)) ] && grep -q "$SO_TOKEN" /tmp/cphud.new; then
            cp /tmp/cphud.new "$TARGET_CONF"
        fi
        rm -f /tmp/cphud.new
    fi
    sed -i '/name="jciCARPLAY"/ s/reset_board="yes"/reset_board="no"/' "$TARGET_CONF" 2>/dev/null || true
fi

# --- 7. Bat NaviSupported=TRUE trong devmgr_config_master.xml ---
MASTER="/etc/devmgr_config_master.xml"
if [ -f "$MASTER" ]; then
    MBAK="/data_persist/$(basename "$MASTER").bak_precphud"
    [ -f "$MBAK" ] || cp -a "$MASTER" "$MBAK" 2>/dev/null || true
    sed -i 's#<name>NaviSupported</name><value>FALSE</value>#<name>NaviSupported</name><value>TRUE</value>#' "$MASTER" 2>/dev/null || true
fi

# --- 8. Hook splim_udpd vao stage_gap2.sh va stage_wifi.sh ---
for STARTUP_SCRIPT in /jci/scripts/stage_gap2.sh /jci/scripts/stage_wifi.sh; do
    if [ -f "$STARTUP_SCRIPT" ]; then
        [ -f "$STARTUP_SCRIPT.bak_listener" ] || cp -a "$STARTUP_SCRIPT" "$STARTUP_SCRIPT.bak_listener" 2>/dev/null || true
        if ! grep -q "splim_udpd" "$STARTUP_SCRIPT"; then
            cat << 'HOOKEOF' >> "$STARTUP_SCRIPT"

# --- VietMap Live HUD Telemetry Listener Hook ---
killall splim_udpd cmu_splim_listener 2>/dev/null || true
if [ -x /data_persist/splim_udpd_start.sh ]; then
    sh /data_persist/splim_udpd_start.sh >/dev/null 2>&1 &
elif [ -x /data_persist/cp-hud-mod/splim_udpd ]; then
    nohup /data_persist/cp-hud-mod/splim_udpd >/dev/null 2>&1 &
fi
HOOKEOF
        fi
    fi
done

# --- 9. Dong bo du lieu xuong eMMC/Flash ---
sync

# --- 10. Hoan tat an toan: KHONG tu dong reboot ---
dbus-send --address=unix:path=/tmp/dbus_service_socket --type=method_call --dest=com.jci.vbs.navi /com/jci/vbs/navi com.jci.vbs.navi.PlayAudioBeep uint32:1 2>/dev/null || true
sync
mount -o ro,remount / 2>/dev/null || true

echo "[OK] Da cai dat HUD + listener, khong tu dong reboot."
echo "Kiem tra bang diag.sh truoc khi khoi dong lai CMU."
echo "Neu can nap LD_PRELOAD moi: nhan MUTE + NAV + BACK trong 10 giay sau khi da xac nhan backup."
