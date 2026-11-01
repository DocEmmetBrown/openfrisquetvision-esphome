#include "zone_climate.h"

#ifdef USE_CLIMATE
#include <cmath>

#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio.climate";

climate::ClimateTraits FrisquetClimate::traits() {
  climate::ClimateTraits traits;
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  traits.add_supported_mode(climate::CLIMATE_MODE_AUTO);
  traits.add_supported_preset(climate::CLIMATE_PRESET_COMFORT);
  traits.add_supported_preset(climate::CLIMATE_PRESET_ECO);
  traits.add_supported_preset(climate::CLIMATE_PRESET_BOOST);
  traits.set_visual_min_temperature(5.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void FrisquetClimate::control(const climate::ClimateCall &call) {
  if (this->parent_ == nullptr) return;
  const uint8_t *block = this->parent_->zone1_block();
  if (block == nullptr) {
    ESP_LOGW(TAG, "Refused: the zone block is not confirmed yet");
    this->published_ = false;
    this->refresh();
    return;
  }
  // One zone write at a time, the latest replacing the previous: apply one change per call.
  bool ok = true;
  const char *what = "nothing";
  const bool boosted = block[4] & 0x40;
  if (call.get_mode().has_value()) {
    switch (*call.get_mode()) {
      case climate::CLIMATE_MODE_OFF: ok = this->parent_->request_zone_mode("Hors Gel"); what = "Hors Gel"; break;
      case climate::CLIMATE_MODE_AUTO: ok = this->parent_->request_zone_mode("Auto"); what = "Auto"; break;
      default: {
        const bool eco = call.get_preset().has_value() && *call.get_preset() == climate::CLIMATE_PRESET_ECO;
        what = eco ? "Réduit" : "Confort";
        ok = this->parent_->request_zone_mode(what);
        break;
      }
    }
  } else if (call.get_preset().has_value()) {
    switch (*call.get_preset()) {
      case climate::CLIMATE_PRESET_BOOST: ok = this->parent_->request_zone_boost(true); what = "boost on"; break;
      case climate::CLIMATE_PRESET_ECO: ok = this->parent_->request_zone_mode("Réduit"); what = "Réduit"; break;
      default:
        // Comfort: from a boost, just stop it; otherwise permanent comfort.
        if (boosted) {
          ok = this->parent_->request_zone_boost(false);
          what = "boost off";
        } else {
          ok = this->parent_->request_zone_mode("Confort");
          what = "Confort";
        }
        break;
    }
  } else if (call.get_target_temperature().has_value()) {
    ok = this->parent_->request_zone_target(*call.get_target_temperature());
    what = "setpoint";
  }
  if (!ok) ESP_LOGW(TAG, "%s refused (a write in progress, not in comfort, or inconsistent values)", what);
  else ESP_LOGI(TAG, "%s queued; the thermostat shows the boiler's state once confirmed", what);
  // Show the real state again until the change is confirmed.
  this->published_ = false;
  this->refresh();
}

void FrisquetClimate::refresh() {
  if (this->parent_ == nullptr) return;
  const uint8_t *block = this->parent_->zone1_block();
  climate::ClimateMode mode = this->mode;
  optional<climate::ClimatePreset> preset = this->preset;
  if (block != nullptr) {
    const uint8_t m = block[3];
    mode = m == 0x05 ? climate::CLIMATE_MODE_AUTO : (m == 0x08 ? climate::CLIMATE_MODE_OFF : climate::CLIMATE_MODE_HEAT);
    const bool comfort = m == 0x06 || (m == 0x05 && (block[4] & 0x01));
    if (block[4] & 0x40) preset = climate::CLIMATE_PRESET_BOOST;
    else if (m == 0x08) preset = optional<climate::ClimatePreset>();
    else preset = comfort ? climate::CLIMATE_PRESET_COMFORT : climate::CLIMATE_PRESET_ECO;
  }
  const float current = this->parent_->zone1_ambient();
  const float target = this->parent_->zone1_setpoint();
  const climate::ClimateAction action = this->parent_->heating_demand() ? climate::CLIMATE_ACTION_HEATING
                                        : mode == climate::CLIMATE_MODE_OFF ? climate::CLIMATE_ACTION_OFF
                                                                            : climate::CLIMATE_ACTION_IDLE;
  auto same = [](float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
  if (this->published_ && mode == this->mode && preset == this->preset && action == this->action &&
      same(current, this->current_temperature) && same(target, this->target_temperature))
    return;
  this->mode = mode;
  this->preset = preset;
  this->action = action;
  this->current_temperature = current;
  this->target_temperature = target;
  this->published_ = true;
  this->publish_state();
}

void FrisquetClimate::dump_config() { LOG_CLIMATE("", "Frisquet zone 1", this); }

}  // namespace frisquet_radio
}  // namespace esphome
#endif
