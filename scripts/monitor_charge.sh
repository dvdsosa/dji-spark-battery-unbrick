#!/bin/bash
# monitor_charge.sh — Live dashboard for a DJI Spark battery (pre)charge, read
# through an Arduino running spark_unbrick.ino. READ-ONLY ('D' command): it never
# writes to the BMS.
#
# Usage: ./monitor_charge.sh [interval_s] [target_mV] [port]
#        ./monitor_charge.sh                # 10 s, 3000 mV, /dev/cu.usbserial-10
# Quit:  Ctrl-C.  A CSV log is written to logs/.

INTERVAL=${1:-10}          # 10 s: catches heating or PF re-latch within one useful cycle
TARGET_MV=${2:-3000}       # disconnect the supply when the LOWEST cell reaches this
PORT=${3:-/dev/cu.usbserial-10}
BASE_MV=2000               # 0 % of the progress bar
TEMP_WARN=350              # tenths of °C
TEMP_STOP=400
SPREAD_WARN=300            # mV spread between cells
BAR_W=40

LOG_DIR="$(cd "$(dirname "$0")/.." && pwd)/logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/charge_$(date +%Y%m%d_%H%M%S).csv"

R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[1m'; D=$'\033[2m'; N=$'\033[0m'
EL=$'\033[K'

[ -e "$PORT" ] || { echo "$PORT not found. Is the Arduino connected?"; exit 1; }

cleanup() {
  tput cnorm 2>/dev/null
  exec 3>&- 2>/dev/null
  printf '\n%sLog saved to:%s %s\n' "$B" "$N" "$LOG"
  exit 0
}
trap cleanup INT TERM

# Open the port once (opening it resets the Uno) and configure it
exec 3<>"$PORT" || exit 1
stty -f "$PORT" 115200 cs8 -cstopb -parenb raw -echo -hupcl
sleep 3
while read -r -t 1 _ <&3; do :; done          # drop the boot menu

echo "time,pack_mV,c1_mV,c2_mV,c3_mV,current_mA,temp_dC,op_status,pf_status" > "$LOG"

bar() {  # bar <percent 0..100>
  local p=$1 f i s=""
  (( p < 0 )) && p=0; (( p > 100 )) && p=100
  f=$(( p * BAR_W / 100 ))
  for (( i = 0; i < BAR_W; i++ )); do
    if (( i < f )); then s+="█"; else s+="░"; fi
  done
  printf '%s' "$s"
}

pct() { echo $(( ($1 - BASE_MV) * 100 / (TARGET_MV - BASE_MV) )); }

fmt_t() { printf '%d.%d' $(( $1 / 10 )) $(( ($1 < 0 ? -$1 : $1) % 10 )); }

fmt_dur() { printf '%dh %02dm %02ds' $(( $1 / 3600 )) $(( $1 % 3600 / 60 )) $(( $1 % 60 )); }

START=$(date +%s)
FIRST_MIN=""; SAMPLES=0; FAILS=0
tput civis 2>/dev/null
clear

while true; do
  NOW=$(date +%s)
  printf 'D' >&3
  LINE=""
  for _ in 1 2 3 4 5; do
    IFS= read -r -t 2 LINE <&3 || break
    LINE=${LINE%$'\r'}
    [[ $LINE == D,* ]] && break
  done

  tput cup 0 0
  printf '%sDJI Spark charge monitor%s   %s%s%s\n' "$B" "$N" "$D" "$(date '+%H:%M:%S')" "$EL$N"
  printf '%sInterval %ss · target lowest cell %s mV · Ctrl-C to quit%s%s\n\n' \
    "$D" "$INTERVAL" "$TARGET_MV" "$N" "$EL"

  if [[ $LINE != D,* ]]; then
    FAILS=$(( FAILS + 1 ))
    printf '%s%sNo reply from Arduino/BMS (%d in a row).%s%s\n' "$R" "$B" "$FAILS" "$N" "$EL"
    printf 'Check the SDA/SCL contacts and that the wake-up supply is still connected.%s\n' "$EL"
    tput ed
    sleep "$INTERVAL"; continue
  fi
  FAILS=0

  IFS=, read -r _ PACK C1 C2 C3 CUR TEMP OP PFS <<< "$LINE"
  echo "$(date '+%F %T'),$PACK,$C1,$C2,$C3,$CUR,$TEMP,$OP,$PFS" >> "$LOG"
  SAMPLES=$(( SAMPLES + 1 ))

  MIN=$C1; MAX=$C1
  for c in $C2 $C3; do (( c < MIN )) && MIN=$c; (( c > MAX )) && MAX=$c; done
  SPREAD=$(( MAX - MIN ))
  [ -z "$FIRST_MIN" ] && FIRST_MIN=$MIN && FIRST_T=$NOW

  PF=0; XCHG=0; SEC="?"
  if (( OP != 4294967295 )); then
    (( (OP >> 12) & 1 )) && PF=1
    (( (OP >> 14) & 1 )) && XCHG=1
    case $(( (OP >> 8) & 3 )) in 1) SEC="full access";; 2) SEC="unsealed";; 3) SEC="sealed";; esac
  fi
  (( PFS != 0 && PFS != 4294967295 )) && PF=1

  # Overall progress = lowest cell (the limiting one)
  P=$(pct "$MIN"); (( P < 0 )) && P=0; (( P > 100 )) && P=100
  printf '%sProgress (lowest cell)%s   %s %3d%%%s\n\n' "$B" "$N" "$(bar "$P")" "$P" "$EL"

  i=1
  for c in $C1 $C2 $C3; do
    cp=$(pct "$c")
    col=$G; (( c < 2500 )) && col=$Y; (( c < 2000 )) && col=$R
    printf '  Cell %d   %s%5d mV%s  %s %3d%%%s\n' "$i" "$col" "$c" "$N" "$(bar "$cp")" \
      $(( cp < 0 ? 0 : (cp > 100 ? 100 : cp) )) "$EL"
    i=$(( i + 1 ))
  done
  printf '\n'

  tcol=$G; (( TEMP >= TEMP_WARN )) && tcol=$Y; (( TEMP >= TEMP_STOP )) && tcol=$R
  scol=$G; (( SPREAD >= SPREAD_WARN )) && scol=$Y
  printf '  Pack          %6d mV%s\n' "$PACK" "$EL"
  printf '  Cell spread   %s%6d mV%s%s\n' "$scol" "$SPREAD" "$N" "$EL"
  printf '  Current       %6d mA%s\n' "$CUR" "$EL"
  printf '  Temperature   %s%6s °C%s%s\n' "$tcol" "$(fmt_t "$TEMP")" "$N" "$EL"
  printf '  Security      %9s%s\n' "$SEC" "$EL"
  if (( PF )); then
    printf '  Perm. fail    %s%9s%s%s\n' "$R" "ACTIVE" "$N" "$EL"
  else
    printf '  Perm. fail    %s%9s%s%s\n' "$G" "no" "$N" "$EL"
  fi

  ELAPSED=$(( NOW - START ))
  RISE=$(( MIN - FIRST_MIN ))
  DT=$(( NOW - FIRST_T ))
  printf '\n  Elapsed       %s   · samples %d%s\n' "$(fmt_dur $ELAPSED)" "$SAMPLES" "$EL"
  if (( DT >= 120 && RISE > 0 )); then
    ETA=$(( (TARGET_MV - MIN) * DT / RISE ))
    (( ETA < 0 )) && ETA=0
    printf '  Rise          %+d mV in %s · estimated to target: %s%s\n' \
      "$RISE" "$(fmt_dur $DT)" "$(fmt_dur $ETA)" "$EL"
  else
    printf '  Rise          %+d mV (estimate after 2 min of data)%s\n' "$RISE" "$EL"
  fi
  printf '\n'

  # Warnings, most severe first
  if (( TEMP >= TEMP_STOP )); then
    printf '%s%s>>> HIGH TEMPERATURE: DISCONNECT THE SUPPLY NOW <<<%s%s\a\n' "$R" "$B" "$N" "$EL"
  elif (( MIN >= TARGET_MV )); then
    printf '%s%s>>> TARGET REACHED: disconnect the supply and move to the DJI charger <<<%s%s\a\n' "$G" "$B" "$N" "$EL"
  elif (( PF )); then
    printf '%s%s>>> Permanent fail re-latched: precharge has stopped.%s%s\a\n' "$Y" "$B" "$N" "$EL"
    printf '    Ctrl-C and run scripts/pf_pump.sh to clear it again.%s\n' "$EL"
  elif (( CUR > 60 )); then
    printf '%s%s>>> Current higher than expected (%d mA): watch it and check the 100 Ω resistor.%s%s\a\n' \
      "$Y" "$B" "$CUR" "$N" "$EL"
  elif (( SPREAD >= SPREAD_WARN )); then
    printf '%sHigh cell spread: if it keeps growing, the lowest cell may be damaged.%s%s\n' "$Y" "$N" "$EL"
  elif (( TEMP >= TEMP_WARN )); then
    printf '%sTemperature rising: watch closely.%s%s\n' "$Y" "$N" "$EL"
  else
    printf '%sAll normal.%s%s\n' "$G" "$N" "$EL"
  fi
  printf '%s%sLog: %s%s\n' "$EL" "$D" "$LOG" "$N"
  tput ed

  sleep "$INTERVAL"
done
