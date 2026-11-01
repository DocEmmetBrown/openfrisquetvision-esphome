# openfrisquetvisio-esphome

Composant ESPHome pour dialoguer avec une chaudière **Frisquet** (Eco Radio Visio) en LoRa/FSK 868 MHz :
émulation du Connect, des satellites d'ambiance et de la sonde extérieure, intégrée à **Home Assistant par l'API
native ESPHome** (sans broker MQTT).

Basé sur le protocole rétro-conçu par [OpenFrisquetVisio](https://github.com/FreedomNX/OpenFrisquetVisio).

> **Statut : phases R à 2d validées sur la chaudière ; depuis le 28/10 l'ESPHome est le Connect de la chaudière (association `D5`).** Voir [docs/DESIGN.md](docs/DESIGN.md) ;
> pour reprendre le travail : [docs/RESUME.md](docs/RESUME.md). Dépôt privé (licence amont à clarifier).

## Entités Home Assistant (zone 1)

- **Thermostat « Zone 1 »** (`climate`) : ambiante, consigne active, action (chauffe / repos). Modes : Auto (programme),
  Chauffe (permanent, préréglages Confort / Réduit), Arrêt = **Hors gel** (la chaudière reste protégée). Préréglage
  Boost = confort + 1 °C pendant 6 h au plus. La consigne réglée est celle du mode actif.
- Commandes séparées : « Zone 1 mode (commande) », consignes confort / réduit / hors-gel, programme, « Zone 1 boost »,
  mode ECS. Un changement depuis HA agit sur la chaudière en moins d'une minute et part aussi au satellite.
- Diagnostics : version du panneau, appareils appairés, températures de départ mini / maxi, état radio.

