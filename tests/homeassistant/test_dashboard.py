import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
DASHBOARD = ROOT / "homeassistant" / "dashboard.yaml"
GENERATOR = ROOT / "homeassistant" / "dashboard.py"


def walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


class DashboardTest(unittest.TestCase):
    def setUp(self):
        self.dashboard = yaml.safe_load(DASHBOARD.read_text(encoding="utf-8"))
        self.nodes = list(walk(self.dashboard))

    def test_setpoints_have_buttons_and_manual_entry(self):
        tiles = [node for node in self.nodes if node.get("type") == "tile"]
        expected = {
            "number.frisquet_zone_1_consigne_confort",
            "number.frisquet_zone_1_consigne_reduit",
            "number.frisquet_zone_1_consigne_hors_gel",
        }
        self.assertEqual({tile["entity"] for tile in tiles}, expected)
        for tile in tiles:
            self.assertEqual(tile["features"], [{"type": "numeric-input", "style": "buttons"}])
            self.assertEqual(tile["tap_action"]["action"], "more-info")
            self.assertEqual(tile["icon_tap_action"]["action"], "more-info")
        config = yaml.load((ROOT / "frisquet.yaml").read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        self.assertEqual(len(config["number"]), 3)
        for number in config["number"]:
            self.assertEqual(number["mode"], "box")

    def test_vacation_actions_require_confirmation(self):
        buttons = [node for node in self.nodes if node.get("type") == "button"]
        self.assertEqual(len(buttons), 3)
        immediate = next(button for button in buttons if button.get("entity") == "script.frisquet_vacances_immediates")
        self.assertEqual(immediate["tap_action"]["service"], "script.frisquet_vacances_immediates")
        self.assertTrue(immediate["tap_action"]["confirmation"]["text"])
        self.assertEqual(immediate["tap_action"]["data"], {
            "start_entity": "datetime.frisquet_chaudiere_vacances_debut",
            "end_entity": "datetime.frisquet_chaudiere_vacances_fin",
            "activate_entity": "button.frisquet_chaudiere_activer_les_vacances",
        })
        for button in buttons:
            if button is immediate:
                continue
            action = button["tap_action"]
            self.assertEqual(action["action"], "call-service")
            self.assertEqual(action["service"], "button.press")
            self.assertEqual(action["target"]["entity_id"], button["entity"])
            self.assertTrue(action["confirmation"]["text"])
        dates = [node["entity"] for node in self.nodes if node.get("entity", "").startswith("datetime.")]
        self.assertEqual(set(dates), {
            "datetime.frisquet_chaudiere_vacances_debut",
            "datetime.frisquet_chaudiere_vacances_fin",
        })

    def test_program_card_uses_existing_program_and_write_entities(self):
        cards = [node for node in self.nodes if node.get("type") == "custom:frisquet-program-card"]
        self.assertEqual(len(cards), 2)
        for card in cards:
            self.assertEqual(card["entity"], "text.frisquet_zone_1_programme")
            self.assertEqual(card["write_entity"], "sensor.frisquet_zone_1_ecriture")
            self.assertEqual(card["profiles"], [f"input_text.frisquet_planning_{i}" for i in range(1, 5)])
        self.assertFalse(cards[0].get("profile_management", False))
        self.assertTrue(cards[1]["profile_management"])
        self.assertEqual(self.dashboard["views"][1]["path"], "plannings")

    def test_vacation_departure_options_are_explicit(self):
        cards = [node for node in self.nodes if node.get("title") == "Vacances"]
        self.assertEqual(len(cards), 1)
        card = cards[0]
        self.assertEqual(card["type"], "entities")
        self.assertEqual([row["entity"] for row in card["entities"][:2]], [
            "datetime.frisquet_chaudiere_vacances_debut",
            "datetime.frisquet_chaudiere_vacances_fin",
        ])
        self.assertEqual(card["entities"][2]["name"], "Partir maintenant")
        self.assertEqual(card["entities"][3]["action_name"], "Programmer")
        self.assertNotIn("input_datetime.frisquet_vacances_retour", str(card))

    def test_prefix_rewrites_entities_and_action_targets(self):
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "cuisine_frisquette"], check=True, capture_output=True, text=True
        )
        self.assertEqual(result.stderr, "")
        self.assertEqual(
            yaml.safe_load(result.stdout),
            yaml.safe_load(re.sub(
                r"\b(climate|sensor|binary_sensor|select|switch|number|button|datetime|text)\.frisquet_",
                r"\1.cuisine_frisquette_", DASHBOARD.read_text(encoding="utf-8"),
            )),
        )

    def test_mixed_prefixes_rewrite_action_targets(self):
        entities = {node["entity"] for node in self.nodes if "entity" in node}
        known = [entity.replace(".frisquet_", ".cuisine_frisquette_") for entity in sorted(entities)]
        target = "button.frisquet_chaudiere_activer_les_vacances"
        alternate = target.replace(".frisquet_", ".frisquette_")
        known[known.index(target.replace(".frisquet_", ".cuisine_frisquette_"))] = alternate
        with tempfile.TemporaryDirectory() as directory:
            entity_file = pathlib.Path(directory) / "entities.txt"
            entity_file.write_text("\n".join(known), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(GENERATOR), "cuisine_frisquette", "--entites", str(entity_file)],
                check=True, capture_output=True, text=True,
            )
        self.assertEqual(result.stderr, "")
        nodes = list(walk(yaml.safe_load(result.stdout)))
        button = next(node for node in nodes if node.get("type") == "button" and node["entity"] == alternate)
        self.assertEqual(button["tap_action"]["target"]["entity_id"], alternate)


if __name__ == "__main__":
    unittest.main()
