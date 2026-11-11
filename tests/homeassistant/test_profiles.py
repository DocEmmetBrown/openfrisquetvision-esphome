import pathlib
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ProfileConfigTest(unittest.TestCase):
    def test_helpers_and_card_agree_and_restore_saved_values(self):
        helpers = yaml.safe_load(
            (ROOT / "homeassistant/program-profiles-helpers.yaml").read_text(encoding="utf-8")
        )["input_text"]
        card = yaml.safe_load(
            (ROOT / "homeassistant/program-profiles-card.yaml").read_text(encoding="utf-8")
        )
        self.assertEqual(card["profiles"], [f"input_text.{name}" for name in helpers])
        for config in helpers.values():
            self.assertEqual(config["max"], 255)
            self.assertNotIn("initial", config)
        for key in ("comfort_entity", "reduced_entity", "frost_entity"):
            self.assertTrue(card[key].startswith("number."))

    def test_atomic_api_action_preserves_mode(self):
        package = yaml.load(
            (ROOT / "homeassistant/program-profiles-esphome.yaml").read_text(encoding="utf-8"),
            Loader=yaml.BaseLoader,
        )
        action, = package["api"]["actions"]
        self.assertEqual(action["action"], "apply_heating_profile")
        self.assertEqual(action["variables"], {
            "program": "string", "comfort": "float", "reduced": "float", "frost": "float",
        })
        self.assertIn("request_zone_profile(program, comfort, reduced, frost)", action["then"][0]["lambda"])
        self.assertNotIn("mode", action["variables"])


if __name__ == "__main__":
    unittest.main()
