#!/bin/bash
# venus supervisor: keep the phone alive and our iOS system running, hands free.
#
# The loop:
#   1. keeps the UDP console listener alive (tools/venus-udplog.py);
#   2. whenever the phone sits in the Qualcomm fastboot, boots the newest
#      image we have (shim/out/shim-boot-venus-vNN.img, newest by mtime);
#   3. whenever the phone runs iOS but the console goes quiet, asks the phone
#      to reset itself into fastboot (kernel patch 0102: the SPMI "restart
#      reason = bootloader" cookie + a PSCI reset) and boots the newest image
#      again.  This is what makes the phone self-sufficient: from the moment
#      a kernel with 0102 is running, no human is needed any more;
#   4. reports state changes to the Telegram status chat.
#
# Nothing here writes to the phone's flash: we only "fastboot boot" a
# temporary image, exactly like the manual flow.
#
# Env knobs: IMG_DIR, LOG, TG, STALE (s of silence before we act), CHECK
# (poll interval), HEARTBEAT (tg heartbeat), PHONE_IP, REBOOT=0 to disable
# the self-reboot request, BOOT=0 to never boot anything.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

IMG_DIR=${IMG_DIR:-shim/out}
LOG=${LOG:-logs/venus-live-20261021l.txt}
TG=${TG:-/Users/matviy/.venus-tg/send.sh}
STALE=${STALE:-180}           # seconds of console silence before we act
CHECK=${CHECK:-8}            # poll interval
HEARTBEAT=${HEARTBEAT:-600}  # telegram heartbeat
PHONE_IP=${PHONE_IP:-192.168.7.2}
REBOOT=${REBOOT:-1}
BOOT=${BOOT:-1}

ts() { date '+%H:%M:%S'; }
log() { echo "[$(ts)] $*"; }
tg()  { [ -x "$TG" ] && bash "$TG" "$1" >/dev/null 2>&1; }

last_size()  { stat -f '%z' "$LOG" 2>/dev/null || echo 0; }
last_mtime() { stat -f '%m' "$LOG" 2>/dev/null || echo 0; }
in_fastboot() { fastboot devices 2>/dev/null | grep -q 'fastboot'; }
newest_image() {
	# A pinned image wins: shim/out/CURRENT holds a file name.  That way we
	# can switch between a good and an experimental image without a rebuild
	# and without the crash-loop guard tripping.
	if [ -f "$IMG_DIR/CURRENT" ]; then
		local pinned="$IMG_DIR/$(cat "$IMG_DIR/CURRENT")"
		[ -f "$pinned" ] && { echo "$pinned"; return; }
	fi
	ls -t "$IMG_DIR"/shim-boot-venus-*.img 2>/dev/null | head -1
}

# The kernel (patch 0102) listens on UDP 5141: the payload "reboot" resets the
# phone into the Qualcomm fastboot, so the supervisor can boot a new image
# without anybody touching the phone.
ask_reboot() {
	[ "$REBOOT" = "1" ] || return 0
	printf 'reboot\n' | nc -u -w2 "$PHONE_IP" 5141 >/dev/null 2>&1
}

log "supervisor started (img=$IMG_DIR log=$LOG stale=${STALE}s)"
tg "venus-sweet: супервізор запущено. Телефон сам перезавантажується в fastboot, я сам завантажую найновіший образ."

last_hb=0
last_stale=0
booted_img=""
last_booted=""
last_boot_ts=0
while :; do
	# 1. the console listener must be alive
	if ! pgrep -f 'venus-udplog.py' >/dev/null 2>&1; then
		log "console listener is gone - restarting it"
		nohup bash tools/venus-udplog.sh "$LOG" >logs/udplog.out 2>&1 &
		sleep 2
	fi

	# 2. phone in fastboot -> boot the newest image
	if in_fastboot && [ "$BOOT" = "1" ]; then
		img=$(newest_image)
		if [ -n "$img" ] && [ "$img" = "$last_booted" ] && [ $(( $(date +%s) - last_boot_ts )) -lt 900 ]; then
			# we already booted this exact image less than 15 minutes ago and it
			# came back to fastboot on its own -> it crashed.  Do not loop.
			log "$img already booted $(( $(date +%s) - last_boot_ts ))s ago and it returned to fastboot -> NOT rebooting it again (crash loop guard)"
			tg "venus-sweet: образ $(basename "$img") впав і сам повернувся у fastboot -> не перезавантажую його повторно (захист від циклу). Потрібен нший образ."
			sleep 60
		elif [ -n "$img" ]; then
			log "fastboot detected -> booting $img"
			tg "venus-sweet: телефон у fastboot -> завантажую $(basename "$img")"
			# The ABL wedges after a long standby: a fastboot command can hang
			# forever.  Bound it, and if the phone does not answer afterwards,
			# tell the user that a power-cycle is needed.
			( fastboot boot "$img" >/tmp/venus-fb.log 2>&1 ) & fbp=$!
			( sleep 90; kill -9 $fbp 2>/dev/null ) & wd=$!
			wait $fbp 2>/dev/null
			kill $wd 2>/dev/null
			tail -2 /tmp/venus-fb.log 2>/dev/null | sed 's/^/    fastboot: /'
			booted_img="$img"
			last_booted="$img"
			last_boot_ts=$(date +%s)
			sleep 5
			if ! fastboot devices 2>/dev/null | grep -q fastboot; then
				log "fastboot did not answer after the boot attempt"
				tg "venus-sweet: fastboot завис (ABL після довгого стояння) -> потрібен power-cycle (Vol- + Power), далі я завантажу сам."
			fi
			sleep 20
		fi
	else
		# 3. The phone runs but the console is quiet.
		#    A quiet console does NOT mean a dead phone: when it sits on the
		#    home screen nothing is logged for minutes.  Only when it is
		#    silent AND it does not answer on the network do we call it dead.
		mt=$(last_mtime); now=$(date +%s)
		silent=$(( now - mt ))
		if [ "$mt" -gt 0 ] && [ "$silent" -gt "$STALE" ]; then
			if ping -c1 -W500 "$PHONE_IP" >/dev/null 2>&1; then
				alive=1
			else
				alive=0
			fi
			if [ "$alive" = "0" ] && [ $(( now - last_stale )) -ge 240 ]; then
				last_stale=$now
				log "phone unreachable (silent ${silent}s, no ping) -> asking it to reset itself"
				tg "venus-sweet: телефон не відповідає (${silent}s без логу і без ping) -> прошу його САМОМУ перезавантажитись у fastboot"
				ask_reboot
				sleep 25
			fi
		fi
	fi

	# 4. periodic heartbeat
	now=$(date +%s)
	if [ $(( now - last_hb )) -ge "$HEARTBEAT" ]; then
		last_hb=$now
		state=""
		in_fastboot && state="$state fastboot"
		[ -n "$booted_img" ] && state="$state booted=$(basename "$booted_img")"
		ping -c1 -W500 "$PHONE_IP" >/dev/null 2>&1 && state="$state iOS-живий"
		[ -n "$state" ] && state=" ($state)"
		tg "venus-sweet: супервізор живий$state"
	fi

	sleep "$CHECK"
done
