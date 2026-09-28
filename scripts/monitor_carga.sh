#!/bin/bash
# monitor_carga.sh — Vigila la precarga de una batería DJI Spark a través del
# Arduino con spark_unbrick.ino. Solo LEE (comando 'D'); nunca escribe en el BMS.
#
# Uso:   ./monitor_carga.sh [intervalo_s] [objetivo_mV] [puerto]
#        ./monitor_carga.sh                 # 10 s, 3000 mV, /dev/cu.usbserial-10
# Salir: Ctrl-C.  Se guarda un registro CSV en logs/.

INTERVAL=${1:-10}          # 10 s: detecta calentamiento o re-bloqueo en <1 ciclo útil
TARGET_MV=${2:-3000}       # desconectar 12 V cuando la celda MÁS BAJA llegue aquí
PORT=${3:-/dev/cu.usbserial-10}
BASE_MV=2000               # 0 % de la barra de progreso
TEMP_WARN=350              # décimas de °C
TEMP_STOP=400
SPREAD_WARN=300            # mV de desequilibrio entre celdas
BAR_W=40

LOG_DIR="$(cd "$(dirname "$0")/.." && pwd)/logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/carga_$(date +%Y%m%d_%H%M%S).csv"

R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[1m'; D=$'\033[2m'; N=$'\033[0m'
EL=$'\033[K'

[ -e "$PORT" ] || { echo "No existe $PORT. ¿Arduino conectado?"; exit 1; }

cleanup() {
  tput cnorm 2>/dev/null
  exec 3>&- 2>/dev/null
  printf '\n%sRegistro guardado en:%s %s\n' "$B" "$N" "$LOG"
  exit 0
}
trap cleanup INT TERM

# Abrir el puerto una sola vez (abrirlo resetea el Uno) y configurarlo
exec 3<>"$PORT" || exit 1
stty -f "$PORT" 115200 cs8 -cstopb -parenb raw -echo -hupcl
sleep 3
while read -r -t 1 _ <&3; do :; done          # descarta el menú de arranque

echo "fecha,pack_mV,c1_mV,c2_mV,c3_mV,corriente_mA,temp_dC,op_status,pf_status" > "$LOG"

bar() {  # bar <porcentaje 0..100>
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
  printf '%sMonitor de precarga · DJI Spark%s   %s%s%s\n' "$B" "$N" "$D" "$(date '+%H:%M:%S')" "$EL$N"
  printf '%sIntervalo %ss · objetivo celda mínima %s mV · Ctrl-C para salir%s%s\n\n' \
    "$D" "$INTERVAL" "$TARGET_MV" "$N" "$EL"

  if [[ $LINE != D,* ]]; then
    FAILS=$(( FAILS + 1 ))
    printf '%s%sSin respuesta del Arduino/BMS (%d seguidas).%s%s\n' "$R" "$B" "$FAILS" "$N" "$EL"
    printf 'Revisa los contactos de SDA/SCL y que los 12 V sigan conectados.%s\n' "$EL"
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

  # Progreso global = celda más baja (es la que limita)
  P=$(pct "$MIN"); (( P < 0 )) && P=0; (( P > 100 )) && P=100
  printf '%sProgreso (celda mínima)%s  %s %3d%%%s\n\n' "$B" "$N" "$(bar "$P")" "$P" "$EL"

  i=1
  for c in $C1 $C2 $C3; do
    cp=$(pct "$c")
    col=$G; (( c < 2500 )) && col=$Y; (( c < 2000 )) && col=$R
    printf '  Celda %d  %s%5d mV%s  %s %3d%%%s\n' "$i" "$col" "$c" "$N" "$(bar "$cp")" \
      $(( cp < 0 ? 0 : (cp > 100 ? 100 : cp) )) "$EL"
    i=$(( i + 1 ))
  done
  printf '\n'

  tcol=$G; (( TEMP >= TEMP_WARN )) && tcol=$Y; (( TEMP >= TEMP_STOP )) && tcol=$R
  scol=$G; (( SPREAD >= SPREAD_WARN )) && scol=$Y
  printf '  Pack          %6d mV%s\n' "$PACK" "$EL"
  printf '  Desequilibrio %s%6d mV%s%s\n' "$scol" "$SPREAD" "$N" "$EL"
  printf '  Corriente     %6d mA%s\n' "$CUR" "$EL"
  printf '  Temperatura   %s%6s °C%s%s\n' "$tcol" "$(fmt_t "$TEMP")" "$N" "$EL"
  printf '  Seguridad     %9s%s\n' "$SEC" "$EL"
  if (( PF )); then
    printf '  Fallo perm.   %s%9s%s%s\n' "$R" "ACTIVO" "$N" "$EL"
  else
    printf '  Fallo perm.   %s%9s%s%s\n' "$G" "no" "$N" "$EL"
  fi

  ELAPSED=$(( NOW - START ))
  RISE=$(( MIN - FIRST_MIN ))
  DT=$(( NOW - FIRST_T ))
  printf '\n  Tiempo        %s   · muestras %d%s\n' "$(fmt_dur $ELAPSED)" "$SAMPLES" "$EL"
  if (( DT >= 120 && RISE > 0 )); then
    ETA=$(( (TARGET_MV - MIN) * DT / RISE ))
    (( ETA < 0 )) && ETA=0
    printf '  Subida        %+d mV en %s · estimado hasta objetivo: %s%s\n' \
      "$RISE" "$(fmt_dur $DT)" "$(fmt_dur $ETA)" "$EL"
  else
    printf '  Subida        %+d mV (estimación tras 2 min de datos)%s\n' "$RISE" "$EL"
  fi
  printf '\n'

  # Avisos, de más a menos grave
  if (( TEMP >= TEMP_STOP )); then
    printf '%s%s>>> TEMPERATURA ALTA: DESCONECTA LOS 12 V YA <<<%s%s\a\n' "$R" "$B" "$N" "$EL"
  elif (( MIN >= TARGET_MV )); then
    printf '%s%s>>> OBJETIVO ALCANZADO: desconecta los 12 V y pasa al cargador DJI <<<%s%s\a\n' "$G" "$B" "$N" "$EL"
  elif (( PF )); then
    printf '%s%s>>> El fallo permanente se ha reactivado: la precarga está parada.%s%s\a\n' "$Y" "$B" "$N" "$EL"
    printf '    Ctrl-C y ejecuta scripts/reset_pf.sh para volver a borrarlo.%s\n' "$EL"
  elif (( CUR > 60 )); then
    printf '%s%s>>> Corriente mayor de lo esperado (%d mA): vigila y comprueba la resistencia de 100 Ω.%s%s\a\n' \
      "$Y" "$B" "$CUR" "$N" "$EL"
  elif (( SPREAD >= SPREAD_WARN )); then
    printf '%sDesequilibrio alto: si sigue creciendo, la celda más baja puede estar dañada.%s%s\n' "$Y" "$N" "$EL"
  elif (( TEMP >= TEMP_WARN )); then
    printf '%sTemperatura subiendo: vigila de cerca.%s%s\n' "$Y" "$N" "$EL"
  else
    printf '%sTodo normal.%s%s\n' "$G" "$N" "$EL"
  fi
  printf '%s%sRegistro: %s%s\n' "$EL" "$D" "$LOG" "$N"
  tput ed

  sleep "$INTERVAL"
done
