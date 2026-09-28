#!/bin/bash
# reset_pf.sh — Borra el Permanent Fail (PF) de una batería DJI Spark a través
# del Arduino con spark_unbrick.ino (comando 'A': unseal -> 0x0029 -> reset -> seal).
#
# Uso:   ./reset_pf.sh [puerto]          # por defecto /dev/cu.usbserial-10
#
# Antes de escribir comprueba la estabilidad del bus (300 lecturas, 0 errores)
# y pide confirmación. Si alguna celda está por debajo de 2 V, el sketch vuelve
# a preguntar y la respuesta la das tú.

PORT=${1:-/dev/cu.usbserial-10}
R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[1m'; N=$'\033[0m'

[ -e "$PORT" ] || { echo "No existe $PORT. ¿Arduino conectado?"; exit 1; }

exec 3<>"$PORT" || exit 1
stty -f "$PORT" 115200 cs8 -cstopb -parenb raw -echo -hupcl
trap 'exec 3>&- 2>/dev/null' EXIT
sleep 3
while read -r -t 1 _ <&3; do :; done          # descarta el menú de arranque

# Lee líneas del Arduino hasta que una contenga el patrón (o venza el plazo).
# Imprime cada línea; deja la última en $LAST.
read_until() {  # read_until <regex> <segundos>
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

echo "${B}1) Comprobando la conexión con la batería...${N}"
printf 'T' >&3
if ! read_until 'Errores: [0-9]+' 20; then
  echo "${R}Sin respuesta del Arduino.${N}"; exit 1
fi
ERR=$(sed -E 's/.*Errores: ([0-9]+).*/\1/' <<< "$LAST")
read_until 'Bus' 3 >/dev/null
if [ "$ERR" != "0" ]; then
  echo "${R}${B}Bus inestable ($ERR errores de 300).${N} Sujeta mejor SDA/SCL y los 12 V, y repite."
  echo "No se ha escrito nada en la batería."
  exit 1
fi
echo "${G}Conexión estable.${N}"
echo

echo "${B}2) Estado actual:${N}"
printf 'I' >&3
read_until '^=+$' 15                          # hasta la línea final de '='
echo

echo "${Y}${B}Se va a desbloquear el BMS y borrar el fallo permanente.${N}"
echo "Mantén los 12 V conectados hasta que termine y ten el cargador DJI preparado."
read -r -p "¿Continuar? (s/N) " ans
[[ $ans == [sSyY]* ]] || { echo "Cancelado. No se ha escrito nada."; exit 0; }
echo

echo "${B}3) Recuperación:${N}"
printf 'A' >&3
END_RE='\[OK\] PF borrado|\[!\] El PF sigue|No se pudo hacer unseal|No responde en 0x0B|Cancelado'
end=$(( $(date +%s) + 120 ))
RESULT=""
while (( $(date +%s) < end )); do
  IFS= read -r -t 2 line <&3 || continue
  line=${line%$'\r'}
  printf '%s\n' "$line"
  if [[ $line == *"(y/n)"* ]]; then
    read -r -p "> (s/N) " a </dev/tty
    if [[ $a == [sSyY]* ]]; then printf 'y' >&3; else printf 'n' >&3; fi
  fi
  if [[ $line =~ $END_RE ]]; then RESULT=$line; break; fi
done
echo

case $RESULT in
  *"[OK] PF borrado"*)
    echo "${G}${B}PF borrado.${N}"
    echo "${B}AHORA, sin esperar:${N} quita los 12 V y los cables del Arduino y pon la"
    echo "batería en el cargador DJI. Si las celdas siguen por debajo del umbral de"
    echo "subtensión (~2,2 V), el chip vuelve a bloquearse en menos de un minuto." ;;
  "")
    echo "${R}No terminó en 120 s. Revisa la salida de arriba.${N}"; exit 1 ;;
  *)
    echo "${R}${B}No se completó:${N} $RESULT"; exit 1 ;;
esac
