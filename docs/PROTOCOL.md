# Protocole radio Frisquet (état des connaissances)

Source : rétro-ingénierie d'OpenFrisquetVisio, vérifiée sur des trames réelles capturées le 25/10/2026
(`tests/fixtures/frames.txt`). Tout ce qui n'est pas confirmé est marqué *(non vérifié)*.

## Couche physique

| Paramètre | Valeur |
|---|---|
| Fréquence | 868,96 MHz |
| Modulation | GFSK, 25 kbit/s, déviation 50 kHz |
| Bande RX | 234,3 kHz par défaut (SX1262) |
| Préambule | 16 bits |
| Mot de synchronisation | **NetworkID** de 4 octets propre à l'installation (`FF:FF:FF:FF` = diffusion, utilisé uniquement pour l'association) |
| Capture | 68 octets bruts après le mot de synchronisation, en longueur fixe, CRC matériel désactivé |

Trame brute : `longueur (1)`, `données (longueur)`, `CRC (2, MSB d'abord)`, puis du bruit. La longueur est de 6 à 63.
**CRC-16** : polynôme `0x1021`, valeur initiale `0x1D0F`, sortie inversée (XOR `0xFFFF`), calculé sur l'octet de longueur
**et** les données. La liaison subit des **glissements de bit** : une réparation d'un seul zéro inséré ou supprimé dans une
suite d'au moins 16 zéros après l'en-tête rattrape une partie des trames (`CRC_REPAIRED`), deux candidats distincts ⇒ rejet.

## En-tête (6 octets)

| Octet | Champ | Remarques |
|---|---|---|
| 0 | destinataire | `0x80` chaudière, `0x08/09/0A` satellites Z1/Z2/Z3, `0x20` sonde extérieure, `0x7E` Connect |
| 1 | expéditeur | idem |
| 2 | association | octet attribué à l'appareil lors de l'association (`0xE3` sur l'installation de référence pour le Connect) |
| 3 | identifiant de message | incrémenté par l'émetteur ; la réponse le reprend |
| 4 | réception | `0x01` message direct ; sinon destinataire final (relais via la chaudière) ; **+0x80 = accusé/réponse** |
| 5 | type | `0x03` lecture, `0x17` écriture/échange (« init »), `0x41` association |

## Lectures (type 0x03)

Requête : en-tête + `adresse (2)` + `taille (2, en mots)` = 10 octets, par exemple `80 7E E3 86 01 03 79 F2 00 1C`.
Réponse : en-tête (réception `0x81`) + `longueur (1)` + données. **La réponse ne contient pas l'adresse** : on l'associe à la
requête par (demandeur, identifiant de message). Une autre famille de chaudière répond aux mêmes données à adresse + `0xC8`.

| Adresse | Contenu | Réponse |
|---|---|---|
| `0x79F2` | températures, pression, état de fonctionnement | 63 octets, longueur 0x38 |
| `0x7A18` | consommation journalière (ECS, chauffage) | 63 octets |
| `0xA063` | consommation mensuelle de l'année (24 mots : chauffage puis ECS pour chaque mois) | 55 octets, longueur 0x30 |
| `0xA07B` | même chose pour l'année précédente | *(non vérifié)* |
| `0xA0FC` | mode ECS (1 mot) | 9 octets, longueur 0x02 |
| `0xA02B` | date/heure de la chaudière | |

### Réponse `0x79F2` (après les 7 octets d'en-tête + longueur)

Mots de 16 bits big-endian signés. Températures en dixièmes de °C, **127,0 (`0x04F6`) et 129,0 (`0x050A`) = capteur absent**.

| Décalage | Champ | Exemple observé |
|---|---|---|
| 0, 2, 4 | ambiante Z1, Z2, Z3 | `00 BA` = 18,6 °C |
| 6, 8, 10 | consigne de départ Z1, Z2, Z3 | `01 42` = 32,2 °C |
| 12, 14, 16 | consigne Z1, Z2, Z3 | `00 C8` = 20,0 °C |
| 18 | extérieure | `05 0A` (aucune sonde) |
| 20 | ECS | `02 E6` = 74,2 °C |
| 22 | corps de chauffe | `03 0E` = 78,2 °C |
| 24, 26, 28 | départ Z1, Z2, Z3 | `01 20` = 28,8 °C |
| 30 | sonde de sécurité | `01 40` = 32,0 °C |
| 32 | **fumées** (valeur brute / 2) : **indicateur du brûleur** | `00 00` à l'arrêt ; `09 99` = 122,85 °C brûleur allumé ; 121,8 °C le 29/10 11:17 |
| 34, 36 | puissance ECS, puissance chauffage | **toujours 0** sur la chaudière observée, même brûleur allumé |
| 38 | inconnu | 0 |
| 40 | pression (÷ 5120, bar) | `1E 00` = 1,50 bar |
| 42 | état de fonctionnement | `00 09` : bits 0-1 = `01` radiateurs, `11` ECS |

## Diffusion chaudière → satellite Z1 (type 0x17, 49 octets, toutes les 10 minutes à `:x0:12`)

`08 80 FA <id> 81 17 2A` puis : extérieure (2), inconnu (2), **date BCD** `AA MM JJ hh mm ss` (6), état chaudière (1),
jour de la semaine (1, 0 = dimanche), puis par zone (Z1, Z2, Z3 ; 10 octets chacune) : ambiante (2), consigne (2), inconnu (1),
**mode (1)**, inconnu (4) — ces 4 octets semblent contenir température et consigne de départ *(non vérifié)*.

- État chaudière : bit 0 anomalie, bit 2 chauffage arrêté, bit 3 en fonctionnement (`0x28` en marche, `0x20` veille). Vu `0x28` le 29/10 à 11:24 brûleur allumé (fumées 121,8 °C à 11:17), mais mis à jour seulement à chaque échange du satellite (10 min, ou à une modification). **La chaudière n'émet aucune trame propre au démarrage ou à l'arrêt du brûleur** ; la flamme est un témoin local de la chaudière.
- Mode : l'octet de mode de la diffusion est l'octet **options** du bloc de zone. Vérifié le 26/10/2026 : la zone restant en Auto
  (mode `05` du bloc), le passage du programme de confort à réduit fait passer les options de `25` à `20`. Donc `25` = **Auto, confort**
  et `20` = **Auto, réduit**. Anciennes valeurs de l'ancien firmware : `00` réduit permanent, `01` confort permanent, `02` dérogation réduit,
  `03` dérogation confort, `04` réduit auto, `05` confort auto, `10` hors gel (vu `14` le 28/10 : hors gel réglé sur le satellite, le bit `0x04` varie comme en Auto) ; le bit `0x20` est un indicateur de compatibilité Connect.
  *Vérifié aussi : le réduit **permanent** (satellite mis en Réduit à la main, 26/10 10:24) donne une diffusion `00` et un bloc de mode `07`,
  options `00`. `20` (Auto réduit) et `00` (réduit permanent) sont donc bien distincts. Après le retour au programme normal, le confort
  en Auto s'est affiché `21` (et non `25`) : le bit `0x04` dépend de ce que le Connect a écrit en dernier (il reprend les options que le
  satellite lui renvoie). Règle de décodage retenue : bit `0x20` = famille Auto, bit `0x01` = confort ; `00` seul = réduit permanent.*

## Échanges de zone (type 0x17, 63 octets)

La chaudière relaie vers le Connect (`7E 80 E3 <id> 08 17 …`, réception `0x08` = zone 1) le bloc que le satellite écrit ; le Connect
**doit accuser** par un écho (`80 7E E3 <id> 88 17 30 …`, réception `0x88`). Sans accusé, la chaudière répète le relais toutes les ~4 s et
finit par afficher **« anomalie avec Connect »** (bit 0 de l'octet d'état de la diffusion, vu après ~11 h sans Connect).

Bloc : `A1 54 <taille>` (lecture) `A1 54 <taille>` (écriture) `30` (48 octets) puis consigne confort, réduit, hors gel (en dixièmes de °C
au-dessus de 5 °C : `0x8C` = 19,0), mode (`05` auto), options (`25` = auto + confort + indicateur `0x20`), puis le **programme hebdomadaire de 42 octets**
(7 jours × 6 octets, dimanche d'abord, bit 1 = confort, bit 0 de l'octet 0 = 00:00-00:30).

## Écriture du mode ECS (type 0x17, direct) *(capturée sur l'air le 26/10/2026 14:23, Eco Horaires → Eco)*

`80 <connect> <assoc> <id> 01 17 A0 FC 00 01 A0 FC 00 01 02 00 <octet de mode>` (17 octets), par exemple `80 7E E3 1C 01 17 A0 FC 00 01 A0 FC 00 01 02 00 88`.
L'octet de mode est le mode (bits `0x7E` :
`00` Max, `08` Eco, `10` Eco Horaires, `18` Eco+, `20` Eco+ Horaires, `28` Stop) plus les **bits de trame** `0x81` lus avec la valeur
actuelle (`0x90` = `0x80` + Eco Horaires sur cette installation). La chaudière répond dans la seconde par un accusé INIT
(`7E 80 E3 1C 81 17 02 00 88`, 9 octets : réception `0x81`, type `0x17`, et la **nouvelle valeur** `00 88` relue).
L'ancien firmware ne relit pas la valeur ; le nouveau la relit (`A0FC`) et ne déclare l'écriture réussie que si elle correspond.

## Écriture d'un bloc de zone (type 0x17, 63 octets) *(capturée le 25/10/2026 17:56 – 17:59)*

`80 <connect> <assoc> <id> <zone 08..0A> 17 A1 54 00 18 A1 54 00 18 30 <bloc de 48 octets>` (63 octets). La chaudière le relaie au satellite
(`08 80 FA <id> 7E 17 …`) et **n'y répond qu'une fois le satellite arrivé à le recevoir** : le satellite n'écoute qu'autour de son créneau
(`:x7:02` sur cette installation), l'acceptation peut donc prendre plusieurs minutes (~10 min observées).

- Le Connect émet des **rafales** de 5 trames espacées de 2 s sous un même id, puis la rafale suivante sous l'id suivant après ~30 s,
  jusqu'à la réponse.
- Réponse de la chaudière : `7E 80 E3 <id> 88 17 30 <bloc>` (55 octets), un écho du bloc, vue environ 1 s après la dernière émission reçue
  par le satellite. L'id peut être celui d'une **rafale précédente** (vu : réponse sous `0x83` alors que les rafales en étaient à `0x8A`).
- Le bloc **ne peut pas être lu** (`A154` renvoie une erreur de type `0x83`) : le Connect le connaît par les relais de la chaudière
  (et sa persistance). Une écriture part toujours du dernier bloc connu, où seuls les champs voulus changent.

## Association (type 0x41, réseau de diffusion) *(capturée le 28/10/2026 22:29, association Connect)*

Procédure côté chaudière : « Partenaires > Ajouter > Connect », jusqu'au dernier OK. **Tant qu'un Connect est déjà associé,
la chaudière répond aussitôt « terminé » sans rien émettre** (deux écoutes de 30 s n'ont rien reçu) : il faut d'abord
« Partenaires > Effacer > Connect », ce qui invalide l'association en cours.

L'offre arrive ~7 s après l'appui, sur le mot de synchronisation `FF:FF:FF:FF`, en 11 octets :
`<dest> 80 <association> <id> <réception> 41 04 <NetworkID:4>`, par exemple `00 80 D5 D4 02 41 04 05 D9 8D C9`
(destinataire `00`, réception `02`, NetworkID de l'installation inchangé, **nouvel octet d'association `D5`**).
Une **récupération du NetworkID** se contente d'écouter cette offre (30 s au plus). Une **association** répond, toujours sur le
réseau de diffusion, 10 fois à 30 ms d'intervalle, par l'en-tête de réponse suivi du NetworkID, **sans octet de longueur**
(10 octets) : `80 <soi> <association> <id> <réception|0x80> 41 <NetworkID:4>`, ici `80 7E D5 D4 82 41 05 D9 8D C9` ; la
chaudière affiche alors OK et répond aux lectures avec la nouvelle association. Le destinataire de l'offre n'est pas vérifié.

## Créneaux (cycle de 10 minutes)

| Instant | Émetteur | Action |
|---|---|---|
| `:00`, `:01`, `:02` (±30 s) | satellites Z1/Z2/Z3 | échanges propres aux satellites : **fenêtre silencieuse** pour le Connect |
| `:03`, `:06`, `:08` | Connect | lecture `0x79F2` |
| `:05` de l'heure | Connect | lecture `0x7A18` et `0xA063` |
| `:07` de l'heure | Connect | lecture `0xA0FC` (mode ECS) |

Une tentative par créneau, sans rattrapage ; l'heure de référence est celle de la chaudière (ou NTP), à défaut le temps de
fonctionnement.

## Réception observée (Heltec V4.3)

Diffusion chaudière → satellite à ≈ −100 dBm ; réponses de la chaudière au Connect à ≈ −80 dBm. L'activation du LNA (bypass désactivé)
a coïncidé avec la disparition d'échecs de lecture intermittents (observation sur quelques heures, pas une preuve) ; un échec
d'association laissait la radio sur le réseau de diffusion (corrigé, voir `DESIGN.md`).
