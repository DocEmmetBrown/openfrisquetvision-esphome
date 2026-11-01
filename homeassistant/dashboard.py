#!/usr/bin/env python3
"""Écrit le tableau de bord Frisquet pour un préfixe d'entités donné.

    python3 homeassistant/dashboard.py cuisine_frisquette > tableau.yaml

Le préfixe se lit sur une entité de la carte : climate.cuisine_frisquette_zone_1 -> cuisine_frisquette.
"""
import pathlib
import re
import sys

if len(sys.argv) != 2 or not re.fullmatch(r"[a-z0-9_]+", sys.argv[1]):
    sys.exit(__doc__)
template = (pathlib.Path(__file__).parent / "dashboard.yaml").read_text(encoding="utf-8")
body = template[template.index("title:"):]
sys.stdout.write(body.replace("PREFIXE_", sys.argv[1].rstrip("_") + "_"))
