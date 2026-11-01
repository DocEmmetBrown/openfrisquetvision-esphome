# Source this file (`. dev/ofv-env.sh`) before any `esphome` command.
# Exports the Wi-Fi credentials from the MQTT firmware's config backup (gitignored, never printed) and the
# PlatformIO directory used by ESPHome builds.
_ofv_root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
_ofv_backup="$_ofv_root/build/mqtt-config-backup.json"
if [ ! -f "$_ofv_backup" ]; then
  echo "missing $_ofv_backup (MQTT portal config export)" >&2
else
  OFV_WIFI_SSID="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['wifiSsid'])" "$_ofv_backup")"
  OFV_WIFI_PASSWORD="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['wifiPass'])" "$_ofv_backup")"
  export OFV_WIFI_SSID OFV_WIFI_PASSWORD
fi
export PLATFORMIO_CORE_DIR="$HOME/.platformio-esphome"
export OFV_DEVICE=10.35.5.140
unset _ofv_root _ofv_backup
