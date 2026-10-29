#!/bin/sh
# Zone write sequence: each write waits for the boiler's verification before the next one.
# Usage (repository root): sh dev/ofv_setpoints.sh <log> "Zone 1 consigne confort=20.5" "select:Zone 1 mode (commande)=Auto" ...
LOG=$1
shift
for step in "$@"; do
  name=${step%=*}
  value=${step##*=}
  start=$(date +%H:%M:%S)
  case "$name" in
    select:*) .venv/bin/python dev/ofv_api.py select "${name#select:}" "$value" > /dev/null ;;
    text:*) .venv/bin/python dev/ofv_api.py text "${name#text:}" "$value" > /dev/null ;;
    *) .venv/bin/python dev/ofv_api.py set "$name" "$value" > /dev/null ;;
  esac
  echo "$start  $name -> $value"
  result=timeout
  i=0
  while [ $i -lt 150 ]; do
    line=$(awk -v t="[$start" '$0 >= t' "$LOG" | grep -a -E "Zone block write: (verified|failed|not-sent)|frisquet_radio\.(select|zone_number|zone_text)\]: .*refused" | head -1)
    if [ -n "$line" ]; then
      result=$(echo "$line" | sed -E 's/.*(verified|failed|not-sent|refused).*/\1/')
      break
    fi
    sleep 10
    i=$((i + 1))
  done
  echo "$(date +%H:%M:%S)  $name -> $value: $result"
  [ "$result" = verified ] || exit 1
  sleep 15
done
