import datetime
import pathlib
import unittest
from zoneinfo import ZoneInfo

import jinja2
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]


class States:
    def __init__(self, values):
        self.values = values

    def __call__(self, entity):
        return self.values.get(entity, "unknown")

    def __getitem__(self, entity):
        return self.values.get(entity)


class ImmediateHolidayTest(unittest.TestCase):
    def setUp(self):
        self.package = yaml.safe_load((ROOT / "homeassistant/holiday-immediate.yaml").read_text())
        self.sequence = self.package["script"]["frisquet_vacances_immediates"]["sequence"]
        self.entities = {
            "start_entity": "datetime.start",
            "end_entity": "datetime.end",
            "activate_entity": "button.activate",
        }
        self.values = {
            "datetime.start": "unknown",
            "datetime.end": "2026-11-12T13:30:00+00:00",
            "button.activate": "unknown",
        }
        self.now = datetime.datetime(2026, 11, 11, 18, 50)

    def render(self, template, **kwargs):
        def as_datetime(value, default):
            try:
                return datetime.datetime.fromisoformat(value)
            except ValueError:
                return default

        context = dict(
            states=States(self.values),
            has_value=lambda entity: self.values.get(entity, "unknown") not in ("unknown", "unavailable"),
            now=lambda: self.now,
            as_datetime=as_datetime,
            as_local=lambda value: value.replace(tzinfo=ZoneInfo("Europe/Paris")) if value.tzinfo is None
            else value.astimezone(ZoneInfo("Europe/Paris")),
            **self.entities,
        )
        environment = jinja2.Environment(undefined=jinja2.StrictUndefined)
        context["return_datetime"] = environment.from_string(
            self.sequence[0]["variables"]["return_datetime"]
        ).render(**context).strip()
        context.update({"departure": self.now.strftime("%Y-%m-%d %H:%M:%S")} | kwargs)
        return environment.from_string(template).render(**context).strip()

    def test_return_date_and_time_are_requested_and_preserved(self):
        self.assertNotIn("input_datetime", self.package)
        self.assertEqual(self.render(self.sequence[0]["variables"]["departure"]), "2026-11-11 18:50:00")
        self.assertEqual(self.render(self.sequence[3]["data"]["datetime"]), "2026-11-12 14:30:00")

    def test_future_return_today_is_accepted(self):
        self.values["datetime.end"] = "2026-11-11T19:15:00+00:00"
        self.assertEqual(self.render(self.sequence[1]["if"][0]["value_template"]), "False")
        self.assertEqual(self.render(self.sequence[3]["data"]["datetime"]), "2026-11-11 20:15:00")

    def test_invalid_or_unavailable_input_stops_with_notification(self):
        template = self.sequence[1]["if"][0]["value_template"]
        self.assertEqual(self.render(template), "False")
        for date in ("2026-11-11 18:50:00", "2026-11-11 18:49:59",
                     "2026-11-10 23:59:59", "unknown", "unavailable"):
            self.values["datetime.end"] = date
            self.assertEqual(self.render(template), "True")
        self.values["datetime.end"] = "2026-11-12T13:30:00+00:00"
        for entity in self.entities.values():
            saved = self.values.pop(entity)
            self.assertEqual(self.render(template), "True")
            self.values[entity] = "unavailable"
            self.assertEqual(self.render(template), "True")
            self.values[entity] = saved
        self.assertTrue(self.sequence[1]["then"][-1]["error"])
        self.assertEqual(self.sequence[1]["then"][0]["action"], "persistent_notification.create")

    def test_activation_waits_for_both_dates_and_never_uses_stale_values(self):
        self.assertEqual([self.sequence[i]["action"] for i in (2, 3, 6)],
                         ["datetime.set_value", "datetime.set_value", "button.press"])
        template = self.sequence[4]["wait_template"]
        departure = "2026-11-11 18:50:00"
        expected = {"departure": departure, "return_datetime": "2026-11-12 14:30:00"}
        self.assertEqual(self.render(template, **expected), "False")
        self.values["datetime.start"] = "2026-11-11T18:50:00"
        self.assertEqual(self.render(template, **expected), "True")
        self.values["datetime.end"] = "2026-11-12T00:00:00"
        self.assertEqual(self.render(template, **expected), "False")
        self.values["datetime.end"] = "2026-11-12T14:30:00"
        self.assertEqual(self.render(template, **expected), "True")
        self.values["datetime.start"] = "2026-11-11T17:50:00+00:00"
        self.values["datetime.end"] = "2026-11-12T13:30:00+00:00"
        self.assertEqual(self.render(template, **expected), "True")
        self.assertEqual(self.sequence[4]["timeout"], "00:00:15")
        self.assertTrue(self.sequence[5]["then"][-1]["error"])
        self.assertEqual(self.sequence[5]["then"][0]["action"], "persistent_notification.create")
