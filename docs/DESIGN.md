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
 │   protocole : transactions Ask/Init/Answer, mémoire (0x79E0…0xA0FC)  │
 │   liaison : en-tête, ack, retry, CRC16 + réparation 1 bit            │
 ├──────────────────────────────────────────────────────────────────────┤
 │ radio driver (RadioLib : SX1262 / SX1276, FEM KCT8103L)              │
 └──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Concurrence

**Révisé le 26/10/2026 : pas de tâche FreeRTOS dédiée.** Le moteur Connect (`proto_engine.h`) est une machine à états **non bloquante** :
il reçoit les trames et l'horloge, et rend la prochaine trame à émettre. Les seuls appels bloquants sont l'émission (≈ 25 ms par trame) ;
les attentes de réponse (jusqu'à 22 s dans l'ancien firmware) sont des états du moteur. Tout tourne donc dans la boucle ESPHome,
sans file ni verrou, et les entités ne sont modifiées que là. Une tâche reste possible si la latence des accusés devient un problème.

- Le **cœur** est une machine à états sans E/S, testée sur hôte avec des trames réelles.
- Réception : IRQ → drapeau → lecture de la capture brute, CRC et réparation dans `loop()`.
- Émission : la porte `evaluate_tx()` juge chaque trame, `loop()` attend 30 ms (comme l'ancien firmware) puis émet ; l'interruption
  « émission terminée » est ignorée pendant l'émission.

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
  `7A18`, `A063`, `A0FC`, trames d'écho de zone, trames avec réparation CRC). Résultats attendus = valeurs observées
  (ex. ECS 70,3 °C, pression 1,3 bar).
- **Simulateur de chaudière** (hôte) pour tester associations, échecs, restauration du NetworkID, fenêtres silencieuses.
- **CI** : compilation de chaque exemple pour chaque carte ; validation de la configuration.
- **Matériel** : check-list manuelle par phase.

**Feuille de route (chaque phase est livrable et réversible)**

| Phase | Contenu | Critère de sortie | Émission |
|---|---|---|---|
| **R. Reflash distant** ✅ | Image ESPHome minimale ; OTA dans les deux sens ; `safe_mode` | MQTT→ESPHome→ESPHome→MQTT sans USB (voir 10.1) | non |
| **0. Spike passif** | ESPHome + RadioLib en réception sur V4.3 ; FEM ; mesure du tas libre ; décision composant maison vs `sx126x` | Trames `08 80 FA` décodées dans HA, API stable | non |
| **1. Cœur + lecture seule** | Cœur C++ + tests hôte ; capteurs issus des trames satellite ; diagnostics radio | Ambiante, consigne, mode, date, état corrects | non |
| **2a. Lectures actives** | Identité Connect `0x7E`/`0xE3` ; ordonnanceur ; lectures `79F2`, `7A18`, `A063`, `A0FC`, `A02B` | Parité des **capteurs** avec le firmware MQTT | lectures seules |
| **2b. Écritures bas risque** | Mode ECS ; consignes Confort/Réduit/Hors gel (relecture) | Changement vérifié par relecture | écritures |
| **2c. Zone complète** | Bloc de zone (modes, consignes, programme 42 octets) | Pilotage d'une zone depuis HA | écritures chauffage |
| **2d. Association** | Associations, récupération du NetworkID, restauration garantie du sync word | Association échouée ⇒ radio sur le bon réseau | modifie le réseau |
| **3. Satellites et sonde** | Satellite physique (écrasement), virtuel, sonde extérieure | Idem, avec capteurs HA | oui |
| **4. Programme et dérogations** | Option retenue en D1 ; blueprint | Auto + dérogations persistants | oui |
| **5. Finition** | Outils dev, migration, CI, autres cartes (V3, LilyGo) | Documentation + premier tag | oui |

### 10.1 Résultat de la phase R (25/10/2026)

Carte unique Heltec V4.3, firmware MQTT d'origine, **aucun accès USB** :

1. MQTT → ESPHome 2026.10.0 (image minimale, 827 Ko) par `POST /api/update` du portail : OK, même IP, API sur 6053.
2. ESPHome → ESPHome par `esphome upload` : OK (168 s à −72 dBm ; prévoir des OTA lentes).
3. ESPHome → firmware MQTT (`esphome upload --file`) : OK ; configuration et programme **identiques** après retour, radio opérationnelle.

Constats : l'OTA ne change ni bootloader ni partitions (conservés : slots de 6,5 Mo) ; le bootloader actuel ne gère pas le
« rollback OTA » d'ESP-IDF (avertissement ESPHome), mais `safe_mode` est actif. **Risque résiduel** : une image qui échoue très tôt
au démarrage, avant `safe_mode`, ne se récupère pas à distance. Chaque nouvelle image reste donc de la **même famille** que l'image
déjà prouvée (même carte, mêmes options flash) et la radio est activée progressivement.

### 10.3 Constat du 26/10/2026 : une écoute seule ne peut pas durer

Après ~11 h avec l'image d'écoute seule (Connect absent depuis 22:00), la chaudière affichait **« anomalie avec Connect »** :
l'octet d'état de la diffusion avait le bit 0 (anomalie) à 1 (`0x29`), alors que la chaudière chauffait toujours. Elle relayait
alors toutes les ~4 s le bloc de configuration de la zone 1 vers le Connect (`0x7E`) sans recevoir d'accusé. Le firmware MQTT,
rétabli par OTA, a de nouveau accusé ces relais et l'anomalie a disparu (vérifié sur le tableau de bord).

Conséquences :

- l'écoute seule n'est utilisable que par **fenêtres courtes** (l'anomalie n'était pas présente après 40 minutes ; le seuil est inconnu) ;
- le **minimum viable d'un Connect** est d'accuser les relais de zone (réponse en écho du bloc avec réception `0x88`),
  pas seulement de lire. Cette réponse est avancée en tête de la phase 2c et testée sur les trames réelles avant tout envoi ;
- tout essai ESPHome sur la carte unique est précédé d'un plan de retour (OTA du firmware MQTT, prouvé en 12 s) ;
- les diagnostics « octets bruts » et « dernier bloc de configuration » sont conservés pour analyser les modes dans l'historique HA.

### 10.4 Fenêtre dry-run du 26/10/2026 (12:03 – 12:27, carte unique, `tx: ack`, `dry_run: true`)

- **Aucune trame émise** : 12 lectures et 10 accusés journalisés `TX DRY_RUN`, aucun `BLOCKED`, aucun avertissement.
- **Lectures** : les 4 lectures de démarrage (`79F2`, `7A18`, `A063`, `A0FC`) après la fenêtre silencieuse de 150 s, puis les créneaux
  horaires. Mêmes trames que l'ancien firmware (`80 7E E3 <id> 01 03 79 F2 00 1C`).
- **Accusés** : 10 relais de zone (3 à chaque changement du satellite : Réduit, puis Auto) reçus ; chacun a produit exactement un accusé,
  l'écho du bloc avec réception `0x88`, construit ≈ 12 ms après le relais.
- **Défaut trouvé et corrigé** : au passage de l'horloge « uptime » à la date de la chaudière, les créneaux `:03` et `:06` suivants étaient
  sautés (la garde d'une tentative par créneau gardait l'ancienne base de temps). Les gardes sont réinitialisées à la première synchronisation ;
  un test hôte reproduit le défaut (il échoue sans le correctif).
- **Retour** : firmware MQTT rétabli à 12:27 par OTA ; aucune anomalie (état « Chauffage », mode Auto) à 12:29.
- **Pas encore comparé** : un accusé réel de l'ancien firmware pour un relais du même moment (aucun relais ne s'est produit après le retour).
  L'égalité octet par octet est vérifiée sur la paire réelle du 25/10 (test hôte).

### 10.5 Fenêtre de lectures réelles du 26/10/2026 (12:35 – 12:51, `tx: read`, `dry_run: false`)

**Première émission réelle du firmware ESPHome** : 8 lectures émises, 8 réponses de la chaudière (≈ 130 ms), 0 échec, 0 trame bloquée.

- Lectures de démarrage (`79F2`, `7A18`, `A063`, `A0FC`) à 12:37:57, après la fenêtre silencieuse, toutes répondues du premier coup.
- Créneaux horaires de la chaudière ensuite : 12:42:58, 12:45:58 et 12:47:58 (`:03`, `:06`, `:08` à ≈ 2 s près de l'horloge de la chaudière),
  ce qui confirme le correctif de réinitialisation des gardes.
- Valeurs identiques à celles de l'ancien firmware relevées avant et après la fenêtre : ECS, corps de chauffe, pression, départ, consigne de départ,
  mode ECS (Eco Horaires), consigne et ambiante de la zone 1 (après le retour, l'ancien firmware lisait exactement 26,5 / 76,2 / 1,42 / 56,5 / 19,8
  que la dernière lecture ESPHome). La fumée à 108,1 °C pendant un chauffage ECS confirme que ce capteur est alimenté brûleur allumé.
- 3 trames réparées par le CRC logiciel pendant la fenêtre (réparation en situation réelle).
- Mémoire stable (264 Ko libres), boucle ≈ 22 ms, aucune anomalie chaudière avant, pendant et après (retour du firmware MQTT à 12:50).
- Les accusés de relais n'étaient pas autorisés (niveau `read`) et aucun relais ne s'est produit pendant la fenêtre.

### 10.6 Fenêtre de lectures et d'accusés réels du 26/10/2026 (12:57 – 13:20, `tx: ack`, `dry_run: false`)

Le firmware ESPHome a tenu seul le rôle minimal d'un Connect pendant ~24 minutes (lectures + accusés de relais, aucune écriture) :

- **2 relais de zone, 2 accusés émis** (≈ 10 à 16 ms après chaque relais), **2 confirmations du satellite** (`08 80 FA <id> FE …`), exactement la séquence
  de l'ancien firmware. Un seul relais par changement : plus de répétitions toutes les ~4 s comme sans accusé.
- Changements provoqués : Réduit permanent (bloc mode `07`, options `00`), puis retour en Auto (mode `05`, options `21`).
- **14 lectures émises, 14 réponses, 0 échec, 0 trame bloquée**, aucun avertissement hormis celui, voulu, de la transmission réelle.
- Programme hebdomadaire inchangé avant/après ; anomalie chaudière absente avant, pendant et après (retour du firmware MQTT à 13:20).

Reste à faire pour que le firmware ESPHome remplace le firmware MQTT : les **écritures** (mode ECS, consignes, bloc de zone, programme),
l'association, les satellites virtuels et la sonde extérieure, et la persistance des identifiants (association, NetworkID, id de message).

### 10.7 Fenêtre d'écriture du mode ECS du 26/10/2026 (14:47 – 14:58, `tx: write`, `dry_run: false`)

Première **écriture** émise par le firmware ESPHome, depuis le sélecteur « Chaudière mode ECS » :

- **Eco Horaires → Eco** (14:52:28) : trame émise `80 7E E3 EE 01 17 A0 FC 00 01 A0 FC 00 01 02 00 88`, accusé de la chaudière 250 ms plus tard
  (`7E 80 E3 EE 81 17 02 00 88`, identique au format capturé), relecture de `A0FC` = `00 88` → état **verified** à 14:52:29.
  L'écriture a attendu la fin de la fenêtre silencieuse (demandée à 14:50:58, émise à 14:52:28).
- **Retour à Eco Horaires** (≈ 14:55) : sélecteur et relecture à « Eco Horaires », état « verified » ; la chaudière vue par le firmware MQTT
  après le retour : mode ECS « Eco Horaires », aucune anomalie.
- 9 lectures réussies, 0 échec, 0 trame bloquée, aucun avertissement hormis celui de la transmission réelle.
- La seconde écriture est antérieure au début de la capture des journaux : elle est établie par l'état final (sélecteur, relecture, tableau de bord MQTT).

### 10.8 Écriture des consignes de zone (construction et tests hôte)

- Constructeur `build_zone_write` identique octet pour octet à la trame réelle du 25/10 (fixture `payload_zone_write`) ; la réponse réelle
  (`payload_zone_write_reply`) est reconnue par le moteur, y compris sous l'id d'une rafale précédente.
- Porte : trame de 63 octets, en-tête exact `A1 54 00 18 A1 54 00 18 30`, consignes 5,0–30,0 °C par pas de 0,5, hors-gel ≤ réduit ≤ confort,
  mode 05–08, et **tous les octets hors du masque `zone_write_mask` identiques au dernier bloc relayé** (masque = consignes seulement tant
  que le mode et le programme ne sont pas validés). Sans bloc connu : aucune émission.
- Moteur : une rafale de 5 émissions, nouvelle rafale (nouvel id) 30 s après la précédente tant que la chaudière n'a pas répondu, abandon
  après 20 minutes (état `failed`) ; hors fenêtre silencieuse seulement ; les lectures planifiées s'intercalent entre les rafales.
  Un relais reçu pendant l'écriture ne remplace pas le bloc connu ; une fois l'écho reçu, l'état est `verified` et le bloc connu est mis à jour.
- Entités : trois `number` (confort, réduit, hors-gel de la zone 1) et le texte d'état `zone_write`.
- Pas encore fait : persistance du bloc (il faut attendre un relais après le démarrage), écriture du mode et du programme, zones 2 et 3.

### 10.9 Fenêtre d'écriture des consignes de zone du 26/10/2026 (15:47 – 16:14, `tx: write`, `dry_run: false`)

Première écriture d'un **bloc de zone** émise par le firmware ESPHome (nombre « Zone 1 consigne confort » 20,5 → 20,0 °C) :

- Le bloc n'est connu qu'après un relais : aucun relais en 6 min de lecture seule, il a fallu une modification sur le satellite (confort 20,5)
  pour le provoquer ; la demande avant cela est refusée (comportement voulu).
- Demande à 16:00:28, mise en attente jusqu'à la fin de la fenêtre silencieuse, première rafale à 16:02:27 (5 émissions à 2,2 s, trame
  identique au format capturé, seul l'octet de confort change : `96`). Deuxième rafale (id suivant) à 16:03:09, **écho de la chaudière à 16:04:36**
  (état `verified`, 2 min 10 après le début), diffusion du satellite à 16:04:37 avec la consigne 20,0 °C, relais de la chaudière accusé à 16:04:40.
- L'octet d'options écrit était `25` ; le satellite le renvoie normalisé en `21` (bit `0x04` retiré) : le bloc connu suit toujours le dernier relais.
- La lecture `79F2` a affiché l'ancienne consigne (20,5) jusqu'à ~2 min après, puis 20,0 : elle est retardée, pas fausse.
- 18 émissions de bloc, 0 trame bloquée. Retour au firmware MQTT à 16:13 : aucune anomalie, programme identique à la sauvegarde.

### 10.10 Fenêtre d'écriture du mode et du programme de la zone 1 du 26/10/2026 (17:26 – 18:27, `tx: write`, `dry_run: false`)

- **Défaut trouvé en direct** : une édition de source perdue (course entre outils) laissait le masque de la politique sur « consignes seules » ;
  la porte a donc refusé (`BLOCKED_ADDRESS`, rien émis) la première demande de mode, ce qui est le comportement voulu. Corrigé, reflashé.
  Rappel : après chaque redémarrage le bloc est inconnu jusqu'au prochain relais (action sur le satellite) ; la persistance reste à faire.
- **Mode** : Auto → Réduit demandé à 17:46:28, accepté par le satellite au créneau `:x4:37` (17:54:37, écho `verified`, satellite « Réduit »).
  Réduit → Auto demandé à 17:55:06, accepté à 18:04:37 (`verified`, satellite « Auto (confort) »). Le satellite n'accepte qu'à son créneau
  (`:x4:37` sur cette installation, toutes les 10 min), les rafales de 40 s en attendant.
- **Programme** : lundi 08:00-21:30 → 08:00-22:00 demandé à 18:08:49 ; l'utilisateur a constaté sur le satellite « 22:00 » : écriture acceptée.
  Non observé par les journaux : le flux de logs s'est arrêté à 18:10 et la carte a redémarré vers 18:25 (« Reboot request from api », sans
  commande de notre part ; à surveiller). Retour au firmware MQTT à 18:26 : aucune anomalie, ECS inchangé.
  Explication probable (27/10) : `api:` sans `reboot_timeout`, donc le défaut d'ESPHome (redémarrage après 15 min sans client API) ;
  la perte du flux à 18:10 et le redémarrage à ~18:25 sont espacés de 15 min, et la source enregistrée est bien `api`.
- Le firmware MQTT ne connaît pas ce nouveau programme avant un prochain relais.

### 10.11 Persistance des blocs de zone (construction et tests hôte, 27/10/2026)

La chaudière refuse la lecture du bloc `0xA154` : le Connect garde donc le dernier bloc reçu, désormais sauvegardé dans les préférences
ESPHome (`proto_zone_store.h`, enregistrement `FZB1` v1 : masque des zones connues, 3 blocs, CRC32 ; tout enregistrement invalide est ignoré).

- Trois états par zone, publiés pour la zone 1 par le capteur texte `zone_block_state` : **inconnu**, **restauré (non confirmé)**, **confirmé**.
- Au démarrage, un bloc restauré est affiché mais **n'autorise aucune écriture** (porte et demandes refusées tant qu'il n'est pas confirmé).
- Confirmation : un relais de la chaudière, l'écho d'une écriture, ou la diffusion du satellite (toutes les 10 min) cohérente avec le bloc
  (mode, consigne active, horloge). Une diffusion incohérente **efface** la copie (retour à « inconnu »). La vérification n'est que partielle :
  une modification du programme ou d'une consigne inactive peut passer inaperçue.
- Sauvegarde seulement quand le bloc change (relais nouveau, écriture vérifiée, copie effacée), pas à chaque répétition d'un relais.
- Pas encore fait : zones 2 et 3 (voir 10.13).

### 10.12 Fenêtre de validation de la persistance du 27/10/2026 (10:57 – 11:10, `tx: write`, `dry_run: false`, aucune écriture)

- 10:57 : MQTT → ESPHome `0.6.0-phase-2c-zone` par `/api/update` du portail. Premier démarrage : bloc « inconnu » (rien de sauvegardé).
- 11:02:42 : relais provoqué par l'utilisateur (confort 20,5 → 20,0 sur le satellite) : accusé envoyé, bloc « confirmé », **une seule**
  sauvegarde (`mask 1`) malgré les répétitions du relais.
- 11:03:17 : redémarrage par OTA ESPHome de la même image. Au démarrage : « restauré (non confirmé) », consignes 20,0 / 17,0 / 8,0 affichées
  aussitôt (auparavant inconnues jusqu'au relais suivant).
- 11:10:09 : diffusion `08 80 FA` du satellite cohérente ⇒ « confirmé », sans nouvelle sauvegarde. Diffusions reçues à 11:00:09, 11:02:39
  puis 11:10:09 seulement (RSSI ≈ −100 dBm) : il faut compter jusqu'à ~10 min avant la confirmation.
- 11:10:41 : retour au firmware MQTT (`b1e6934`, recompilé pour l'occasion) : portail, MQTT, ECS « Eco Horaires », zone 1 Auto 20,0, aucune anomalie.

### 10.13 Fenêtre d'écriture depuis un bloc restauré du 27/10/2026 (11:32 – 11:47, `tx: write`, `dry_run: false`)

- 11:32 : MQTT → ESPHome. Le bloc sauvegardé à 11:02 a **survécu à 20 min de firmware MQTT** : « restauré (non confirmé) » dès le démarrage.
- 11:32:52 : demande confort 20,5 alors que le bloc n'est que restauré : **refusée**, rien émis, le nombre revient à 20,0.
- 11:40:09 : diffusion du satellite cohérente ⇒ « confirmé ».
- 11:40:23 : confort 20,0 → 20,5 ; rafales dès 11:43:14 (seul l'octet de confort change, `96` → `9B`), écho à 11:44:35 (créneau `:x4:37`),
  état `verified`, bloc sauvegardé.
- 11:44:53 : confort 20,5 → 20,0 ; la chaudière transmet aussitôt le bloc au satellite (`08 80 FA … A1 54 … 30 96`) et l'écho arrive à
  11:47:00, **hors du créneau `:x4:37`** : le satellite n'attend donc pas toujours son créneau (à confirmer sur d'autres essais).
- 35 trames d'écriture, 0 bloquée. Retour au firmware MQTT à 11:47 : MQTT, ECS « Eco Horaires », zone 1 Auto 20,0, aucune anomalie.
- L'installation n'a que la zone 1 : les zones 2 et 3 ne peuvent pas être validées sur l'air.

### 10.14 Association et récupération du NetworkID (construction et tests hôte, 27/10/2026)

- `proto_pairing.h` : décodage de l'offre `0x41`, construction de la confirmation (octet pour octet celle du firmware MQTT),
  session (`écoute` → `confirmation` → `réussie` / `échouée` / `annulée`) et enregistrement `FNW1` (NetworkID, association, CRC32).
- **Le mot de synchronisation est dicté par la session** : diffusion seulement pendant l'écoute et la confirmation, le réseau de
  l'installation sinon. La radio compare à chaque boucle ce mot avec celui qu'elle applique : toute sortie (délai de 30 s, 5 s pour
  confirmer, annulation, arrêt d'urgence, dry-run, refus de la porte, échec de `setSyncWord` réessayé) la ramène sur un vrai réseau.
  Test hôte de la régression du 25/10, y compris au passage de `millis()` par zéro.
- Pendant une session, aucune trame de l'installation n'est émise (lectures, accusés, écritures suspendus ; la trame en attente est
  abandonnée) et les trames entendues ne sont données qu'à la session. Après la session, l'émission et le traitement des trames
  restent bloqués tant que le mot de synchronisation **réellement appliqué** n'est pas celui de l'installation (cas d'un
  `setSyncWord` en échec, signalé en revue de code).
- Porte : classe `association-confirm`, refusée sauf si c'est **exactement** la confirmation de l'offre reçue et que `pairing: true` ;
  en dry-run ou refusée, la session est annulée (rien ne change de notre côté puisque la chaudière n'a rien reçu).
- Réussite : NetworkID (et, pour une association, octet d'association) sauvegardés **et écrits en flash aussitôt** (`sync()`), puis
  utilisés au démarrage à la place du YAML ;
  bouton « oublier le réseau appris » pour revenir au YAML. Capteurs `network` et `pairing_state`.
- Configuration `dev/phase-2d-pairing.yaml` (`pairing: false`) : seule la récupération (écoute) peut agir.
- Capture d'une offre réelle et association sur l'air : faites le 28/10 (voir 10.16). Comme prévu, l'association a changé l'octet
  du Connect (`E3` → `D5`) : le firmware MQTT doit être réassocié pour redevenir le Connect.

### 10.15 Fenêtre de récupération du NetworkID sans mode association du 27/10/2026 (12:06 – 12:10, `tx: write`, `pairing: false`)

- Démarrage sur `05:D9:8D:C9 (YAML), association E3` ; bloc de zone restauré comme en 10.13.
- 12:09:02 : bouton « récupérer le NetworkID », chaudière **pas** en mode association : passage sur `FF:FF:FF:FF`, **rien émis**
  pendant 30 s (une lecture `A063` partie à 12:09:02.03 attendait sa réponse ; seul un bruit `INVALID_LENGTH` reçu).
- 12:09:32 : `failed`, retour sur `05:D9:8D:C9` ; la lecture interrompue est renvoyée (même id) 20 ms après et répondue aussitôt,
  lectures 5/5, 0 trame bloquée. C'est le chemin du bug du 25/10, vérifié sur la carte.
- Bouton « oublier » sans réseau appris : reste sur le YAML. Retour au firmware MQTT à 12:10 : aucune anomalie.
- Reste : une offre réelle (chaudière en mode association) en récupération seule, puis, éventuellement, l'association.

### 10.16 Fenêtre d'association du Connect du 28/10/2026 (22:21 – 22:35)

- 22:21 : MQTT → ESPHome `0.7.0-phase-2d-pairing` (`pairing: false`). Zone 1 était passée en Hors Gel 8,0 depuis la veille (côté
  satellite).
- 22:22:49 et 22:24:50 : deux récupérations du NetworkID (écoute 30 s) pendant « Partenaires > Ajouter > Connect » : la chaudière
  répond aussitôt « terminé », **rien reçu** sur `FF:FF:FF:FF`, retour sur `05:D9:8D:C9` à chaque fois ; l'association `E3` reste
  valide (lectures 4/4 à 22:24). Cause : un Connect était déjà associé.
- 22:28 : `0.7.1-phase-2d-associate` (`pairing: true`). L'utilisateur efface le Connect sur la chaudière puis relance « Ajouter ».
- 22:29:31 : bouton « associer le Connect ». **22:29:38 : offre reçue** `00 80 D5 D4 02 41 04 05 D9 8D C9` (−99,5 dBm), 10 confirmations
  `80 7E D5 D4 82 41 05 D9 8D C9` en 0,36 s (porte : `SEND`), réseau appris `05:D9:8D:C9` (identique), **association `D5`** sauvegardée.
  La chaudière affiche OK. 22:30:04 : lectures 4/4 avec `D5`.
- 22:33 : `0.7.2-phase-2d-associate` (identique + `api: reboot_timeout: 0s`, pour fonctionner sans client API) ; après le
  redémarrage, réseau `appris` et association `D5` relus de la flash.
- **Conséquence** : le firmware MQTT (`E3`) n'est plus le Connect de la chaudière ; y revenir exige de le réassocier
  (« Effacer » puis « Ajouter » Connect, bouton « Associer Connect » du portail). La carte reste sous ESPHome.

### 10.17 Mode Hors gel affiché « Auto » (28/10/2026 22:54 – 23:00)

- Signalé depuis HA : satellite en Hors gel, « Zone 1 mode (commande) » en Auto, « Zone 1 mode » « Inconnu ».
- Causes : la diffusion porte `14` (hors gel + bit `0x04`, sans `0x20`), que le décodeur ne reconnaissait qu'en `10` exact ; la
  vérification du bloc restauré (sauvegardé en Auto le 27/10) restait donc « non concluante » et le sélecteur montrait ce bloc périmé.
- Corrections (`0.7.3`) : tout octet sans `0x20` portant `0x10` est Hors gel ; le sélecteur suit le bloc seulement s'il est
  **confirmé**, sinon le mode diffusé par le satellite (`select_mode_from_broadcast`). Tests hôte (692 vérifications).
- Vérifié à 23:00:08 : bloc périmé écarté (« inconnu »), « Zone 1 mode » = Hors gel, sélecteur = Hors Gel. Les écritures de zone
  restent refusées jusqu'au prochain relais (une modification sur le satellite le provoque).

### 10.18 Brûleur et lectures chaque minute (29/10/2026 11:06 – 11:36)

- Question : savoir quand le brûleur chauffe. Aucune trame spontanée : la flamme n'est qu'un témoin de la chaudière (le satellite
  n'en affiche pas). Zone 1 repassée en Auto sur le satellite à 11:07 : échange satellite immédiat (hors cycle), bloc « confirmé ».
- Mesures : fumées 0 °C à 11:07, 11:12, 11:15, puis **121,8 °C à 11:17:56** (départ 40,2 → 60,4 °C) ; octet d'état `0x20` à 11:07 et
  11:10, **`0x28`** à 11:24:36. Les fumées sont l'indicateur du brûleur ; l'octet d'état le confirme mais toutes les 10 min seulement.
- Retour en Hors gel depuis HA (`select`) : demandé 11:12:33, **vérifié à 11:24:35** (créneau `:x4:37`, celui de 11:14:37 n'a pas
  pris) ; options normalisées en `10` par le satellite.
- `0.7.4` : lecture `0x79F2` **chaque minute de :03 à :09** (7 par cycle de 10 min au lieu de 3 ; fenêtres satellites inchangées,
  6 min sans lecture au plus) ; capteur binaire `burner` (« Chaudière brûleur », classe `heat`) = fumées > 40 °C. Vérifié :
  lectures 8/8 à la minute, brûleur OFF en Hors gel. Tests hôte 704 vérifications.

### 10.19 Exploration de la mémoire pour un état du brûleur en direct (29/10/2026 15:04 – 16:05)

- `0x79F2` n'est **pas** en direct : sur 158 lectures faites chaque minute, la réponse ne change qu'à :x3 (ambiante et consigne de
  zone) et :x8 (températures de la chaudière, fumées). Un puisage d'eau chaude à 14:53 (flamme notée par l'utilisateur) tombe entre
  deux relevés et passe inaperçu (fumées 0 °C ; seule la hausse du corps de chauffe et du départ à :x8 en garde la trace).
  `0x79F2` revient donc à :03 et :08.
- `0xA029` (21 mots, ce que lit le satellite) est **en direct** (horloge à la seconde) et lu chaque minute de :03 à :09 ; mais
  son octet d'état reste `20` pendant une flamme d'eau chaude (15:23 – 15:24:30) : le bit `0x08` suit la demande de chauffage des
  zones, pas le brûleur. Aucun autre octet ne bouge.
- Outil (`0.7.6`) : option `explore: true`, entrée texte `type: explore` (« ADDR MOTS »), porte limitée à 0x1C mots et à cette
  option, script `dev/ofv_scan.py` (résultats dans `build/logs/scan-*.jsonl`).
- Balayage flamme éteinte : répondent `791C` (`FA00 FA08 D57E FFFF…`, table d'associations ?), `7938` (configuration ?), `79E0`,
  `79FC`, `7A18`, `7A34` (`FFFF…`), `A038`, `A054`, `A070` ; rien dans `9C00 – 9CFF`. **Un bloc partiellement invalide est
  entièrement refusé** (`A01C` et `A0FC` muets en 0x1C mots) : la cartographie doit se faire plus finement.
- Flamme d'eau chaude 15:44:58 – 15:46:31 avec lecture toutes les ~11 s : `79E0` (instantané pris vers :x2) et `79FC` (vers :x8)
  ne bougent pas pendant la flamme ; `79FC` change à 15:47:39 (après), fumées toujours 0.
- Reste : réponse « pas de réponse » plus rapide pour l'exploration (1 essai, ~2 s) et découpage automatique des blocs refusés,
  puis comparaison flamme éteinte / allumée. Le capteur « Chaudière brûleur » n'est fiable qu'à l'instant du relevé de :x8.

### 10.20 Capture complète en fonctionnement normal (29/10/2026 16:13 – 16:50)

- Outil `dev/ofv_frames.py` : inventaire de toutes les trames des journaux (classe, nombre, heures, RSSI, exemple). Résultat consigné
  dans PROTOCOL.md (« Inventaire des trames de la chaudière »).
- Séance : ECS Eco Horaires → Eco → Eco Horaires (vérifiés en ~1 s chacun) ; zone 1 Hors gel → Auto (16:14:34) → Confort (16:29:22) →
  Réduit (16:34:34) → **Hors gel (16:44:34)**, état final demandé. Séquence complète d'une écriture de zone : transmission au
  satellite (réc `7E`), écho (`88`), réponse d'état au satellite (`81`), relais du bloc normalisé (`08`), relais de notre accusé (`FE`).
- **Correction** : la diffusion d'un satellite en **Confort permanent vaut `05`** (lu « Auto (confort) » jusque-là) ; le bit `0x04` est
  ignoré dans les deux familles (`0.7.7`, tests 731 vérifications). Vérifié après redémarrage : bloc confirmé, Hors gel affiché.
- Découvertes : la chaudière **refuse une lecture invalide par une trame de type `0x83`** (7 octets) au lieu de se taire ; le RSSI
  « −100 dBm » des trames courtes est un artefact de la capture à 68 octets ; les trames émises **par le satellite** ne sont jamais
  reçues (cause inconnue).

### 10.21 Consignes confort, réduit et hors-gel (29/10/2026 16:53 – 17:34)

- Script `dev/ofv_setpoints.sh` : écritures en chaîne, chacune vérifiée avant la suivante. Confort 20,0 → 20,5, réduit 17,0 → 17,5,
  hors-gel 8,0 → 8,5, puis retour 8,0 / 17,0 / 20,0 : **6 sur 6 vérifiées** (entre 1 min 40 et 9 min 40 chacune), 228 trames
  d'écriture, 0 bloquée. État final identique à l'état initial (Hors gel, 8,0 °C, bloc confirmé).
- Seul l'octet visé change ; la consigne active diffusée ne suit que le hors-gel (zone en Hors gel) ; le satellite ne relaie son
  bloc au Connect que pour un changement de sa consigne active (voir PROTOCOL.md).

### 10.22 Tests à distance du 29/10/2026 au soir (17:50 – 22:25)

- **Modes ECS** : les 6 modes écrits et relus (Max `80` … Stop `A8`), retour Eco Horaires.
- **Lectures** : `A07B` et `A063` (consommations annuelles) nulles ; `A02B` = horloge en direct (`A029` décalée).
- **Refus `0x83`** (`0.7.8`) : une lecture refusée finit aussitôt (~0,7 s au lieu de 22 s) ; tests hôte.
- **Carte mémoire** : balayage mot à mot de `7800 – 7BFF`, `9C00 – 9DFF`, `A000 – A2FF` (~3 500 lectures) : 5 plages lisibles, 352 mots
  (PROTOCOL.md, « Carte mémoire »). Reste à les relire en boucle pendant une flamme pour trouver un état du brûleur en direct.
- **Dérogations** (`0.7.9`, sélecteur « Dérogation Confort / Réduit ») : Dérogation Réduit appliquée (`26`) ; Dérogation Confort
  ignorée en période de confort malgré l'écho, appliquée en période réduite (`27`, 20,0 °C) ; Auto l'annule.
- **Programme** : dimanche tout confort, samedi tout réduit, jeudi en deux plages : acceptés et suivis par la diffusion ; programme
  d'origine rétabli (19:04:42) puis **Hors gel** (19:14:43), état final identique à l'initial.
- **Réception du satellite** : bande RX portée à 467 kHz (`0.7.9-bw467`) : toujours aucune trame du satellite, ni rejetée ; ce n'est
  pas un décalage de fréquence. Revenu à 234,3 kHz (`0.7.10`). À tester sur place (carte près du satellite).
- **Phase 3** (étude du firmware MQTT) : sonde extérieure = appareil `0x20` à associer sur la chaudière, écrit `A029` toutes les 10 min ;
  satellite virtuel = écritures en `A02F`. Les deux modifient la régulation : rien d'émis sans accord.

### 10.23 Brûleur en direct par la température du corps de chauffe (31/10/2026 10:49 – 11:23)

- La carte a tourné 36 h sans intervention (2 083 lectures, 0 échec) avant la séance.
- Relecture en boucle des 352 mots lisibles (32 passes) pendant un puisage, flamme 10:54:35 – 10:56:30 : seuls bougent les horloges
  (`9C56`, `A02D`, `A047`), un compteur (`7A16`, ~1/s), les deux instantanés (`79E0` à :x2, `79FC` à :x7) et **`A05E`**, la
  température du corps de chauffe **en direct** : −1,5 °C / 14 s pendant le puisage, +2 à +3 °C / 12 s flamme allumée (inertie ~30 s),
  pic 85,3 °C 25 s après l'extinction, puis descente. Aucune donnée « flamme » dans la mémoire lisible.
- `0.7.11` → `0.7.12` : lecture de `A05E` toutes les 15 s hors fenêtres satellite (`policy.live_body_reads`, activée si le capteur
  `burner` ou `heating_body_live` est configuré) ; capteur « Chaudière corps de chauffe (direct) » ; **« Chaudière brûleur » =
  pente** (`BurnerDetector`, `proto_burner.h`) : allumé à +1,0 °C / 15 s, éteint à +0,1 °C ou moins, une baisse après une fenêtre
  satellite l'éteint. Validé sur un second puisage (flamme 11:08:20 → ON à 11:09:02 ; extinction dans la fenêtre satellite → OFF à
  la première lecture, 11:12:40) ; un faux ON (+0,6 °C au repos, pas du capteur) a fait relever le seuil de 0,5 à 1,0.
- L'horloge de la chaudière est apprise à la première réponse `A029` (~3 min après un démarrage) au lieu d'attendre la diffusion
  (jusqu'à 10 min, pendant lesquelles les fenêtres satellite étaient décalées). Tests hôte : 778 vérifications.

### 10.24 Brûleur en chauffage (31/10/2026 11:24 – 12:16)

- Zone 1 : confort 21,5 °C (vérifié 11:34:33), Confort (11:44:33). Demande de chauffage (état `28`) à 11:49 seulement, la chaudière
  affichant « Arrêt chauffage » entre-temps ; flamme continue d'avant 11:55 jusqu'à 12:15:25 (utilisateur).
- **Corps de chauffe `A05E`** : oscille de 41 à 82 °C sur des cycles de 1 à 3 min, flamme allumée (retour des radiateurs) : le
  détecteur à pente bascule ON/OFF toutes les minutes, **faux en chauffage**. En fin de demande (12:14:35, état `20`), il monte encore
  d'≈ 6 °C après l'extinction (12:15:25) : arrêt de la pompe.
- **Fumées** : 122,0 – 123,1 °C sur tous les instantanés pendant la chauffe ; relues toutes les 10 s (`7A02`, `79E6`) pendant 5 min :
  inchangées entre deux instantanés (`79E6` vers `:x2`, `7A02` vers `:x7:38`). Règle de l'utilisateur confirmée : fumées > 0 ⇒ brûleur.
- `0.7.13` : `BurnerFusion` (`proto_burner.h`) : demande de chauffage ⇒ dernier instantané de fumées > 40 °C (lecture de `79E6` à
  `:03` en plus de `79F2`) ; sans demande ⇒ pente (eau chaude). Tests hôte : 800 vérifications.
- Retour : Hors gel vérifié 12:14:41.
- **Conflit avec un réglage local** : pendant notre écriture « confort 20,0 », l'utilisateur a réglé le confort à 19,0 sur le satellite
  (relais `8C` reçu et accusé à 12:22:35). Le moteur ignorait les relais pendant une écriture et aurait écrasé ce réglage :
  arrêt d'urgence à 12:22:44, avant la rafale suivante. `0.7.14` : un relais différent de l'ancien bloc et de notre demande annule
  l'écriture (état `superseded`, **le réglage local l'emporte**) ; un relais équivalent à notre demande (options normalisées) la
  termine. Tests hôte : 809 vérifications. Après le redémarrage, le bloc restauré gardait 21,5 °C (périmé, non vérifiable en
  Hors gel) : resynchronisé en écrivant 19,0 °C.
- **Bouton « info » du satellite** (astuce de frisquet-bridge) : appuyé à ≈ 12:20, le satellite s'est réveillé (échanges à 12:20:07,
  :20, :34) mais pendant notre fenêtre de silence (`:x9:30 – :x2:30`) : aucune rafale envoyée. À faire : émettre l'écriture en attente
  dès qu'on entend une réponse d'état hors créneau (satellite éveillé).

### 10.25 Consigne directe au nom du satellite (31/10/2026 12:28 – 12:42)

- Idée de l'utilisateur : écrire la consigne **directement** à la chaudière (effet immédiat) et, en parallèle, le bloc de zone au
  satellite (qui l'adopte à son réveil).
- `0.7.15` : option `satellite_override` + `satellite_association: 0xFA` ; classe d'émission `satellite-consigne` (source `08`,
  association du satellite, `A029 x15` lu, `A02F x4` écrit : ambiante, consigne, `00`, mode, mot suivant recopié) vérifiée champ par
  champ par la porte ; commande de test `SAT <16 hex>` sur l'entrée exploratoire. Tests hôte : 825 vérifications.
- Essai : page `A029` lue (`00A9 0050 0010 00C6`), envoi de `00A9 0055 0010 00C6` (hors-gel 8,5 °C) à 12:37:46 : réponse de la chaudière
  en 0,12 s et `A029` à **8,5 °C aussitôt**. Échange intermédiaire du satellite à 12:38:41 : inchangé. **Échange régulier de 12:40:06 :
  le satellite remet 8,0 °C.** Une consigne directe tient donc jusqu'au prochain échange régulier (≤ 10 min).
- Double envoi (`0.7.16` → `0.7.17`) : avec `satellite_override`, toute écriture de zone 1 envoie aussi la consigne directe (consigne
  active calculée : mode, dérogation, ou programme à l'heure de la chaudière ; ambiante et mot suivant recopiés de la dernière page) ;
  après chaque échange propre du satellite qui montre une autre consigne, elle est réaffirmée aussitôt (même dans la fenêtre réservée,
  l'échange étant terminé) ; arrêt dès que l'écriture du bloc est finie (vérifiée, échouée ou supplantée). Tests hôte : 846.
- **Défaut trouvé** : la réponse à notre consigne directe (12:37:46, page à 8,5 °C) a été prise pour l'échange du satellite et a fait
  effacer le bloc restauré (hors-gel 8,0 °C, jugé périmé). `0.7.17` : nos propres réponses ne servent plus à vérifier le bloc. Le bloc
  reste inconnu jusqu'au prochain changement fait sur le satellite ; essai du double envoi à faire ensuite.

### 10.26 Double envoi sur la chaudière (01/11/2026 12:25 – 12:35)

- La carte a tourné 23 h seule (5 315 lectures, 0 échec). Bloc réappris par un aller-retour sur le satellite (confort 19 → 19,5 → 19).
- **Hors-gel 8,5 °C depuis HA** (12:26:08) : consigne directe **appliquée par la chaudière à 12:26:09** (0,13 s, `A029` et HA à 8,5) ;
  bloc accepté par le satellite à 12:26:55 (47 s : encore éveillé après le réglage local), relais `08` à 12:27:00 accusé ; l'échange
  régulier de 12:30:06 garde 8,5 °C (le satellite l'a adopté). **Le satellite affiche 8,5 °C** (utilisateur).
- **Retour à 8,0 °C** (12:31:13, dans la fenêtre réservée) : consigne directe seulement à 12:32:24 (fin de fenêtre), bloc accepté au
  créneau `:x4:37` (12:34:33). `0.7.18` : la consigne directe part aussi dans la fenêtre réservée, sauf 40 s autour de l'échange du
  satellite de la zone 1 (`:x9:48 – :x0:28`) ; journal « satellite-consigne » au lieu de « ? ». Tests hôte : 853 vérifications.
- Bilan : effet immédiat sur la chaudière, satellite mis à jour entre 1 et 10 minutes plus tard ; l'échange régulier ne réécrit pas
  forcément la consigne (le 31/10 il l'avait remise, le satellite n'ayant pas encore le bloc).
- Bouton « info » : il provoque un échange d'état du satellite (réponse de la chaudière entendue, « Chaudière horloge » mise à jour),
  mais une rafale envoyée aussitôt après (12:33:53) n'a abouti qu'au créneau `:x4:37` (12:34:31) : le satellite n'écoute pas les
  relais à ce moment-là, ou pas assez longtemps.

### 10.2 Garde-fous d'émission (phases 2a à 5)

1. Niveaux cumulatifs d'émission : `none` < `read` < `ack` < `write`. `none` ne compile **aucun code d'émission du projet** (le build ne contient ni `transmit_frame` ni le moteur ; vérifié sur l'ELF ; la méthode virtuelle `transmit` de RadioLib reste liée mais n'est jamais appelée) (phases R à 1) ;
   `read` n'autorise que les lectures de la liste blanche ; `ack` ajoute l'accusé des relais de zone (écho exact du bloc relayé, vérifié
   octet par octet avant envoi) ; `write` est réservé aux écritures de la liste blanche (mode ECS ; bloc de zone limité aux champs du masque).
   La porte `evaluate_tx()` (`proto_txgate.h`) refuse tout ce qui n'est pas reconnu : identité (émetteur, association), liste d'adresses,
   taille, interrupteur d'arrêt, niveau, dry-run.
2. **Liste blanche** d'adresses par palier (2a : lectures ; 2b : `A0FC` ; 2c : bloc de zone).
3. **Dry-run** : journalise la trame qui serait envoyée, à comparer aux trames du firmware MQTT avant activation.
4. **Interrupteur d'arrêt d'urgence** dans HA, forcé à l'arrêt après un redémarrage anormal.
5. Aucune écriture si l'heure ou le programme sont inconnus ; une tentative par créneau ; pas de rattrapage en rafale.
6. Restauration du NetworkID / sync word par garde RAII, avec test hôte (régression du bug corrigé le 25/10).

## 11. Décisions

| # | Question | Décision (validée le 25/10/2026) |
|---|---|---|
| **D1** | Propriété du programme (section 6) | **C. Hybride** |
| **D2** | Composant radio : RadioLib maison ou `sx126x` officiel | **RadioLib maison** ; le spike confirme |
| **D3** | Cible initiale | **Heltec V4.3 seul** (V3 et LilyGo en phase 5) |
| **D4** | Matériel | **Une seule carte**, **reflash 100 % à distance** (pas d'USB). Lecture seule d'abord ; les lectures actives (2a) sont avancées juste après le spike passif. Pas de coexistence : un seul émulateur du Connect à la fois |
| **D5** | Identifiants d'entités | Non reproduits à l'identique ; table de correspondance dans `MIGRATION.md` |
| **D6** | **Licence / attribution** : le dépôt amont n'a **aucun fichier LICENSE**. Réutiliser son code (CRC, adresses mémoire, structures de trames) suppose l'accord de FreedomNX | Dépôt **privé** tant que ce point n'est pas clarifié ; aucune publication |
| **D7** | Langue | Français (entités, docs) + anglais (code) |

## 12. Risques

| Risque | Mitigation |
|---|---|
| RadioLib + API ESPHome + tâche radio : mémoire ou temps réel insuffisants | Spike phase 0 ; mesure du tas libre ; tâche radio sur son propre cœur |
| Protocole rétro-ingénié : régressions silencieuses | Tests par trames réelles ; mode lecture seule avant l'émission |
| Réception marginale | Diagnostics, réglages à chaud (bande, LNA), antenne |
| Dérive des versions ESPHome / Arduino | Version d'ESPHome épinglée en CI |
| Interférence si deux émulateurs (Connect réel / ancien firmware / nouveau) | Documenter, mode passif, D4 |
