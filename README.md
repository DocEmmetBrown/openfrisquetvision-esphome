# openfrisquetvisio-esphome

Pilotez une chaudière **Frisquet Visio** (Eco Radio Visio) depuis **Home Assistant**, sans cloud ni broker MQTT, avec une carte
**Heltec WiFi LoRa 32 V4** à une trentaine d'euros. La carte se fait passer pour le **Frisquet Connect** (le module Wi-Fi
officiel) : elle dialogue avec la chaudière sur sa radio 868 MHz et expose tout à Home Assistant par l'API native ESPHome.

Protocole rétro-conçu à partir de [OpenFrisquetVisio](https://github.com/FreedomNX/OpenFrisquetVisio), puis vérifié et
complété sur une vraie chaudière (voir [docs/PROTOCOL-REFERENCE.md](docs/PROTOCOL-REFERENCE.md)).

> Projet indépendant, sans lien avec Frisquet. Il émet sur la radio de votre chaudière : utilisez-le en connaissance de cause.
> Testé sur une installation à **une zone** (zone 1) avec satellite Visio.

## Ce que vous obtenez

- **Thermostat « Zone 1 »** : température ambiante, consigne, chauffe en cours ; modes Auto (programme), Chauffe (Confort ou
  Réduit), Arrêt = Hors gel ; préréglage **Boost** (+1 °C, 6 h au plus).
- **Effet immédiat** : un changement depuis Home Assistant agit sur la chaudière en moins d'une minute ; le satellite suit
  à son prochain réveil (au plus 10 min, tout de suite si vous appuyez sur sa touche « i »).
- Consignes confort / réduit / hors-gel, programme hebdomadaire, mode d'**eau chaude** (Max, Eco, Eco Horaires, Eco+,
  Eco+ Horaires, Stop).
- **Mode vacances** : programmer début et fin à la minute (y compris une absence de quelques heures), puis activer ou
  annuler explicitement la période depuis Home Assistant. La commande et son état vérifié sont visibles dans le tableau
  de bord (entités date/heure nécessitant Home Assistant 2024.4 ou plus récent). Programmation, annulation et fin
  automatique vérifiées sur chaudière réelle le 11/11/2026, avec passage de la consigne à 8 °C puis retour à 18 °C.
  Le satellite peut répercuter la consigne avec 10 à 20 minutes de retard : ne pas utiliser ce mode pour quelques minutes.
- Mesures : ambiante, extérieure, eau chaude, départ, corps de chauffe, fumées, pression, **brûleur allumé**.
- **Consommation** chauffage et eau chaude en kWh pour le tableau Énergie : **estimée** à partir du temps de flamme et de la
  puissance de la chaudière (`burner_power_kw`, `burner_average_load` en tête de `frisquet.yaml`). Les compteurs de la
  chaudière elle-même sont aussi lus, mais certaines ne comptent pas (toujours 0) : ils sont désactivés par défaut.
- **Alertes** : anomalie chaudière, pile du satellite (si la carte l'entend), événements.
- Diagnostic : version du panneau, appareils appairés, qualité radio. **Écran OLED** de la carte.
- Garde-fous : arrêt d'urgence (plus aucune émission), aucune émission au nom d'un satellite qui ne répond plus.

## Matériel

| Élément      | Détail                                                                                |
| ------------ | ------------------------------------------------------------------------------------- |
| Chaudière    | Frisquet **Visio** avec satellite(s) radio Visio, compatible Frisquet Connect         |
| Carte        | **Heltec WiFi LoRa 32 V4** (V4.3), version **868 MHz**, avec son antenne              |
| Alimentation | Câble USB-C et chargeur 5 V, près de la chaudière (portée radio : comme le satellite) |

Un seul Connect par chaudière : si vous avez un Frisquet Connect officiel, la carte le remplace.

## Installation

### 1. Principe

Deux façons de faire, au choix : depuis le **module ESPHome de Home Assistant** (rien à installer sur un ordinateur), ou avec
ESPHome en ligne de commande. Dans les deux cas, seul `frisquet.yaml` est nécessaire : le composant est téléchargé depuis
ce dépôt GitHub.

Aucune autre valeur n'est à fournir : le réseau radio de la chaudière et les identifiants sont appris à l'association.

### 2. Flasher la carte

**Avec le module ESPHome de Home Assistant** :

1. Dans `/config/esphome/secrets.yaml` (bouton « Secrets » du module), ajoutez les clés de
   [secrets.yaml.example](secrets.yaml.example) : `wifi_ssid`, `wifi_password`, `api_key`.
2. **Nouvel appareil** › continuer › ignorer l'installation, puis **Modifier** l'appareil créé et remplacez tout son contenu par
   celui de [frisquet.yaml](frisquet.yaml).
3. **Installer** : par câble USB la première fois (« Brancher sur cet ordinateur »), sans fil ensuite.

**En ligne de commande** (`pip install esphome`) : copiez `frisquet.yaml` et `secrets.yaml.example` (renommé
`secrets.yaml`, à remplir) dans un dossier, branchez la carte en USB, puis :

```bash
esphome run frisquet.yaml
```

Les mises à jour suivantes passent par le Wi-Fi.

**Remplacer un firmware ESPHome déjà installé** (autre nom, mot de passe OTA) sans câble : pour le premier envoi seulement,
ajoutez `use_address: <adresse IP de la carte>` sous `wifi:` et remplacez `encryption: {}` de la section `ota:` par
`password: "<mot de passe OTA de l'ancien firmware>"`. Une fois installé, revenez à `encryption: {}`, retirez `use_address`
et réinstallez.

### 3. Ajouter à Home Assistant

Home Assistant découvre la carte (« Frisquet ») : acceptez, et donnez la clé `api_key` des secrets.

### 4. Associer la carte à la chaudière (une seule fois)

Sur le panneau de la chaudière :

1. Ouvrez **Mise en service › Radio**.
2. Choisissez **Remplacer › Frisquet Connect** si un Connect est déjà appairé, sinon **Ajouter › Frisquet Connect**.
3. Arrêtez-vous **juste avant le dernier OK**.
4. Dans Home Assistant, appuyez sur **« Radio associer le Connect »** (la carte écoute 30 s).
5. Validez aussitôt **OK** sur la chaudière. L'association se fait en quelques secondes.

« Radio réseau » affiche alors le réseau et l'association (par exemple `AA:BB:CC:DD (appris), association 21`). C'est gardé en
mémoire, y compris après une coupure de courant. Si la chaudière répond « terminé » sans rien attendre, passez d'abord par
**Effacer › Connect**, puis **Ajouter**.

### 5. Attendre la première synchronisation

Les valeurs de la zone arrivent au plus tard **10 minutes** après l'association (au prochain échange du satellite). Les
commandes de zone sont acceptées dès que « Zone 1 bloc » vaut **confirmé** (après chaque redémarrage aussi : jusqu'à 10 min).

## Utilisation

| Entité                                             | Rôle                                                                                                                                                                      |
| -------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Zone 1 (thermostat)                                | Mode, préréglage, consigne du mode actif                                                                                                                                  |
| Zone 1 mode (commande)                             | Auto, Dérogation Confort / Réduit, Confort, Réduit, Hors Gel                                                                                                              |
| Zone 1 consigne confort / réduit / hors-gel        | Consignes (pas de 0,5 °C)                                                                                                                                                 |
| Zone 1 boost                                       | +1 °C sur la consigne confort, en Confort ou en période confort ; arrêt automatique après 6 h                                                                             |
| Zone 1 programme                                   | 84 caractères hexadécimaux : 6 octets par jour (dimanche d'abord), un bit par demi-heure, bit de poids faible d'abord ; « Zone 1 programme (lisible) » l'affiche en clair |
| Chaudière mode ECS                                 | Mode d'eau chaude                                                                                                                                                         |
| Chaudière vacances début / fin                     | Dates et heures locales, conservées après redémarrage                                                                                                                      |
| Chaudière activer / annuler les vacances           | Programme ou efface explicitement la période ; « Chaudière vacances (état) » confirme la valeur relue                                                                       |
| Chaudière brûleur, état, anomalie, dernière alerte | Suivi de la chaudière                                                                                                                                                     |
| Consommation chauffage / eau chaude (estimée) | kWh pour le tableau Énergie ; ajuster `burner_average_load` avec le compteur de gaz |
| Radio arrêt d'urgence                              | Coupe toute émission (la carte n'écoute plus que)                                                                                                                         |

Bon à savoir :

- **Délais** : la chaudière réagit en moins d'une minute ; le satellite reçoit le nouveau réglage à son prochain réveil.
  Appuyer sur sa touche « i » le réveille. Un petit nuage s'affiche alors brièvement sur le satellite : c'est normal.
- Un réglage fait sur le satellite reste prioritaire : Home Assistant le voit en quelques secondes.
- Une seule modification à la fois : une nouvelle demande remplace celle qui attend encore le satellite.

## Home Assistant

- **Tableau de bord** prêt à coller : [homeassistant/dashboard.yaml](homeassistant/dashboard.yaml) (thermostat, zone,
  chaudière, historique, consommations, radio), dans l'« éditeur de configuration brute » d'un nouveau tableau de bord. Il
  suppose les identifiants par défaut (`climate.frisquet_zone_1`…). S'ils diffèrent chez vous (autre nom d'appareil, pièce
  dans le nom), générez la version adaptée : `python3 homeassistant/dashboard.py cuisine_frisquette > tableau.yaml`
  (option `--entites` si les préfixes sont mélangés, voir `--help`).
- **Consignes sans slider** : les tuiles Confort / Réduit / Hors-gel proposent des boutons − / + par pas de 0,5 °C.
  Cliquez sur une tuile pour saisir directement une valeur. Pour cette saisie, ajoutez `mode: box` aux trois entités
  `number` dans votre configuration ESPHome (comme dans [frisquet.yaml](frisquet.yaml)), puis réinstallez le firmware.
- **Vacances immédiates** : installez le package **Home Assistant** [holiday-immediate.yaml](homeassistant/holiday-immediate.yaml)
  dans `/config/packages/holiday-immediate.yaml`. Si nécessaire, activez les packages dans `configuration.yaml` :
  ```yaml
  homeassistant:
    packages: !include_dir_named packages
  ```
  Fusionnez cette option dans le bloc `homeassistant` existant, vérifiez la configuration HA puis redémarrez HA.
  Remplacez ensuite la carte Vacances avec celle du dashboard fourni. Choisissez la **date et l'heure de retour** et
  confirmez **Partir maintenant** : le champ Départ est remplacé par l'heure locale actuelle,
  avec le même champ Retour que pour un départ planifié, même aujourd'hui s'il est futur.
  Les trois entités ESPHome sont passées au script par le dashboard : adaptez-les à votre installation, ou utilisez
  `dashboard.py --entites`. Le script attend la confirmation des dates par l'ESP avant d'activer ; une erreur est notifiée
  dans HA. L'état d'écriture chaudière reste la confirmation finale. Aucun nouveau flash n'est nécessaire.
- **Départ planifié** : dans la même carte Vacances, choisissez la date et l'heure de départ et de retour,
  puis **Programmer** ; « Annuler les vacances »
  efface la période. La période confirmée par la chaudière et l'état de la commande restent visibles ; un changement
  des champs seuls n'envoie aucune commande. Après mise à jour d'un ancien firmware, si la période apparaît invalide,
  annulez-la avant d'en programmer une nouvelle. Le mode Auto et les températures sauvegardées ne sont pas remplacés.
- **Éditeur graphique du programme** : copiez [homeassistant/frisquet-program-card.js](homeassistant/frisquet-program-card.js)
  dans `/config/www/frisquet-program-card.js` sur HA (créez `www` si nécessaire ; redémarrez HA après sa première création).
  Dans Paramètres > Tableaux de bord > ⋮ > Ressources (mode avancé du profil activé), ajoutez
  `/local/frisquet-program-card.js?v=4` comme **Module JavaScript**, puis rechargez le navigateur.
  Pour mettre à jour une ancienne carte, remplacez le fichier, passez à `v=4` dans la ressource et rechargez complètement.
  La vue compacte place les jours à gauche des bandes ; les explications sont dans **Aide et fonctionnement**.
  Le tableau de bord utilise cette carte :

  ```yaml
  type: custom:frisquet-program-card
  entity: text.frisquet_zone_1_programme
  write_entity: sensor.frisquet_zone_1_ecriture
  ```

  Chaque jour est représenté par une bande bicolore : **orange = Chauffage (Confort)**, **bleu = Réduit**,
  avec les horaires et le résumé des périodes. Cliquez sur le jour pour ajouter, modifier ou supprimer ses périodes de
  chauffage par pas de 30 minutes (fin possible à 24:00). Les autres heures restent en Réduit ; pour une période passant
  minuit, éditez les deux journées. Les chevauchements et les fins antérieures au début sont refusés.
  Cochez les jours destinataires puis **Valider et copier aux jours cochés** pour réutiliser une journée sur plusieurs
  autres, après confirmation. **Valider la journée** conserve seulement le jour édité ; **Fermer sans valider** abandonne
  son édition. Ces actions ne transmettent rien à la chaudière. Cliquez sur **Appliquer le
  programme**, puis confirmez pour envoyer les modifications ; la carte attend ensuite le programme publié par l'ESP.
  L'état d'écriture indique la confirmation radio ou un refus. Le bouton **Annuler les modifications** abandonne le
  brouillon et reprend la dernière valeur reçue (sans déclencher de lecture radio). Un changement externe pendant
  l'édition bloque l'envoi jusqu'à l'annulation des modifications. Le programme agit en **mode Auto**, que la carte ne change pas.
  Aucun nouveau firmware n'est nécessaire pour cette carte si l'entité texte du programme est déjà disponible.
  Adaptez les deux identifiants à votre installation ; le générateur du tableau de bord les adapte également.
- **Blueprint de notification** (anomalie, pile du satellite) :
  [homeassistant/blueprints/automation/frisquet_alerte.yaml](homeassistant/blueprints/automation/frisquet_alerte.yaml), à
  copier dans `config/blueprints/automation/` de Home Assistant.
- **Plusieurs plannings** : profils nommés conservés dans HA, chacun avec sa semaine et ses températures Confort /
  Réduit / Hors-gel, partagés entre téléphone et ordinateur. Leur application conserve le mode actuel et utilise une
  commande unique programme + consignes (mise à jour firmware nécessaire).
  Le dashboard complet les gère dans l'onglet **Plannings** ; l'onglet **Chauffage** sert à les charger et les activer.
  Les deux cartes nécessitent les helpers et l'action ; l'exemple de carte simple ci-dessus reste utilisable sans profils.
  Voir [l'installation des profils](docs/PROGRAM-PROFILES.md).


## Sonde extérieure via Home Assistant (expérimental)

Une mesure extérieure HA peut être transmise à la chaudière avec une identité radio de sonde distincte du Connect.
Cette fonction est **désactivée par défaut** et nécessite une association explicite, après confirmation de compatibilité.
La dernière mesure est conservée au maximum 180 minutes ; au-delà, les émissions cessent et un défaut chaudière est possible.
Voir [l'installation et les précautions](docs/OUTDOOR-SENSOR.md) avant toute activation.

## Écran

L'écran OLED de la carte affiche : ambiante > consigne, mode de la zone, état de la chaudière (et la flamme), température de
l'eau chaude, dernière alerte. Sans écran, supprimez les blocs `i2c:`, `font:`, `display:` et le `on_boot:` de `frisquet.yaml`.

## Dépannage

| Symptôme                                    | Que faire                                                                                                                |
| ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| « Radio réseau » = non associé              | Refaire l'étape 4.                                                                                                       |
| La chaudière affiche « Appareil incorrect » | Supprimer `pairing_version` de `frisquet.yaml`, reflasher, refaire l'étape 4.                                            |
| Une commande de zone est refusée            | « Zone 1 bloc » n'est pas encore « confirmé » : attendre le prochain échange du satellite (10 min au plus).              |
| « Zone 1 écriture » reste sur `sending`     | Le satellite n'a pas encore pris le réglage : attendre 10 min ou appuyer sur « i ». La chaudière, elle, l'applique déjà. |
| Rien ne bouge après un changement           | Vérifier « Radio âge dernière trame » (quelques secondes) et rapprocher la carte de la chaudière.                        |
| Comportement anormal                        | Activer « Radio arrêt d'urgence » : la carte n'émet plus rien.                                                           |

Les journaux détaillés : `esphome logs frisquet.yaml` (passer `logger: level` à `DEBUG` pour voir chaque trame radio).

## Pour aller plus loin

- [docs/PROTOCOL-REFERENCE.md](docs/PROTOCOL-REFERENCE.md) : **référence complète du protocole radio**, autonome, pour le
  réimplémenter sans ce code.
- [docs/DESIGN.md](docs/DESIGN.md) : conception et journal de toutes les fenêtres de test sur la chaudière.
- [docs/PROTOCOL.md](docs/PROTOCOL.md) : journal de découverte du protocole ; [docs/EXTERNAL-RESEARCH.md](docs/EXTERNAL-RESEARCH.md) :
  synthèse des sources communautaires.
- [docs/RESUME.md](docs/RESUME.md) : état du développement et procédures des développeurs.
- `components/frisquet/` : le composant (cœur du protocole dans les en-têtes `proto_*.h`, sans dépendance ESPHome).
- `tests/host/` : tests du cœur sur ordinateur, `bash tests/host/run.sh` (compilateur C++ seulement).
- `dev/` : configurations et outils des phases de développement (non nécessaires à l'utilisation).

### Options du composant `frisquet:`

| Option                      | Défaut        | Rôle                                                                                           |
| --------------------------- | ------------- | ---------------------------------------------------------------------------------------------- |
| `board`                     | (obligatoire) | `heltec_v4_3`                                                                                  |
| `tx`                        | `none`        | `none` (écoute seule), `read`, `ack`, `write` (tout)                                           |
| `dry_run`                   | `true`        | Construit et journalise les trames sans les émettre                                            |
| `pairing`                   | `false`       | Autorise le bouton d'association à répondre à la chaudière                                     |
| `pairing_version`           | —             | Réponse d'association (version d'un Connect, `01:21:01:02`) au lieu de l'écho du réseau        |
| `satellite_override`        | `false`       | Effet immédiat des changements de zone 1 (au nom du satellite)                                 |
| `satellite_association`     | appris        | Octet d'association du satellite de zone 1                                                     |
| `network_id`, `association` | appris        | Réseau et association, si on les connaît déjà                                                  |
| `rx_boosted_gain`           | `false`       | Gain de réception renforcé (≈ +3 dB)                                                           |
| `rx_bandwidth`              | `234.3`       | Bande passante de réception (kHz)                                                              |
| `explore`                   | `false`       | Lectures libres de la mémoire de la chaudière (texte de type `explore`, pour le développement) |

## Crédits et licence

Protocole d'origine : [OpenFrisquetVisio](https://github.com/FreedomNX/OpenFrisquetVisio) et la communauté (voir
[docs/EXTERNAL-RESEARCH.md](docs/EXTERNAL-RESEARCH.md)). Code et documentation de ce dépôt : licence [MIT](LICENSE).
