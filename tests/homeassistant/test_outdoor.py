import datetime
import pathlib
import sys
import types
import unittest

import jinja2
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from components.frisquet import CONFIG_SCHEMA
from esphome import config_validation as cv


class OutdoorTest(unittest.TestCase):
    def test_feature_is_opt_in_and_requires_writes(self):
        self.assertFalse(CONFIG_SCHEMA({"board": "heltec_v4_3"})["outdoor_emulation"])
        for level in ("none", "read", "ack"):
            with self.assertRaises(cv.Invalid):
                CONFIG_SCHEMA({"board": "heltec_v4_3", "tx": level, "outdoor_emulation": True})
        self.assertTrue(CONFIG_SCHEMA({
            "board": "heltec_v4_3", "tx": "write", "outdoor_emulation": True,
        })["outdoor_emulation"])

    def test_automation_uses_report_age_instead_of_value_change_time(self):
        automation = yaml.safe_load(
            (ROOT / "homeassistant/outdoor-sensor-automation.yaml").read_text(encoding="utf-8")
        )
        env = jinja2.Environment()
        now = datetime.datetime(2026, 11, 9, tzinfo=datetime.timezone.utc)
        env.globals.update(
            now=lambda: now,
            as_timestamp=lambda value: value.timestamp(),
            is_number=lambda value: str(value).replace(".", "", 1).replace("-", "", 1).isdigit(),
        )
        condition = env.from_string(automation["conditions"][0]["value_template"])
        age_template = env.from_string(automation["actions"][0]["data"]["sample_age_seconds"])
        source = types.SimpleNamespace(state="5.0", attributes={"unit_of_measurement": "°C"})
        env.globals["states"] = types.SimpleNamespace(sensor=types.SimpleNamespace(
            timmerflotte_temp_hmd_sensor_temperature_5=source,
        ))
        for age, expected in [(0, True), (60, True), (10799, True), (10800, False), (-10, False)]:
            source.last_reported = now - datetime.timedelta(seconds=age)
            self.assertEqual(condition.render().strip(), str(expected))
            self.assertEqual(int(age_template.render()), age)
        source.state = "unavailable"
        self.assertEqual(condition.render().strip(), "False")
        source.state = "61"
        self.assertEqual(condition.render().strip(), "False")
        source.state = "5"
        source.attributes["unit_of_measurement"] = "°F"
        self.assertEqual(condition.render().strip(), "False")


if __name__ == "__main__":
    unittest.main()
