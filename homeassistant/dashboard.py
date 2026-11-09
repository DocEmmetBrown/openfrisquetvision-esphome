#!/usr/bin/env python3
"""Adapte le tableau de bord Frisquet (dashboard.yaml, écrit pour climate.frisquet_zone_1…) à vos entités.

    python3 homeassistant/dashboard.py cuisine_frisquette > tableau.yaml
    python3 homeassistant/dashboard.py cuisine_frisquette --entites entites.txt > tableau.yaml

Le préfixe se lit sur une entité de la carte : climate.cuisine_frisquette_zone_1 -> cuisine_frisquette.

Si toutes vos entités n'ont pas le même préfixe (par exemple certaines avec la pièce, d'autres sans), donnez aussi la
liste de vos entités : dans Home Assistant, Outils de développement > Modèle, collez

    {{ states | map(attribute='entity_id') | select('search', 'frisquet') | list }}

(remplacez « frisquet » par un mot du nom de votre appareil), et enregistrez le résultat dans entites.txt. Chaque entité
du tableau est alors cherchée dans cette liste par la fin de son identifiant.
"""
import argparse
import pathlib
import re
import sys

ENTITY = re.compile(r"\b(climate|sensor|binary_sensor|select|switch|number|button|datetime|text)\.frisquet_([a-z0-9_]+)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefixe", help="préfixe des entités, par exemple cuisine_frisquette")
    parser.add_argument("--entites", type=pathlib.Path, help="fichier contenant la liste de vos entités")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9_]+", args.prefixe):
        parser.error("le préfixe ne contient que des minuscules, chiffres et _")
    prefix = args.prefixe.rstrip("_")

    known: list[str] = []
    if args.entites:
        known = re.findall(r"\b[a-z_]+\.[a-z0-9_]+\b", args.entites.read_text(encoding="utf-8"))

    def resolve(match: re.Match) -> str:
        domain, suffix = match.group(1), match.group(2)
        default = f"{domain}.{prefix}_{suffix}"
        if not known or default in known:
            return default
        candidates = [e for e in known if e.startswith(domain + ".") and e.endswith("_" + suffix)]
        if len(candidates) == 1:
            return candidates[0]
        # Several devices may match: prefer the one sharing the most of the given prefix.
        candidates.sort(key=lambda e: -sum(part in e for part in prefix.split("_")))
        if candidates:
            return candidates[0]
        print(f"introuvable : {default}", file=sys.stderr)
        return default

    template = (pathlib.Path(__file__).parent / "dashboard.yaml").read_text(encoding="utf-8")
    body = template[template.index("title:"):]
    sys.stdout.write(ENTITY.sub(resolve, body))


if __name__ == "__main__":
    main()
