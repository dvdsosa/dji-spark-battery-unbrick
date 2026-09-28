#!/bin/bash
# pf_pump.sh — "Pump" a DJI Spark battery past the undervoltage PF threshold.
#
# When the lowest cell is below ~2.2 V the BMS re-latches Permanent Fail 2-3 s
# after it is cleared. During that short window the BMS precharges from the
# wake-up supply (~13 mA through 100 ohm), so each round (unseal -> clear PF ->
# reset) lifts the cells a few mV. Once the lowest cell is above the threshold
# the PF stops re-latching and the BMS stays in normal precharge on its own.
#
# Measured on a Spark pack: ~6-10 mV per round; the PF stopped re-latching at
# round 3 (lowest cell 2.20 V), with only 2 extra PF flash writes.
#
# Usage: ./pf_pump.sh [max_rounds] [port]   # default 90, /dev/cu.usbserial-10
#
# Safety: checks bus stability first, asks for confirmation, stops on high
# temperature, failed unseal or lost communication. Each re-latch writes the
# BMS data flash, which has limited endurance: keep max_rounds low.

MAX_ROUNDS=${1:-90}
PORT=${2:-/dev/cu.usbserial-10}
TEMP_STOP=350                 # tenths of °C
REST_S=5                      # wait after each reset before reading at rest
R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[1m'; N=$'\033[0m'

[ -e "$PORT" ] || { echo "$PORT not found. Is the Arduino connected?"; exit 1; }

exec 3<>"$PORT" || exit 1
stty -f "$PORT" 115200 cs8 -cstopb -parenb raw -echo -hupcl
trap 'exec 3>&- 2>/dev/null' EXIT
sleep 3
while read -r -t 1 _ <&3; do :; done          # drop the boot menu

# wait_for <regex> <seconds>: consume lines until a match; last line in $LAST
wait_for() {
  local end=$(( $(date +%s) + $2 )) l
  LAST=""
  while (( $(date +%s) < end )); do
    IFS= read -r -t 2 l <&3 || continue
    l=${l%$'\r'}
    LAST=$l
    [[ $l =~ $1 ]] && return 0
  done
  return 1
}

# sample: one 'D' line parsed into PACK C1 C2 C3 CUR TEMP OP PFS PF MIN
sample() {
  local l
  printf 'D' >&3
  for _ in 1 2 3; do
    IFS= read -r -t 2 l <&3 || break
    l=${l%$'\r'}
    if [[ $l == D,* ]]; then
      IFS=, read -r _ PACK C1 C2 C3 CUR TEMP OP PFS <<< "$l"
      (( C1 < 0 || OP == 4294967295 )) && return 1
      PF=0
      (( (OP >> 12) & 1 )) && PF=1
      (( PFS != 0 )) && PF=1
      MIN=$C1; for c in $C2 $C3; do (( c < MIN )) && MIN=$c; done
      return 0
    fi
  done
  return 1
}

show() {  # show <label>
  local pf="${G}clear${N}"; (( PF )) && pf="${R}LATCHED${N}"
  printf '%-7s cells %4d / %4d / %4d mV   min %4d   %3d mA   %d.%d °C   PF %s\n' \
    "$1" "$C1" "$C2" "$C3" "$MIN" "$CUR" $(( TEMP / 10 )) $(( TEMP % 10 )) "$pf"
}

echo "${B}1) Checking the connection...${N}"
printf 'T' >&3
wait_for 'Errors: [0-9]+' 20 || { echo "${R}No reply from the Arduino.${N}"; exit 1; }
ERR=$(sed -E 's/.*Errors: ([0-9]+).*/\1/' <<< "$LAST")
wait_for 'Bus' 3
if [ "$ERR" != "0" ]; then
  echo "${R}${B}Unstable bus ($ERR/300 errors).${N} Secure the contacts and retry. Nothing written."
  exit 1
fi
sample || { echo "${R}Could not read the battery.${N}"; exit 1; }
show "start"
echo

if (( ! PF )); then
  echo "${G}PF is not latched. Nothing to do; use monitor_charge.sh to watch the charge.${N}"
  exit 0
fi

echo "${Y}${B}Up to $MAX_ROUNDS rounds of unseal -> clear PF -> reset will be run.${N}"
echo "Keep the wake-up supply (12 V through 100 Ω) connected the whole time."
if (( MIN < 2000 )); then
  echo "${R}${B}Lowest cell is $MIN mV (< 2.0 V): it may be internally damaged.${N}"
  read -r -p "Type 'yes' to continue anyway: " ans
  [ "$ans" = "yes" ] || { echo "Cancelled. Nothing written."; exit 0; }
else
  read -r -p "Continue? (y/N) " ans
  [[ $ans == [yY]* ]] || { echo "Cancelled. Nothing written."; exit 0; }
fi
echo

echo "${B}2) Pumping (reading at rest ${REST_S} s after each reset):${N}"
START_MIN=$MIN
for (( r = 1; r <= MAX_ROUNDS; r++ )); do
  printf 'U' >&3
  wait_for 'UNSEALED|FULL ACCESS|Unseal failed|Already unsealed' 30 || { echo "${R}Unseal: no reply.${N}"; exit 1; }
  [[ $LAST == *"Unseal failed"* ]] && { echo "${R}Unseal failed at round $r. Stopping.${N}"; exit 1; }
  printf 'P' >&3
  wait_for '\(cleared\)|may not update|run U first' 15
  printf 'R' >&3
  wait_for 'responding again|not responding' 10
  sleep "$REST_S"

  if ! sample; then
    sleep 2
    sample || { echo "${R}Lost communication at round $r. Stopping.${N}"; exit 1; }
  fi
  show "r$r"

  if (( TEMP >= TEMP_STOP )); then
    echo "${R}${B}Temperature above 35 °C. Stopping: disconnect the supply.${N}"; exit 1
  fi

  if (( ! PF )); then
    # Confirm it stays clear before declaring success
    sleep 10
    if sample && (( ! PF )); then
      show "check"
      echo
      echo "${G}${B}PF stays clear after $r round(s).${N} Lowest cell: $START_MIN -> $MIN mV."
      echo "The BMS should now precharge on its own from the supply."
      echo "Next: ./scripts/monitor_charge.sh, then the DJI charger once the target is reached."
      exit 0
    fi
    show "check"
  fi
done

echo
echo "${Y}${B}PF still re-latching after $MAX_ROUNDS rounds.${N} Lowest cell: $START_MIN -> $MIN mV."
echo "Consider precharging the cells directly (outside the BMS) instead of more rounds."
exit 2
