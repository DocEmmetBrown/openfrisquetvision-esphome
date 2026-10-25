# OpenFrisquetVisio ESPHome — Document de conception

> **Statut : brouillon v0.1, à valider ensemble.** Aucun code n'est écrit.
> Les points marqués **[DÉCISION]** attendent votre arbitrage (section 11).

## 1. Objectif

Réécrire [OpenFrisquetVisio](https://github.com/FreedomNX/OpenFrisquetVisio) comme un **composant externe ESPHome** :
un ESP32 + radio LoRa/FSK qui parle directement à une chaudière Frisquet (émulation du Connect, des satellites
d'ambiance et de la sonde extérieure) et s'intègre à Home Assistant par l'**API native ESPHome**, **sans broker MQTT**.

**Principes directeurs**

1. **Tout ce qui touche la radio, ou qui doit continuer à fonctionner sans Home Assistant, reste dans l'appareil.**
2. **Tout ce qui est interface, historique, planification de confort, notifications ou dérivé se fait dans HA.**
3. **Aucune fonctionnalité de l'ancien firmware n'est perdue** : elle est soit conservée, soit déplacée dans HA/ESPHome
   (voir la table de la section 7).
4. **Sûreté du chauffage d'abord** : le mode lecture seule (écoute passive) est livré avant toute émission radio.

### Hors périmètre

- Remplacer le protocole Frisquet (il reste celui rétro-ingénié par OpenFrisquetVisio).
- Support Matter (hors sujet pour la v1 ; l'API ESPHome suffit).
- Portail web propre au projet (remplacé par ESPHome, voir section 7).

## 2. Ce que l'on sait déjà (contraintes issues du firmware MQTT)

| Constat | Conséquence pour la conception |
|---|---|
| Les échanges radio sont **bloquants** (`sendAsk` : jusqu'à ~21 s avec 2 tentatives de 10 s). | Ils ne peuvent pas tourner dans la boucle principale d'ESPHome (watchdog, déconnexions API). → **tâche radio dédiée** (section 4.2). |
| Une seule radio, **half-duplex**, et des créneaux imposés par la chaudière : satellites à :00 / :01 / :02 (±30 s), lectures Connect à :03 / :06 / :08, consommation à :05, mode ECS à :07 (10 min). | Un **ordonnanceur de créneaux** central avec file de transactions et fenêtre silencieuse. |
| Une association échouée laissait la radio sur l'ID réseau de diffusion `FF:FF:FF:FF` (bug corrigé aujourd'hui dans le firmware MQTT, `b1e6934`). | La configuration radio est **toujours restaurée** (garde RAII) ; test dédié. |
| Réception **marginale** sur le Heltec V4.3 : capture brute de 68 octets, CRC logiciel, réparation d'un bit, bypass du LNA et bande RX réglables. | Le pilote radio doit exposer la **capture brute** et le CRC maison. Diagnostics radio indispensables (RSSI, compteurs). |
| Les chaudières diffèrent : fumées, puissances et consommations restent à 0 sur certains modèles. | Capteurs **optionnels** et mention « non fourni par cette chaudière ». |
| Le Connect écrit le **bloc complet de zone** (modes, consignes, programme de 42 octets). | Le programme doit être **relu et réécrit à l'identique** même si l'utilisateur ne l'édite pas. |
| Le code actuel mélange protocole, état métier et MQTT dans les mêmes classes. | Séparer un **cœur C++ sans dépendance ESPHome** (testable sur hôte). |

## 3. Pourquoi un composant maison plutôt que `sx126x` d'ESPHome

Le composant officiel `sx126x` propose FSK (bande passante RX, sync word jusqu'à 8 octets, CRC matériel,
préambule, déviation). Il ne couvre pas :

- la **capture brute** avec CRC logiciel et réparation de glissement de bit (`RadioCrc`) ;
- le **changement de sync word à chaud** (NetworkID de 4 octets, diffusion pendant l'association) ;
- la commande du **FEM KCT8103L** du V4.3 (alimentation, activation, bascule PA/LNA) ;
- le support du **SX1276** (LilyGo V2.1).

→ **On réutilise RadioLib 7.8.1** (déjà validé sur le terrain) dans un composant ESPHome en framework Arduino
(`esphome: libraries:`). L'option « composant `sx126x` officiel » est évaluée dans le spike (section 10) et abandonnée
si elle ne permet pas la capture brute.

## 4. Architecture

### 4.1 Couches

```
 ┌─────────────────────────── Home Assistant ───────────────────────────┐
 │  Entités (API native)  ·  actions  ·  événements  ·  blueprints      │
 └──────────────────────────────▲───────────────────────────────────────┘
                                │ boucle principale ESPHome (non bloquante)
 ┌──────────────────────────────┴───────────────────────────────────────┐
 │ components/frisquet  (glue ESPHome : hub, climate, sensor, select…)  │
 ├──────────────────────────────────────────────────────────────────────┤
 │ core/ (C++ pur, sans ESPHome, testé sur hôte)                        │
 │   modèle métier : Boiler, Zone×3, Programme, Dérogations             │
 │   émulateurs : Connect · Satellite×3 (physique/virtuel) · Sonde ext. │
 │               · Écouteur passif                                      │
 │   protocole : transactions Ask/Init/Answer, mémoire (0x79E0…0xA0F0)  │
 │   liaison : en-tête, ack, retry, CRC16 + réparation 1 bit            │
 ├──────────────────────────────────────────────────────────────────────┤
 │ radio driver (RadioLib : SX1262 / SX1276, FEM KCT8103L)              │
 └──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Concurrence

- **Une tâche FreeRTOS** possède la radio (aucun autre code n'y touche) et exécute les transactions.
- La boucle ESPHome envoie des **commandes** dans une file (`SetMode`, `SendZone`, `ReadMemory`, …) et reçoit des
  **résultats/événements** via une seconde file. **Les entités ne sont modifiées que dans la boucle principale.**
- Le **cœur** est une machine à états sans E/S : il reçoit des horodatages et des trames, et rend des actions radio.
  Il est donc testable avec des trames enregistrées, sans matériel.
- Réception : IRQ → tampon → décodage (CRC/réparation) dans la tâche radio ; les trames « non attendues » sont
  mises en tampon (comme `bufferUnexpectedPacket`) pour être réutilisées.

### 4.3 Ordonnanceur de créneaux

- Horloge = **heure ESPHome** (`time:` via HA/SNTP), repli sur l'heure chaudière, puis sur l'uptime.
- Chaque tâche périodique a une période et un décalage (ex. températures à :03/:06/:08 par tranche de 10 min).
  **Une tentative par créneau**, pas de rattrapage en rafale.
- **Fenêtre silencieuse** : pas d'émission autour des créneaux des satellites (:00 à :02 ±30 s).
- Priorité : *action utilisateur* > *envoi de zone en attente* > *lectures périodiques*.

### 4.4 Persistance

Via le système de préférences d'ESPHome : NetworkID, ID d'association par appareil émulé, consignes
(Confort/Réduit/Hors gel/Boost), programme hebdomadaire, échéance de dérogation, indicateurs « envoi en attente ».

### 4.5 Configuration (YAML)

Ce que le portail faisait (WiFi, MQTT, mot de passe, OTA) devient de la **configuration ESPHome standard** (`wifi:`,
`api: encryption`, `ota:`). Les réglages propres au projet sont dans le bloc `frisquet:` :

```yaml
frisquet:
  id: chaudiere
  radio:
    chip: sx1262            # sx1262 | sx1276
    board: heltec_v4_3      # pins + FEM KCT8103L préconfigurés
    rx_bandwidth: 234.3kHz
    lna_bypass: false       # V4.3 : false = LNA actif
  network_id: "AA:BB:CC:DD" # ou « auto » via le bouton de récupération
  connect: active           # none | active | passive
  outdoor_probe:
    source: sensor          # none | sensor (DS18B20, capteur HA, …) | frisquet (sonde existante)
    sensor: outdoor_temp    # un `sensor` ESPHome quelconque
  zones:
    - id: 1
      source: connect       # connect | physical_satellite | virtual_satellite
    - id: 2
      source: virtual_satellite
      ambient_sensor: salon_temp
  developer: false

sensor:
  - platform: homeassistant          # sonde virtuelle : directement depuis HA
    id: outdoor_temp
    entity_id: weather.maison
    attribute: temperature
```

> **Gain par rapport au firmware MQTT** : la « sonde virtuelle » et la température ambiante d'un satellite virtuel
> se lisent directement depuis une entité HA (`platform: homeassistant`) ou un capteur local (`dallas_temp`).
> Plus d'automatisation MQTT à écrire.

Les choix **structurels** (zones, sources, Connect actif/passif) sont dans le YAML (recompilation nécessaire) ;
les réglages **d'usage** (consignes, modes, boost) sont des entités modifiables à chaud.

## 5. Entités exposées

### Par zone (N = 1..3, si activée)

| Entité | Type ESPHome | Notes |
|---|---|---|
| Thermostat | `climate` | Modes `auto` / `heat` ; consigne ; préréglages : Confort, Réduit, Hors gel, Boost, Dérogation Confort, Dérogation Réduit. Même sémantique que le firmware MQTT. |
| Température ambiante | `sensor` | Satellite virtuel : alimentée par `ambient_sensor`. |
| Consigne active / Consigne de départ / Température de départ | `sensor` | |
| Consignes Confort, Réduit, Hors gel, supplément Boost | `number` (config) | 5–30 °C, pas de 0,5 °C. |
| Boost | `switch` | |
| Mode actif / Régime actif | `text_sensor` | Conservent le détail (écoute seule comprise). |
| Écrasement de la consigne (satellite physique) | `switch` | |
| Association | `button` | « Associer satellite Z\<N\> ». |

### Chaudière

| Entité | Type | Notes |
|---|---|---|
| État chaudière (Chauffage / ECS / Veille / Inconnu) | `text_sensor` | Déduit des indicateurs reçus. |
| Anomalie | `binary_sensor` | Indépendante de l'état. |
| Températures ECS, corps de chauffe, fumées, extérieure | `sensor` | Optionnelles selon modèle. |
| Pression | `sensor` | |
| Puissance instantanée ECS / chauffage | `sensor` | Souvent à 0 selon le modèle. |
| Consommation (veille) ECS / chauffage ; **cumul de l'année** | `sensor` (`energy`) | Le cumul annuel alimente le tableau de bord Énergie. |
| Mode ECS (Max, Eco, Eco Horaires, Eco+, Eco+ Horaires, Stop) | `select` | |
| Association Connect, sonde, récupération du NetworkID | `button` | |

### Diagnostic radio (nouveau, utile pour les réglages de réception)

`sensor` : RSSI de la dernière trame, trames valides / réparées / rejetées, échecs de lecture consécutifs, âge de la
dernière lecture réussie ; `binary_sensor` : « Chaudière joignable ». Réglages à chaud (catégorie *config*) : bande RX et
bypass du LNA avec redémarrage de la radio sans reflasher.

## 6. Programmation hebdomadaire **[DÉCISION D1]**

Contrainte : en émulation Connect, le bloc de zone envoyé à la chaudière contient les **42 octets** du programme
(7 jours × 48 demi-heures, dimanche en premier, bit 1 = Confort). Pour un **satellite virtuel**, le mode *Auto* doit
être calculé localement pour que la chaudière reçoive le bon régime **même si HA est arrêté**.

| Option | Principe | + | − |
|---|---|---|---|
| **A. Appareil propriétaire** (parité) | Programme stocké dans l'appareil ; action `set_program(zone, jours[])` ; capteur d'état. | Autonome, identique à l'existant, Auto fonctionne sans HA. | Il faut un éditeur côté HA. |
| **B. HA propriétaire** | Un helper `schedule` HA bascule Confort/Réduit. L'appareil ne connaît pas de programme. | Éditeur natif HA, rien à développer. | Si HA tombe, le dernier mode reste (risque de rester en Confort) ; pas d'Auto autonome ; l'Auto du Connect perd son sens. |
| **C. Hybride (recommandé)** | Appareil = stockage + application + réécriture du bloc (option A). HA = **éditeur** : un blueprint convertit un helper `schedule` en appel `set_program`. | Autonomie + éditeur natif ; pas de page web à maintenir. | Un blueprint/package HA à écrire et tester. |

Dérogations (fin au prochain changement Confort/Réduit, échéance conservée au redémarrage) : **dans l'appareil**,
car elles conditionnent ce que reçoit la chaudière.

## 7. Table de parité : où va chaque fonctionnalité

| Fonctionnalité du firmware MQTT | Dans l'appareil (ESPHome) | Dans HA | Remplacée / abandonnée |
|---|---|---|---|
| Émulation Connect (actif) | ✅ | | |
| Mode Connect passif (écoute seule) | ✅ | | |
| Zones 1-3 : températures, consignes, modes, boost | ✅ | | |
| Sélection du mode ECS | ✅ | | |
| Consommation gaz (veille + cumul année) | ✅ lecture | Tableau de bord Énergie | |
| Satellite physique (écoute, écrasement de consigne) | ✅ | | |
| Satellite virtuel (ambiante + consigne + Auto) | ✅ | Capteur source (`homeassistant`) | |
| Sonde extérieure réelle (DS18B20) | ✅ via `dallas_temp` ESPHome | | Code `DS18B20` maison supprimé |
| Sonde extérieure virtuelle | ✅ émission radio | Entité météo lue par `platform: homeassistant` | Automatisation MQTT supprimée |
| Association Connect / sonde / satellites | ✅ boutons | | |
| Récupération du NetworkID | ✅ bouton | | |
| Programmation hebdomadaire | ✅ stockage/application | **Éditeur** (blueprint, option C) | Page `/programming` supprimée |
| Dérogations locales | ✅ | | |
| Découverte Home Assistant | ✅ native | | MQTT Discovery supprimé |
| Configuration WiFi / MQTT | | | `wifi:` / `api:` ESPHome ; portail supprimé |
| Mot de passe du portail | | | Clé de chiffrement API + mot de passe OTA |
| Mise à jour OTA | | | `ota:` ESPHome |
| Journaux | | | `logger:` ESPHome (`esphome logs`, HA) |
| Journal radio, trame personnalisée (debug) | ✅ actions `send_raw`, événement `frisquet_frame` (si `developer: true`) | Logbook/outils dev | Page `radio-logs` supprimée |
| Lecture de zones mémoire (debug) | ✅ action `read_memory(addr, len)` + événement | | Page `memory` supprimée |
| Réglages radio (bande RX, bypass LNA) | ✅ YAML + `select`/`switch` à chaud | | |
| Informations système / réseau | | ESPHome (diagnostic standard) | |
| Mode développeur | ✅ `developer: true` | | |
| Compteur de version | | ESPHome (`project:`) | `tools/version.py` supprimé |
| Cartes : Heltec V3, V4.3 ; LilyGo V2.1 (SX1276) | ✅ packages YAML | | |

## 8. Dérivés laissés à HA

Template sensors (régime effectif, « chauffe en cours » combiné), helpers de boost temporaires, notifications
(échec de lecture prolongé, pression basse, anomalie), tableaux de bord, statistiques et tableau Énergie.

## 9. Arborescence du dépôt

```
openfrisquetvisio-esphome/
├─ components/frisquet/          # composant externe ESPHome
│  ├─ __init__.py  climate.py  sensor.py  select.py  number.py  button.py  …   (codegen Python)
│  ├─ core/                      # C++ sans ESPHome : protocole, émulateurs, ordonnanceur
│  ├─ radio/                     # pilote RadioLib + FEM
│  └─ glue/                      # tâche radio, files, entités ESPHome
├─ packages/                     # heltec_v4_3.yaml · heltec_v3.yaml · lilygo_v21.yaml
├─ examples/                     # configurations complètes (Connect actif, passif, satellites, …)
├─ ha/                           # blueprint programmation, exemples de dashboards/templates
├─ tests/
│  ├─ host/                      # tests C++ du cœur (CRC, codecs, ordonnanceur, machines à états)
│  ├─ fixtures/                  # trames réelles capturées (voir ci-dessous)
│  └─ config/                    # `esphome config` / compilation de chaque exemple
├─ docs/  DESIGN.md  PROTOCOL.md  MIGRATION.md
└─ .github/workflows/            # CI : tests hôte + compilation des exemples
```

## 10. Stratégie de test et feuille de route

**Tests**

- **Hôte** : le cœur est rejoué avec des **trames réelles** (déjà disponibles dans les journaux RADIO : requêtes `79F2`,
  `7A18`, `A063`, `A0F0`, trames d'écho de zone, trames avec réparation CRC). Résultats attendus = valeurs observées
  (ex. ECS 70,3 °C, pression 1,3 bar).
- **Simulateur de chaudière** (hôte) pour tester associations, échecs, restauration du NetworkID, fenêtres silencieuses.
- **CI** : compilation de chaque exemple pour chaque carte ; validation de la configuration.
- **Matériel** : check-list manuelle par phase.

**Feuille de route (chaque phase est livrable et réversible)**

| Phase | Contenu | Critère de sortie |
|---|---|---|
| **0. Spike (go/no-go)** | ESPHome + RadioLib sur V4.3 ; API active ; réception de trames chaudière ; **une** lecture `79F2` depuis la tâche radio ; décision composant maison vs `sx126x` | Température ECS correcte dans HA, sans coupure API |
| **1. Lecture seule** | Cœur + tests hôte ; écouteur passif ; capteurs chaudière/zones ; diagnostics | Parité des capteurs avec le firmware MQTT, **aucune émission** |
| **2. Connect actif** | Lectures planifiées ; envoi de zone ; mode ECS ; association ; NetworkID | Pilotage d'une zone depuis HA |
| **3. Satellites et sonde** | Satellite physique (écrasement), virtuel, sonde extérieure | Idem, avec capteurs HA |
| **4. Programme et dérogations** | Option retenue en D1 ; blueprint | Auto + dérogations persistants |
| **5. Finition** | Outils dev, migration, CI, autres cartes (V3, LilyGo) | Documentation + premier tag |

## 11. Décisions à valider

| # | Question | Recommandation |
|---|---|---|
| **D1** | Propriété du programme (section 6) | **C. Hybride** |
| **D2** | Composant radio : RadioLib maison ou `sx126x` officiel | **RadioLib maison**, spike pour confirmer |
| **D3** | Cible initiale | **Heltec V4.3 seul** (V3 et LilyGo en phase 5) |
| **D4** | Coexistence avec le firmware MQTT | **Non** : une seule émulation du Connect à la fois. Développer sur une **deuxième carte** si possible, sinon phase 1 (lecture seule) d'abord |
| **D5** | Compatibilité des identifiants d'entités avec l'ancien firmware | Non reproduite à l'identique ; table de correspondance dans `MIGRATION.md` |
| **D6** | **Licence / attribution** : le dépôt amont n'a **aucun fichier LICENSE** (droits réservés par défaut). Réutiliser son code (RadioCrc, adresses mémoire, structures de trames) suppose l'accord de son auteur (FreedomNX). | Contacter l'auteur avant toute publication ; en attendant, dépôt **local/privé** |
| **D7** | Langue des entités et de la documentation | Français (noms d'entités) + anglais pour le code |

## 12. Risques

| Risque | Mitigation |
|---|---|
| RadioLib + API ESPHome + tâche radio : mémoire ou temps réel insuffisants | Spike phase 0 ; mesure du tas libre ; tâche radio sur son propre cœur |
| Protocole rétro-ingénié : régressions silencieuses | Tests par trames réelles ; mode lecture seule avant l'émission |
| Réception marginale | Diagnostics, réglages à chaud (bande, LNA), antenne |
| Dérive des versions ESPHome / Arduino | Version d'ESPHome épinglée en CI |
| Interférence si deux émulateurs (Connect réel / ancien firmware / nouveau) | Documenter, mode passif, D4 |
