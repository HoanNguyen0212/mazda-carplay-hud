#!/bin/sh
# ============================================================================
#  Mazda CX-5 Deep Diagnostic & System Log Collector (v3.5)
#  Target: Mazda Connect CMU150 (FW 74.00.324)
#  No underscores in script filename for easy terminal typing.
# ============================================================================

mount -o rw,remount / 2>/dev/null
MYDIR=$(dirname "$(readlink -f "$0")")
mount -o rw,remount "${MYDIR}" 2>/dev/null
mount -o rw,remount /tmp/mnt/sda1 2>/dev/null
mount -o rw,remount /tmp/mnt/sdb1 2>/dev/null

LOGDIR="${MYDIR}/logs"
mkdir -p "$LOGDIR" 2>/dev/null
OUT="${LOGDIR}/diag.log"

if ! touch "$OUT" 2>/dev/null; then
    OUT="/tmp/diag.log"
fi

echo "=== MAZDA CX-5 DEEP HUD & SYSTEM DIAGNOSTIC (v3.5) ===" > "$OUT"
echo "Capture Time: $(date)" >> "$OUT"
echo "Uptime: $(uptime 2>&1)" >> "$OUT"
echo "" >> "$OUT"

echo "==================== 1. HARDWARE & FIRMWARE ====================" >> "$OUT"
if [ -x /jci/scripts/get_board_type.sh ]; then
    /jci/scripts/get_board_type.sh >> "$OUT" 2>&1
    echo "Board type exit code: $?" >> "$OUT"
fi
cat /jci/version.ini 2>/dev/null >> "$OUT"
echo "" >> "$OUT"

echo "==================== 2. CARPLAY & MOD RUNTIME ====================" >> "$OUT"
CP_PID=$(pgrep -f jciCARPLAY 2>/dev/null | head -n 1)
if [ -n "$CP_PID" ]; then
    echo "Found jciCARPLAY PID: $CP_PID" >> "$OUT"
    cat "/proc/$CP_PID/cmdline" >> "$OUT"
    echo "" >> "$OUT"
    tr '\0' '\n' < "/proc/$CP_PID/environ" | grep -E "LD_PRELOAD|LD_LIBRARY_PATH" >> "$OUT"
    echo "" >> "$OUT"
    grep -i "libpatch" "/proc/$CP_PID/maps" >> "$OUT" 2>&1
else
    echo "jciCARPLAY process is NOT running!" >> "$OUT"
fi
echo "" >> "$OUT"

echo "==================== 3. HUD SERVICE D-BUS INTROSPECTION ====================" >> "$OUT"
if [ -x /usr/bin/dbus-send ]; then
    echo "--- Probe com.jci.vbs.navi.GetHUDStatus ---" >> "$OUT"
    dbus-send --address=unix:path=/tmp/dbus_service_socket --print-reply \
        --dest=com.jci.vbs.navi /com/jci/vbs/navi com.jci.vbs.navi.GetHUDStatus 2>&1 >> "$OUT"
    echo "" >> "$OUT"
    echo "--- Introspect /com/jci/vbs/navi ---" >> "$OUT"
    dbus-send --address=unix:path=/tmp/dbus_service_socket --print-reply \
        --dest=com.jci.vbs.navi /com/jci/vbs/navi org.freedesktop.DBus.Introspectable.Introspect 2>&1 >> "$OUT"
fi
echo "" >> "$OUT"

echo "==================== 4. VEHICLE CONFIG & HUD SETTING FILES ====================" >> "$OUT"
for cfg in /data/TSR_Feature_Mode.txt /data/vehicleType.txt /data/lds_gps.txt \
           /data_persist/model.txt /data_persist/fuelType.txt /data_persist/HubType.txt \
           /data_persist/netmgmt_WCPAvailability.txt; do
    if [ -f "$cfg" ]; then
        echo "--- $cfg ---" >> "$OUT"
        cat "$cfg" 2>&1 >> "$OUT"
        echo "" >> "$OUT"
    fi
done
echo "" >> "$OUT"

echo "==================== 5. ACTIVE NETWORK & PORTS ====================" >> "$OUT"
ifconfig -a >> "$OUT" 2>&1
echo "" >> "$OUT"
netstat -an 2>/dev/null | grep -E "8888|50505|LISTEN" >> "$OUT" 2>&1
echo "" >> "$OUT"
route -n 2>/dev/null >> "$OUT" 2>&1
echo "" >> "$OUT"
arp -an 2>/dev/null >> "$OUT" 2>&1
echo "" >> "$OUT"

# Read-only raw-speed probe. GetCarSpeed unit/scaling is not documented by the
# CMU XML, so preserve the unscaled replies for units 0..3 instead of inferring
# km/h from the listener's normalized debug line alone.
RAW_SPEED_LOG="${LOGDIR}/cmu_speed_raw_probe.log"
if [ -x /usr/bin/dbus-send ] && [ -S /tmp/dbus_service_socket ]; then
    : > "$RAW_SPEED_LOG"
    for unit in 0 1 2 3; do
        echo "=== GetCarSpeed speedUnit=${unit} ===" >> "$RAW_SPEED_LOG"
        /usr/bin/dbus-send --address=unix:path=/tmp/dbus_service_socket --print-reply \
            --type=method_call --dest=com.jci.vbs.bcm /com/jci/vbs/bcm \
            com.jci.vbs.bcm.GetCarSpeed "uint32:${unit}" >> "$RAW_SPEED_LOG" 2>&1
        echo "" >> "$RAW_SPEED_LOG"
    done
fi

echo "==================== 6. MOD LOGS & TELEMETRY ====================" >> "$OUT"
for lf in /tmp/carplay_bridge.log /tmp/cmu_listener.log /tmp/cmu_splim_listener.log \
          /tmp/cmu_speed_debug.log /data_persist/cp-hud-mod/clock-sync-status.txt \
          /data_persist/cp-hud-mod/build-info.txt /data_persist/cp-hud-mod/manifest.txt \
          "${LOGDIR}/test_hud.log" "${LOGDIR}/testhud.log" \
          "${LOGDIR}/live.log" "${LOGDIR}/live_hud_drive.log" "${LOGDIR}/live_speed_raw.log" \
          "${LOGDIR}/cmu_speed_raw_probe.log"; do
    if [ -f "$lf" ]; then
        echo "--- File: $lf ($(wc -l < "$lf") lines) ---" >> "$OUT"
        tail -n 80 "$lf" >> "$OUT"
        cp -f "$lf" "${LOGDIR}/" 2>/dev/null
        echo "" >> "$OUT"
    fi
done
echo "" >> "$OUT"

echo "==================== 7. PERSISTENT HUD FILES ====================" >> "$OUT"
ls -la /data_persist/cp-hud-mod/ >> "$OUT" 2>&1
for pf in /data_persist/splim /data_persist/nav /data_persist/splim_over; do
    if [ -f "$pf" ]; then
        echo "--- $pf: [$(cat "$pf")] ---" >> "$OUT"
    else
        echo "$pf does not exist" >> "$OUT"
    fi
done
echo "" >> "$OUT"
echo "MD5 Check of installed .so:" >> "$OUT"
md5sum /data_persist/cp-hud-mod/libpatch-blmjcicarplay.so 2>/dev/null >> "$OUT"
md5sum "${MYDIR}/libpatch-blmjcicarplay.so" 2>/dev/null >> "$OUT"
md5sum /data_persist/cp-hud-mod/cmu_splim_listener 2>/dev/null >> "$OUT"
md5sum "${MYDIR}/cmu_splim_listener" 2>/dev/null >> "$OUT"
echo "" >> "$OUT"

echo "==================== 8. CONFIG FILES ON CMU ====================" >> "$OUT"
grep -n -C 3 "jciCARPLAY" /jci/sm/sm.conf 2>/dev/null >> "$OUT"
grep -n -C 3 "jciCARPLAY" /jci/sm/sm_WCP.conf 2>/dev/null >> "$OUT"
grep -n -C 2 "NaviSupported" /etc/devmgr_config_master.xml 2>/dev/null >> "$OUT"
grep -n -C 3 "cmu_splim_listener" /jci/scripts/stage_wifi.sh 2>/dev/null >> "$OUT"
echo "" >> "$OUT"

echo "==================== 9. KERNEL LOGS (dmesg) ====================" >> "$OUT"
dmesg | tail -n 80 >> "$OUT" 2>&1

# --- 10. AUTOMATED EVIDENCE-BASED DIAGNOSIS -------------------------------
# Keep this separate from diag.log so a short tail cannot hide the full raw logs.
ANALYSIS="${LOGDIR}/diagnosis.txt"
LISTENER_LOG="${LOGDIR}/cmu_listener.log"
BRIDGE_LOG="${LOGDIR}/carplay_bridge.log"
SPEED_LOG="${LOGDIR}/cmu_speed_debug.log"

limit_rx=0
limit_zero=0
nav_rx=0
nav_idle=0
over_rx=0
hud_frames=0
hud_zero=0
hud_nonzero=0
hud_transitions=0
route_retries=0
last_limit=0
speed_min=0
speed_max=0
speed_last=0
speed_count=0

if [ -f "$LISTENER_LOG" ]; then
    limit_rx=$(grep -E -c 'RX: LIMIT [0-9]+' "$LISTENER_LOG" 2>/dev/null || true)
    limit_zero=$(grep -E -c 'RX: LIMIT 0([[:space:]]|$)' "$LISTENER_LOG" 2>/dev/null || true)
    nav_rx=$(grep -E -c 'RX: NAV ' "$LISTENER_LOG" 2>/dev/null || true)
    nav_idle=$(grep -E -c 'RX: NAV 0 0([[:space:]]|$)' "$LISTENER_LOG" 2>/dev/null || true)
    over_rx=$(grep -E -c 'RX: OVER ' "$LISTENER_LOG" 2>/dev/null || true)
    last_limit=$(awk '/RX: LIMIT [0-9]+/ { for (i=1; i<=NF; i++) if ($i == "LIMIT") v=$(i+1)+0 } END { print v+0 }' "$LISTENER_LOG" 2>/dev/null)
fi

if [ -f "$BRIDGE_LOG" ]; then
    hud_frames=$(grep -E -c 'hud_send: SetHUDDisplayMsgReq ' "$BRIDGE_LOG" 2>/dev/null || true)
    hud_zero=$(grep -E -c 'hud_send: SetHUDDisplayMsgReq .*splim=0([[:space:]]|$)' "$BRIDGE_LOG" 2>/dev/null || true)
    hud_nonzero=$(grep -E -c 'hud_send: SetHUDDisplayMsgReq .*splim=[1-9][0-9]*([[:space:]]|$)' "$BRIDGE_LOG" 2>/dev/null || true)
    route_retries=$(grep -E -c 'retry#[0-9]+ StartNaviRouteGuidence' "$BRIDGE_LOG" 2>/dev/null || true)
    hud_transitions=$(awk '
        /hud_send: SetHUDDisplayMsgReq / {
            v=$0
            sub(/^.*splim=/, "", v)
            sub(/[^0-9].*$/, "", v)
            if (v ~ /^[0-9]+$/) {
                if (have && v != prev) transitions++
                prev=v
                have=1
            }
        }
        END { print transitions+0 }
    ' "$BRIDGE_LOG" 2>/dev/null)
fi

if [ -f "$SPEED_LOG" ]; then
    set -- $(awk '
        /queried speed=/ {
            v=$0
            sub(/^.*speed=/, "", v)
            v=v+0
            if (n == 0 || v < min) min=v
            if (v > max) max=v
            last=v
            n++
        }
        END {
            if (n) print min, max, last, n
            else print 0, 0, 0, 0
        }
    ' "$SPEED_LOG" 2>/dev/null)
    speed_min=${1:-0}; speed_max=${2:-0}; speed_last=${3:-0}; speed_count=${4:-0}
fi

current_splim="missing"
current_over="missing"
[ -f /data_persist/splim ] && current_splim=$(tr '\r\n' ' ' < /data_persist/splim 2>/dev/null)
[ -f /data_persist/splim_over ] && current_over=$(tr '\r\n' ' ' < /data_persist/splim_over 2>/dev/null)

{
    echo "=== AUTOMATED HUD DIAGNOSIS ==="
    echo "Generated: $(date)"
    echo "Listener LIMIT packets: ${limit_rx} (LIMIT 0: ${limit_zero})"
    echo "Listener NAV packets: ${nav_rx} (NAV 0 0: ${nav_idle})"
    echo "Listener OVER packets: ${over_rx}"
    echo "Bridge HUD frames: ${hud_frames} (painted splim=0: ${hud_zero}, non-zero: ${hud_nonzero}, transitions: ${hud_transitions})"
    echo "Speed samples: ${speed_count} (min=${speed_min}, max=${speed_max}, last=${speed_last} km/h)"
    echo "Last logged LIMIT: ${last_limit} km/h"
    echo "Current /data_persist/splim: ${current_splim}"
    echo "Current /data_persist/splim_over: ${current_over}"
    echo "StartNaviRouteGuidence retries logged: ${route_retries}"
    echo ""
    if [ "$hud_frames" -gt 0 ] && [ "$limit_rx" -gt 0 ]; then
        echo "TRANSPORT: PASS — UDP listener received telemetry and the CarPlay bridge emitted HUD frames."
    else
        echo "TRANSPORT: INCOMPLETE — missing listener RX or bridge HUD frames."
    fi
    if [ "$hud_zero" -gt 0 ] && [ "$hud_nonzero" -gt 0 ]; then
        if [ "$last_limit" -ge 5 ] && [ "$speed_max" -ge $((last_limit + 2)) ]; then
            echo "CLASSIFICATION: OVERSPEED_BLINK_CONSISTENT_NOT_PROVEN — splim=0 alternation matches the configured overspeed blink (speed >= limit + 2 km/h), not a LIMIT-0 packet storm. Validate the BCM GetCarSpeed unit/scaling before treating it as definitive."
        else
            echo "CLASSIFICATION: SPEED_LIMIT_TOGGLE_REQUIRES_REVIEW — both zero and non-zero HUD limits were painted, but this capture lacks matching overspeed evidence."
        fi
    elif [ "$hud_nonzero" -gt 0 ]; then
        echo "CLASSIFICATION: STEADY_SPEED_LIMIT — no zero-limit HUD paint was observed."
    else
        echo "CLASSIFICATION: NO_SPEED_LIMIT_FRAME — no non-zero HUD speed-limit frame was observed."
    fi
    if [ "$limit_zero" -gt 0 ] && [ "$hud_zero" -gt "$limit_zero" ]; then
        echo "NOTE: HUD zero paints exceed received LIMIT 0 packets; inspect the overspeed flag path before blaming telemetry clearing."
    fi
    if [ "$nav_rx" -gt 0 ]; then
        echo "NAVIGATION: listener received active navigation packets; repeated idle packets must be treated as state transitions, not fresh clears."
    fi
} > "$ANALYSIS"
cat "$ANALYSIS" >> "$OUT"

if [ "$OUT" = "/tmp/diag.log" ] && [ -d "$LOGDIR" ]; then
    cp -f /tmp/diag.log "${LOGDIR}/diag.log" 2>/dev/null
fi

sync
echo ""
echo "[OK] DA THU THAP LOG XONG!"
echo "File luu tai: logs/diag.log va logs/diagnosis.txt tren USB"
