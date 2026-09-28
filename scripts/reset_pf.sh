#!/bin/bash
# reset_pf.sh — Clear the Permanent Fail (PF) of a DJI Spark battery through an
# Arduino running spark_unbrick.ino ('A' command: unseal -> 0x0029 -> reset -> seal).
#
# Usage: ./reset_pf.sh [port]            # default /dev/cu.usbserial-10
#
# Before writing it checks bus stability (300 reads, 0 errors) and asks for
# confirmation. If a cell is below 2 V the sketch asks again and you answer.
#
# If the lowest cell is below the undervoltage threshold (~2.2 V) the PF
# re-latches 2-3 s after the reset; use pf_pump.sh in that case.

PORT=${1:-/dev/cu.usbserial-10}
R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[1m'; N=$'\033[0m'

[ -e "$PORT" ] || { echo "$PORT not found. Is the Arduino connected?"; exit 1; }

exec 3<>"$PORT" || exit 1
stty -f "$PORT" 115200 cs8 -cstopb -parenb raw -echo -hupcl
trap 'exec 3>&- 2>/dev/null' EXIT
sleep 3
while read -r -t 1 _ <&3; do :; done          # drop the boot menu

# Read Arduino lines until one matches the regex (or the timeout expires).
# Prints every line; leaves the last one in $LAST.
read_until() {  # read_until <regex> <seconds>
  local end=$(( $(date +%s) + $2 )) line
  LAST=""
  while (( $(date +%s) < end )); do
    IFS= read -r -t 2 line <&3 || continue
    line=${line%$'\r'}
    printf '%s\n' "$line"
    LAST=$line
    [[ $line =~ $1 ]] && return 0
  done
  return 1
}

echo "${B}1) Checking the connection to the battery...${N}"
printf 'T' >&3
if ! read_until 'Errors: [0-9]+' 20; then
  echo "${R}No reply from the Arduino.${N}"; exit 1
fi
ERR=$(sed -E 's/.*Errors: ([0-9]+).*/\1/' <<< "$LAST")
read_until 'Bus' 3 >/dev/null
if [ "$ERR" != "0" ]; then
  echo "${R}${B}Unstable bus ($ERR errors out of 300).${N} Secure SDA/SCL and the supply, then retry."
  echo "Nothing was written to the battery."
  exit 1
fi
echo "${G}Connection stable.${N}"
echo

echo "${B}2) Current status:${N}"
printf 'I' >&3
read_until '^=+$' 15                          # up to the closing '=' line
echo

echo "${Y}${B}The BMS will be unsealed and its permanent fail cleared.${N}"
echo "Keep the wake-up supply connected until it finishes and have the DJI charger ready."
read -r -p "Continue? (y/N) " ans
[[ $ans == [yY]* ]] || { echo "Cancelled. Nothing was written."; exit 0; }
echo

echo "${B}3) Recovery:${N}"
printf 'A' >&3
END_RE='\[OK\] PF cleared|\[!\] PF still active|Unseal failed|No reply at 0x0B|Cancelled'
end=$(( $(date +%s) + 120 ))
RESULT=""
while (( $(date +%s) < end )); do
  IFS= read -r -t 2 line <&3 || continue
  line=${line%$'\r'}
  printf '%s\n' "$line"
  if [[ $line == *"(y/n)"* ]]; then
    read -r -p "> (y/N) " a </dev/tty
    if [[ $a == [yY]* ]]; then printf 'y' >&3; else printf 'n' >&3; fi
  fi
  if [[ $line =~ $END_RE ]]; then RESULT=$line; break; fi
done
echo

case $RESULT in
  *"[OK] PF cleared"*)
    echo "${G}${B}PF cleared.${N}"
    echo "If the lowest cell is above ~2.2 V, move the battery to the DJI charger now."
    echo "If it is below, the PF will re-latch within seconds: run pf_pump.sh instead." ;;
  "")
    echo "${R}Did not finish within 120 s. Check the output above.${N}"; exit 1 ;;
  *)
    echo "${R}${B}Not completed:${N} $RESULT"; exit 1 ;;
esac
