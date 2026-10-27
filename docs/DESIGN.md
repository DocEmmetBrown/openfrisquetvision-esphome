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
- Pas encore fait : écriture depuis un bloc restauré puis confirmé, zones 2 et 3.

### 10.12 Fenêtre de validation de la persistance du 27/10/2026 (10:57 – 11:10, `tx: write`, `dry_run: false`, aucune écriture)

- 10:57 : MQTT → ESPHome `0.6.0-phase-2c-zone` par `/api/update` du portail. Premier démarrage : bloc « inconnu » (rien de sauvegardé).
- 11:02:42 : relais provoqué par l'utilisateur (confort 20,5 → 20,0 sur le satellite) : accusé envoyé, bloc « confirmé », **une seule**
  sauvegarde (`mask 1`) malgré les répétitions du relais.
- 11:03:17 : redémarrage par OTA ESPHome de la même image. Au démarrage : « restauré (non confirmé) », consignes 20,0 / 17,0 / 8,0 affichées
  aussitôt (auparavant inconnues jusqu'au relais suivant).
- 11:10:09 : diffusion `08 80 FA` du satellite cohérente ⇒ « confirmé », sans nouvelle sauvegarde. Diffusions reçues à 11:00:09, 11:02:39
  puis 11:10:09 seulement (RSSI ≈ −100 dBm) : il faut compter jusqu'à ~10 min avant la confirmation.
- 11:10:41 : retour au firmware MQTT (`b1e6934`, recompilé pour l'occasion) : portail, MQTT, ECS « Eco Horaires », zone 1 Auto 20,0, aucune anomalie.

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
