# Protocole radio Frisquet : référence pour une réimplémentation

Ce document décrit tout ce qu'il faut savoir pour réimplémenter un « Connect » Frisquet (passerelle radio de la chaudière) sans
refaire la rétro-ingénierie. Il est **autonome** : il ne suppose ni le code de ce dépôt, ni celui d'OpenFrisquetVisio.

- Statut de chaque fait : **[vérifié]** = observé sur l'air ou testé sur la chaudière de référence (oct. 2026) ;
  **[MQTT]** = repris du firmware OpenFrisquetVisio, non observé ici ; **[hypothèse]** = déduction non confirmée.
- Installation de référence : chaudière Frisquet à une seule zone, satellite d'ambiance Z1 sans fil, pas de sonde extérieure,
  carte Heltec WiFi LoRa 32 V4.3 (SX1262). NetworkID masqué en `AA:BB:CC:DD` dans ce document (les CRC des exemples de
  trames restent ceux du vrai identifiant ; les trames de [`tests/fixtures/frames.txt`](../tests/fixtures/frames.txt) sont intactes). Association du Connect `21` depuis le 01/11 21:33 (`D5` du 28/10 au 01/11, `E3` avant).
- Notation : octets en hexadécimal, mots de 16 bits **big-endian**, adresses mémoire en mots (`A029 x15` = 0x15 mots à partir de `A029`).
- Historique et mesures détaillées : [DESIGN.md](DESIGN.md) §10 ; état des connaissances au fil de l'eau : [PROTOCOL.md](PROTOCOL.md) ;
  trames réelles : [`tests/fixtures/frames.txt`](../tests/fixtures/frames.txt).

---

## 1. Couche physique

| Paramètre | Valeur | Statut |
|---|---|---|
| Fréquence | 868,96 MHz | [vérifié] |
| Modulation | 2-FSK, **sans mise en forme** (shaping none), codage NRZ, **sans blanchiment** | [vérifié] |
| Débit | 25 kbit/s | [vérifié] |
| Déviation | 50 kHz | [vérifié] |
| Préambule | 16 bits (`0x55…`) | [vérifié] |
| Mot de synchronisation | 4 octets = **NetworkID** de l'installation ; `FF FF FF FF` pendant une association | [vérifié] |
| Format | longueur variable : `longueur (1)` + `données` + `CRC (2)` | [vérifié] |
| Puissance d'émission | 10 dBm suffisent (chaudière à quelques mètres) | [vérifié] |
| Bande de réception | 234,3 kHz (467 kHz n'apporte rien) | [vérifié] |

### 1.1 Trame et CRC

Après le mot de synchronisation : `L` (longueur des données, 6 à 63), les `L` octets de données, puis le CRC sur 2 octets, poids fort
d'abord.

**CRC-16** : polynôme `0x1021`, valeur initiale `0x1D0F`, sans réflexion, **résultat inversé** (`XOR 0xFFFF`), calculé sur
**l'octet de longueur et les données**. C'est le CRC « CCITT » du SX126x configuré avec `init = 0x1D0F, inverted = true`.

```c
uint16_t crc = 0x1D0F;
for (each byte b of [L, data...]) {
  crc ^= b << 8;
  for (int i = 0; i < 8; i++) crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
}
crc ^= 0xFFFF;   // émis poids fort d'abord
```

Vecteurs de test (octets sur l'air après le mot de synchronisation) :

| Trame | Octets |
|---|---|
| lecture `79F2` | `0A 80 7E D5 88 01 03 79 F2 00 1C EB A6` |
| réponse `A0FC` | `09 7E 80 D5 8B 81 03 02 00 90 AA EA` |
| confirmation d'association | `0A 80 7E D5 D4 82 41 AA BB CC DD 91 1D` |

### 1.2 Réception : glissements de bit **[vérifié]**

Les trames de la chaudière arrivent parfois avec **un bit `0` en trop ou en moins** au milieu d'une longue suite de zéros (champs
nuls des réponses). Un CRC matériel les rejetterait. La méthode qui fonctionne :

1. recevoir en **longueur fixe de 68 octets**, CRC matériel désactivé (on capture longueur, données, CRC et un peu de bruit) ;
2. vérifier le CRC en logiciel ; s'il est faux, pour chaque suite d'au moins 16 bits nuls **après l'en-tête**, essayer d'y insérer
   puis d'y retirer un `0`, et recalculer ;
3. accepter si **un seul** candidat distinct est valide ; plusieurs candidats différents ⇒ rejet.

Ordre de grandeur : quelques trames réparées par heure, aucune fausse réparation observée. Conséquence de la capture en 68 octets :
le RSSI lu après coup est celui du bruit pour une trame courte (≈ −100 dBm), et non celui de la trame (≈ −75 dBm). Lire le RSSI
pendant la réception si on en a besoin.

### 1.3 Émission

Repasser en **longueur variable avec CRC matériel** (mêmes paramètres de CRC) pour émettre, puis revenir en réception. Attendre
**≈ 30 ms** après la fin d'une trame reçue avant de répondre (comme le firmware d'origine).

---

## 2. En-tête (6 octets)

| Octet | Champ | Description |
|---|---|---|
| 0 | destinataire | identifiant de l'appareil (voir 2.1) |
| 1 | émetteur | idem |
| 2 | association | octet d'association **de l'appareil non-chaudière** de l'échange (le Connect ou le satellite) |
| 3 | id de message | choisi par l'initiateur ; la réponse reprend le même |
| 4 | réception | `01` message direct ; sinon **destinataire ou origine finale** d'un relais ; `+0x80` = réponse/accusé |
| 5 | type | `03` lecture, `17` échange (lecture et écriture), `41` association ; **`+0x80` = refus/erreur** |

### 2.1 Identifiants **[vérifié sauf indication]**

| Id | Appareil |
|---|---|
| `80` | chaudière |
| `7E` | Connect (la passerelle à réimplémenter) |
| `08`, `09`, `0A` | satellites des zones 1, 2, 3 (`09`, `0A` [MQTT]) |
| `20` | sonde extérieure [MQTT] |
| `00` | diffusion (offre d'association) |

### 2.2 Octet de réception

- `01` : message direct entre l'émetteur et le destinataire.
- `08`/`09`/`0A` ou `7E` : la chaudière **relaie** un message : vers le Connect, `réception` = zone d'origine ; vers un satellite,
  `réception` = `7E` (origine Connect).
- `+0x80` : réponse (`81` à un message direct, `88` à un relais de la zone 1, `FE` = `7E + 80` relayé vers le satellite, `82`…).

### 2.3 Octet d'association et id de message

- Chaque appareil reçoit son octet d'association de la chaudière lors de l'appariement (section 3). La chaudière ignore
  vraisemblablement les trames portant une autre association que celle enregistrée pour l'émetteur [hypothèse : non testé].
- Le satellite de la zone 1 utilise `FA` sur l'installation de référence ; la chaudière l'utilise aussi dans ses trames vers lui.
- L'id de message est incrémenté par l'initiateur ; les relais et diffusions de la chaudière ont leur propre compteur. Une réponse
  peut porter l'id d'une émission **antérieure** de la même opération (cas des écritures de zone, 6.3).

---

## 3. Réseau et association

### 3.1 NetworkID

Les 4 octets du mot de synchronisation sont propres à l'installation et ne changent pas lors d'une association. Ils figurent en
mémoire en `791A – 791B` (section 8). Sans eux, on n'entend rien : il faut soit les connaître, soit écouter une offre d'association.

### 3.2 Offre d'association **[vérifié le 28/10/2026]**

Côté chaudière : « Partenaires > Ajouter > Connect », jusqu'au dernier OK. **Si un Connect est déjà associé, la chaudière répond
aussitôt « terminé » sans rien émettre** : il faut d'abord « Partenaires > Effacer > Connect ».

Environ **7 s** après le dernier OK, sur le mot de synchronisation **`FF FF FF FF`**, la chaudière émet (11 octets) :

```
<dest> 80 <assoc> <id> <réception> 41 04 <NetworkID:4>
00     80 D5      D4   02          41 04 AA BB CC DD        (exemple réel)
```

`assoc` est le **nouvel octet d'association** attribué. Le destinataire (`00`) et la réception (`02`) n'ont pas besoin d'être vérifiés.

### 3.3 Confirmation

Toujours sur `FF FF FF FF`, répondre **10 fois à 30 ms d'intervalle** par l'en-tête de réponse suivi du NetworkID, **sans octet de
longueur** dans les données :

```
80 <soi> <assoc> <id> <réception|80> 41 <NetworkID:4>
80 7E    D5      D4   82             41 AA BB CC DD         (10 octets)
```

Puis revenir sur le NetworkID et utiliser `assoc` dans toutes les trames suivantes. La chaudière affiche OK et répond aussitôt aux
lectures portant la nouvelle association.

**Variante avec version [vérifié le 01/11/2026]** : un vrai Connect met sa **version** à la place du NetworkID (`01 21 01 02`,
v21.01.02, d'après d33d33). La chaudière l'accepte de même : « Mise en service › Radio › Remplacer › Frisquet Connect »,
offre `00 80 21 38 02 41 04 AA BB CC DD`, réponse `80 7E 21 38 82 41 01 21 01 02`, nouvelle association `21`, lectures
normales aussitôt. `7A30 – 7A33` (section 8) n'a pas changé ensuite (`FF7E FFFF` : pas de version enregistrée pour le Connect).

**Récupérer seulement le NetworkID** : écouter une offre sans y répondre. Il faut donc que la chaudière en émette une : supprimer
d'abord le Connect (3.2), ou lancer « Remplacer Satellite Z1 » puis annuler sur la chaudière [MQTT].

**Piège** : quel que soit le résultat (offre reçue, délai écoulé, annulation), **toujours revenir** sur le NetworkID de
l'installation ; rester sur `FF FF FF FF` coupe le Connect de la chaudière.

---

## 4. Lectures (type `03`)

### 4.1 Requête et réponse **[vérifié]**

```
requête : 80 7E <assoc> <id> 01 03 <adresse:2> <taille en mots:2>          (10 octets)
réponse : 7E 80 <assoc> <id> 81 03 <longueur:1> <données: 2 × taille>
refus   : 7E 80 <assoc> <id> 81 83 02                                        (7 octets)
```

- **La réponse ne contient pas l'adresse** : on l'apparie à la requête par (destinataire, id de message).
- Taille maximale utile : `0x1C` mots (56 octets de données, trame de 63 octets).
- **Refus** (type `83`) : l'adresse n'existe pas ou n'est pas lisible. **Une lecture est refusée en entier dès qu'un seul de ses
  mots est invalide** : pour cartographier, découper les blocs refusés jusqu'au mot.
- Une autre famille de chaudières répondrait aux mêmes données à l'adresse `+ 0x00C8` [MQTT].

### 4.2 Répétition **[vérifié, temporisation du firmware d'origine]**

Émettre **5 fois à 2,2 s d'intervalle** sous le même id ; sans réponse, une seconde tentative sous l'id suivant ; échec après ≈ 22 s.
Un refus `83` termine la lecture immédiatement.

### 4.3 Adresses connues

| Adresse | Taille | Contenu | Rafraîchissement |
|---|---|---|---|
| `79F2` | `1C` | températures, consignes, pression, état (section 7.1) | **instantané** pris à `:x3` (zones) et `:x8` (chaudière) |
| `79E0`, `79FC` | `1C` | deux instantanés de même structure que `79FC…` (ECS, corps de chauffe, départs, fumées, pression…) | `:x2` et `:x7`/`:x8` |
| `7A18` | `1C` | consommations journalières : ECS au mot 9, chauffage au mot 10 (kWh) | [vérifié : toujours 0 ici] |
| `A029` | `15` | **état partagé en direct** : extérieure, date, état chaudière, zones (section 7.2) | **en direct** |
| `A02B` | `06` | date et heure (partie de la page `A029`) | en direct |
| `A05E` | `01` | **température du corps de chauffe en direct** (dixièmes de °C) | en direct |
| `A063` | `18` | consommations mensuelles de l'année : chauffage puis ECS pour chaque mois | [vérifié : 0 ici] |
| `A07B` | `18` | même chose, année précédente | [vérifié : 0 ici] |
| `A0FC` | `01` | **mode ECS** (section 6.1) | en direct |
| `9C54` | `04` | date et heure en direct (lue par la sonde extérieure) | en direct |
| `A154` | — | bloc de zone : **illisible** (refus `83`) ; on ne le connaît que par les relais | — |

---

## 5. Échanges (type `17`)

Forme générale (un échange **lit** une plage et en **écrit** une autre dans la même trame) :

```
80 <émetteur> <assoc> <id> <réception> 17 <adr. lue:2> <taille lue:2> <adr. écrite:2> <taille écrite:2> <long.:1> <données écrites>
```

La réponse (`réception | 80`, type `17`) porte `<longueur:1>` et **les données lues**. Exemples : l'écriture du mode ECS relit le même
mot ; un satellite lit `A029` et écrit sa température ambiante (section 9) ; les écritures de zone portent le bloc (`A154`).

---

## 6. Commandes du Connect

### 6.1 Mode ECS (eau chaude sanitaire) **[vérifié, les 6 modes]**

```
80 7E <assoc> <id> 01 17 A0 FC 00 01 A0 FC 00 01 02 00 <mode>              (17 octets)
réponse : 7E 80 <assoc> <id> 81 17 02 00 <mode relu>                         (9 octets)
```

| Mode | Octet (bits `0x7E`) | Valeur complète observée |
|---|---|---|
| Max | `00` | `80` |
| Eco | `08` | `88` |
| Eco Horaires | `10` | `90` |
| Eco+ | `18` | `98` |
| Eco+ Horaires | `20` | `A0` |
| Stop | `28` | `A8` |

Les bits `0x81` (« bits de trame », `0x80` ici) doivent être **recopiés tels que lus** dans `A0FC` ; ne pas les inventer : lire
`A0FC` avant la première écriture. Vérifier l'écriture en relisant `A0FC` (la réponse donne déjà la valeur relue). Délai : < 1 s.

### 6.2 Période vacances **[écriture non vérifiée sur chaudière réelle]**

`A0F0 – A0F7` contient les dates de début et de fin, chacune sur 32 bits et dans le même ordre de mots que l'horloge (`mot
faible d'abord`). Les deux valeurs sont nulles lorsqu'aucune période n'est programmée. L'interface Home Assistant accepte des
dates/heures locales, encodées comme l'horloge locale de la chaudière (sans appliquer d'offset UTC), et l'activation envoie
seulement ces quatre mots, sans réécrire le reste du bloc `A0F0 – A104` :

```
80 7E <assoc> <id> 01 17 A0 F0 00 04 A0 F0 00 04 08 00 <début:4> <fin:4>
```

Cette forme est déduite de l'échange `17` général et de l'encodage observé, mais **la trame d'écriture vacances et son effet
sur le chauffage / l'ECS n'ont pas encore été capturés sur une chaudière réelle**. Après l'accusé, l'implémentation relit
`A0F0 × 0D` et ne déclare l'opération vérifiée que si les deux valeurs correspondent. Deux zéros annulent la période.
Les notices Frisquet décrivent le mode vacances comme hors-gel sur chaque zone et arrêt de l'ECS ; l'effet réel de cette
écriture, l'interprétation été/hiver et le comportement exact à l'instant de début/fin restent à confirmer. Effectuer d'abord
un essai contrôlé avec une courte période et surveiller la chaudière.

### 6.3 Bloc de zone (48 octets) **[vérifié]**

| Octet | Champ | Codage |
|---|---|---|
| 0 | consigne confort | `(t − 5,0) × 10` : `96` = 20,0 °C ; 5,0 à 30,0 °C par 0,5 |
| 1 | consigne réduit | idem (`78` = 17,0) |
| 2 | consigne hors-gel | idem (`1E` = 8,0) ; contrainte : hors-gel ≤ réduit ≤ confort |
| 3 | mode | `05` Auto, `06` Confort, `07` Réduit, `08` Hors gel |
| 4 | options | bits ci-dessous |
| 5 | inconnu | `00` observé ; **ne jamais le modifier** |
| 6 – 47 | programme | 7 jours × 6 octets, **dimanche d'abord** ; 48 demi-heures par jour, bit 0 de l'octet 0 = 00:00 – 00:30 ; `1` = confort |

Bits de l'octet d'options :

| Bit | Sens |
|---|---|
| `0x01` | confort (période ou demande) |
| `0x02` | **dérogation** (Auto seulement) |
| `0x04` | Auto [MQTT] ; **valeur sans signification en lecture** : le satellite le met ou l'enlève à sa guise |
| `0x10` | hors gel |
| `0x20` | indicateur posé par le satellite (« compatible Connect ») ; à conserver |
| `0x40` | boost : consigne confort + 1,0 °C, posé par le Connect, 6 h au plus **[vérifié le 01/11]** : le satellite l'accepte, affiche 20 °C et garde `41` dans son bloc |

Valeur à écrire pour chaque choix (les autres bits sont conservés depuis le dernier bloc connu) :

| Choix | Mode | Options (bits `0x17`) |
|---|---|---|
| Auto | `05` | `04` + bit confort courant |
| Dérogation Confort | `05` | `07` (`04 | 02 | 01`) |
| Dérogation Réduit | `05` | `06` (`04 | 02`) |
| Confort | `06` | `01` |
| Réduit | `07` | `00` |
| Hors gel | `08` | `10` |

### 6.3 Écriture d'un bloc de zone **[vérifié]**

```
80 7E <assoc> <id> 08 17 A1 54 00 18 A1 54 00 18 30 <bloc:48>             (63 octets, réception = zone 08/09/0A)
```

Séquence observée sur l'air :

1. Le Connect émet des **rafales de 5 trames à 2,2 s** sous un même id ; sans réponse, une nouvelle rafale sous un **nouvel id**
   30 s plus tard. Abandon après ≈ 20 min.
2. La chaudière relaie chaque trame au satellite (`08 80 FA <id> 7E 17 A1 54 …`, réception `7E`).
3. Le satellite n'écoute qu'**autour de son créneau** (`:x4:37` sur l'installation de référence, toutes les 10 min) ; parfois plus
   tôt. Acceptation **entre quelques secondes et ≈ 10 min** après la demande.
4. **Écho** : `7E 80 <assoc> <id> 88 17 30 <bloc:48>` (55 octets) ; l'id peut être celui d'une rafale précédente. C'est la preuve
   que la chaudière l'a transmis.
5. Si le satellite **change effectivement** quelque chose (mode ou consigne **active**), il renvoie son bloc normalisé : relais
   `7E 80 <assoc> <id'> 08 17 A1 54 00 15 A1 54 00 18 30 <bloc>` (section 6.4), à accuser.

Règles indispensables :

- Le bloc **n'est pas lisible** : partir **toujours du dernier bloc relayé** par la chaudière, ne changer que le champ voulu, et
  conserver tous les autres octets (dont l'octet 5 et les bits d'options non concernés). Le persister (le Connect peut redémarrer).
- Après un redémarrage, ne pas écrire tant que le bloc mémorisé n'est pas confirmé par un relais ou par une diffusion cohérente
  (section 7.2) : un bloc périmé écraserait les réglages faits sur le satellite entre-temps.
- **L'écho ne prouve pas que le satellite applique la valeur** : une dérogation sans effet (Dérogation Confort pendant une période de
  confort) reçoit l'écho mais aucun relais, et la diffusion ne change pas.
- Le satellite normalise les options (`25` écrit → `21` relayé, `30` → `10`) : se resynchroniser sur le relais.
- Une seule écriture de zone à la fois ; regrouper plusieurs changements dans un même bloc.
- **Le réglage local l'emporte** : si la chaudière relaie pendant notre écriture un bloc différent de l'ancien et du nôtre, quelqu'un
  a changé la zone sur le satellite ; abandonner l'écriture et adopter le relais (sinon on écrase son réglage). Un relais égal à notre
  demande, aux bits d'options normalisés près (`0x04`, `0x20`), vaut confirmation.
- Un appui sur le bouton « info » du satellite le réveille : envoyer l'écriture en attente **à ce moment-là**, même pendant la fenêtre
  réservée aux satellites (sinon le réveil est perdu).

### 6.4 Relais de zone et accusé (obligatoire) **[vérifié]**

Quand un satellite écrit son bloc (réglage local, ou normalisation après notre écriture), la chaudière le relaie au Connect :

```
7E 80 <assoc> <id> 08 17 A1 54 00 15 A1 54 00 18 30 <bloc:48>             (63 octets)
```

Le Connect **doit accuser** par l'écho exact du bloc :

```
80 7E <assoc> <id> 88 17 30 <bloc:48>                                      (55 octets, même id, réception 08 | 80)
```

Sans accusé, la chaudière **répète le relais toutes les ≈ 4 s** et finit par signaler **« anomalie avec Connect »** (bit 0 de l'état,
vu après ≈ 11 h sans Connect actif). La chaudière relaie ensuite l'accusé au satellite (`08 80 FA <id> FE 17 30 <bloc>`).

**Minimum vital d'un Connect** : accuser ces relais. Lire, c'est du confort.

---

## 7. Données

Températures : mots signés en dixièmes de °C ; **`04F6` (127,0) et `050A` (129,0) = capteur absent**.

### 7.1 Réponse `79F2` (56 octets de données) **[vérifié]**

| Octets | Champ | Exemple |
|---|---|---|
| 0, 2, 4 | ambiante Z1, Z2, Z3 | `00 BA` = 18,6 °C |
| 6, 8, 10 | consigne de départ Z1, Z2, Z3 | `00 C6` = 19,8 °C |
| 12, 14, 16 | consigne d'ambiance active Z1, Z2, Z3 | `00 C8` = 20,0 °C |
| 18 | extérieure | `05 0A` = absente |
| 20 | ECS | `02 E6` = 74,2 °C |
| 22 | corps de chauffe | `03 0E` = 78,2 °C |
| 24, 26, 28 | départ Z1, Z2, Z3 | `01 20` = 28,8 °C |
| 30 | sonde de sécurité | `01 40` = 32,0 °C |
| 32 | fumées, **valeur ÷ 2** | `09 99` → 122,85 °C brûleur allumé ; `00 00` à l'arrêt |
| 34, 36 | puissance ECS, chauffage | toujours 0 ici |
| 40 | pression, ÷ 5120 bar | `1C 00` = 1,40 bar |
| 42 | état de fonctionnement | bits 0-1 : `01` radiateurs, `11` ECS |

C'est un **instantané** : les valeurs de zone changent à `:x3`, celles de la chaudière (dont les fumées) à `:x8`. Une chauffe
courte entre deux instantanés n'y laisse aucune trace.

### 7.2 Bloc d'état partagé `A029` (42 octets) **[vérifié]**

C'est la page commune lue par les satellites (leur « diffusion »), où chaque appareil écrit sa propre mesure.

| Octets | Mot | Champ |
|---|---|---|
| 0 | `A029` | température extérieure (écrite par la sonde) |
| 2 | `A02A` | inconnu (`0000`) |
| 4 – 9 | `A02B – A02D` | date BCD `AA MM JJ hh mm ss` |
| 10 | `A02E` (fort) | **état chaudière** : bit 0 anomalie, bit 2 chauffage arrêté, bit 3 demande de chauffage (`28`) / veille (`20`) |
| 11 | `A02E` (faible) | jour de la semaine, 0 = dimanche |
| 12 + 10 × (z − 1) | `A02F + 5 × (z − 1)` | **zone z** : ambiante (2), consigne active (2), inconnu (1, toujours `00`), **mode diffusé** (1), mot écrit par le satellite (`A032` : la consigne de départ lue à l'échange précédent, `00C6` au repos), puis la **consigne de départ** calculée par la chaudière (`A033` pour Z1 : ≈ 20 °C sans demande, 54 – 60 °C en demande) |

Le bit 3 de l'état suit la **demande de chauffage des zones**, pas la flamme : une flamme d'eau chaude le laisse à `20`.
**Ce qui déclenche la demande n'est pas encore connu** [01/11] :

- une consigne écrite au nom du satellite (Confort 19,0 pour une ambiante de 16,7, mode `05` ou `01`) **ne la lance pas**
  (plus de 8 min, puis 6 min : état `20`, consigne de départ 19,8 puis 23,2), tant que le bloc de zone de la chaudière dit Hors gel ;
- **[vérifié le 01/11, DESIGN 10.29]** elle démarre en quelques secondes quand la chaudière a un bloc Confort (écrit par le
  satellite, ou en son nom) **et** reçoit la consigne au nom du satellite avec le dernier mot (`A032`) à `0000`. Avec `00C6`
  (valeur habituelle du satellite) rien ne démarre ; le vrai satellite envoie `0000` juste après avoir accepté un bloc ;
  conditions supplémentaires : l'octet de mode doit être l'octet d'options du bloc (`01` en Confort, pas `05`), et la consigne
  doit arriver au moins ≈ 50 s après l'écriture du bloc ;
- elle apparaît à la **première trame du satellite qui suit son acceptation d'un bloc Confort** (31/10 : 4 min après ; 01/11 : moins
  de 40 s après la réécriture `A154` du satellite) ;
- une consigne Hors gel écrite au nom du satellite **la coupe** en moins de 25 s.

Hypothèse : la chaudière croise sa copie du bloc de zone (`A154`, illisible depuis le Connect) et la consigne du satellite. Une trame
reçue avec un CRC invalide le 01/11 à 13:30:05 portait `40` dans l'octet inconnu : c'était une erreur de bit (le CRC est juste avec `00`).

**Mode diffusé** (octet d'options normalisé par le satellite ; bit `0x04` à ignorer) **[vérifié le 29/10]** :

| Valeur (sans `0x04`) | Mode |
|---|---|
| `21` | Auto, période confort |
| `20` | Auto, période réduite |
| `23` | Auto, dérogation confort |
| `22` | Auto, dérogation réduit |
| `01` | Confort |
| `00` | Réduit |
| `10` (ou `30`) | Hors gel |

Toutes les 10 minutes (`:x0:08` pour Z1), et après chaque changement de zone, la chaudière envoie cette page au satellite
(`08 80 FA <id> 81 17 2A <42 octets>`, 49 octets) : c'est la réponse à l'échange périodique du satellite (section 9). Un Connect
peut la lire directement en `A029 x15`, à tout moment.

**Vérifier un bloc mémorisé** avec cette page : le mode et la consigne active doivent correspondre ; en Auto, la période (confort ou
réduite) doit correspondre au programme à l'heure de la page (éviter les deux minutes autour d'une demi-heure).

### 7.3 État du brûleur **[vérifié les 31/10/2026]**

**Aucune trame ni aucune donnée lisible n'indique la flamme en direct.** Deux sources partielles :

- **Fumées** (`7A02`, dans `79F2`, instantané vers `:x7` ; `79E6`, instantané vers `:x2` ; valeur ÷ 20) : ≈ 122 °C flamme allumée,
  `0` éteinte. Exactes, mais deux fois par 10 minutes seulement.
- **Corps de chauffe en direct** (`A05E`) : puisage d'eau chaude sans flamme, baisse de ≈ 1,5 °C / 14 s ; flamme, hausse de 2 à
  3,5 °C / 12-15 s (≈ 30 s d'inertie) ; au repos, pas de mesure de ≈ 0,5 °C. **Inutilisable en chauffage** : avec la pompe vers les
  radiateurs, il oscille de 15 à 40 °C en 1 à 3 minutes alors que la flamme reste allumée ; et il **monte encore** une minute
  après l'arrêt de la pompe en fin de demande.

Règle retenue : **demande de chauffage** (bit `0x08` de l'état `A029`) ⇒ brûleur = dernier instantané de fumées > 40 °C ;
**sans demande** (le brûleur ne sert qu'à l'eau chaude) ⇒ brûleur = hausse du corps de chauffe ≥ 1,0 °C / 15 s (éteint à ≤ 0,1 °C).

### 7.4 Consommations

`7A18` (journalier, mots 9 et 10), `A063` / `A07B` (mensuel, année courante / précédente) existent mais valent 0 sur la chaudière
de référence : elle ne les compte pas. Le format est celui du firmware MQTT (24 mots : chauffage puis ECS pour chaque mois).

---

## 8. Carte mémoire lisible **[vérifié, balayage mot à mot]**

Dans `7800 – 7BFF`, `9C00 – 9DFF` et `A000 – A2FF`, seuls ces mots se lisent (352 au total) :

| Plage | Contenu |
|---|---|
| `7919 – 7965` | `791A – 791B` NetworkID ; `791C…` **table des associations** (`FA00`, `FA08` = satellite Z1, `D57E` = Connect, `FFFF` = libre) ; **`7931` version du panneau** (BCD, `1901` = V19.01) ; `793A – 793B` `AE80 66D3` (même codage que `7A16` : 2024-09-01) ; `793E – 7956` paramètres par circuit, dont **`7946` = départ mini / maxi de la zone 1** (`143C` = 20 / 60 °C) **[vérifié le 01/11]** |
| `79E0 – 7A4F` | `79E0` et `79FC` : instantanés (`79EC` = `0320`, 80,0 constant : une consigne, pas l'ECS instantanée) ; `79F2` (7.1) ; **`7A16 – 7A17` horloge** : heure Unix sur 32 bits, **mot faible d'abord**, heure locale codée comme UTC ; `7A18` consommations ; **`7A30 – 7A33` appareils appairés** : deux fois (index, adresse) puis version BCD, ici `0308 2700 FF7E FFFF` = satellite `08` V27.00, Connect `7E` sans version (même après une association avec version, 3.3) ; reste `FFFF` |
| `9C40 – 9C5A` | `9C54 – 9C5A` : date et heure en direct, état et jour, NetworkID, `0080` |
| `A029 – A09B` | `A029` page d'état (7.2) ; `A041` copie décalée ; **`A05E` corps de chauffe en direct** ; `A063 – A09A` consommations |
| `A0F0 – A104` | **`A0F0 – A0F7` début / fin des vacances** (deux horodatages 32 bits, mot faible d'abord ; interprétation [hypothèse]) ; `A0F8 – A0F9` identifiant `AE80 66D3` ; **`A0FC` mode ECS** |

---

## 9. Les autres appareils

| Appareil | Échange | Fréquence |
|---|---|---|
| Satellite zone z | lit `A029 x15`, écrit `A02F + 5 × (z − 1)` : 1 mot (ambiante) ou 4 mots (ambiante, consigne, mode) [MQTT] ; écrit son bloc de zone (`A154`) lors d'un réglage local [vérifié] | toutes les 10 min, Z1 à `:x0`, Z2 à `:x1`, Z3 à `:x2` [MQTT] |
| Sonde extérieure (`20`) | lit `9C54 x4` (date), écrit `A029 x1` (température extérieure) [MQTT] | toutes les 10 min à `:x4` [MQTT] |

**Trames du satellite [vérifié le 01/11/2026]** : reçues depuis que le satellite a été déplacé (31/10, ≈ −100 dBm ; jamais reçues
avant). Échange régulier toutes les 10 min à `:x0:05`/`:x0:06` :

```
80 08 FA <id> 01 17 A0 29 00 15 A0 2F 00 04 08 <ambiante:2> <consigne:2> 00 <mode> 00 C6     (23 octets, 4 mots)
80 08 FA <id> 01 17 A0 29 00 15 A0 2F 00 01 02 <ambiante:2>                                  (17 octets, ambiante seule)
```

- `mode` = octet d'options du bloc (`21` Auto confort, `10` hors gel) ; consigne en dixièmes (`00BE` = 19,0, `0050` = 8,0).
- Satellite **réveillé** (bouton, réglage local, prise en main) : échanges courts « ambiante seule » toutes les 1 à 10 s pendant
  ~30 s, puis retour au rythme de 10 min. L'ambiante n'est donc mise à jour qu'à ce rythme hors réveil.
- `+` / `−` en Auto modifient la **consigne confort du bloc** (relais `08 → 80`, mode `05`, options `21`, en ≈ 3 s), pas une
  dérogation. Une modification de programme est relayée de même (≈ 3 s).
- La nuit (≈ 01:05) : lectures `791A`, `7932`, `794A` x18 par le satellite.

**Consigne directe [vérifié le 31/10/2026]** : un Connect peut faire lui-même l'échange du satellite, **en son nom** (source `08`,
association du satellite, ici `FA`) :

```
80 08 FA <id> 01 17 A0 29 00 15 A0 2F 00 04 08 <ambiante:2> <consigne:2> 00 <mode> <mot suivant:2>     (23 octets)
réponse : 08 80 FA <id> 81 17 2A <page A029 : 42 octets>
```

Recopier l'ambiante de la page `A029` lue juste avant. La chaudière l'applique **immédiatement** (réponse en 0,1 s, page `A029` à
jour), jusqu'au prochain **échange régulier** du satellite (`:x0`), qui remet sa propre consigne ; les échanges intermédiaires
(ambiante seule) ne la modifient pas. Pour un changement durable : consigne directe **et** bloc de zone au satellite (6.3), en
réaffirmant la consigne directe après chaque échange régulier tant que le bloc n'a pas abouti.

**Démarrer le chauffage tout de suite [vérifié le 01/11/2026]** : la consigne seule change l'affichage, et une consigne plus basse
que l'ambiante **arrête** aussitôt la demande, mais elle ne la **lance** pas. Pour la lancer, comme le satellite après un réglage :

1. Écrire le bloc de zone **au nom du satellite**, comme sa réécriture après un réglage local :
   `80 08 FA <id> 7E 17 A1 54 00 15 A1 54 00 18 30 <bloc:48>` (63 octets). La chaudière le garde, le transfère au Connect
   (`7E 80 <assoc> <id> 08 17 A1 54 00 15 …`, à accuser comme un relais, 6.4), puis répond `08 80 FA <id> FE 17 30 <bloc>`.
2. Envoyer la consigne au nom du satellite avec : **mode = octet d'options du bloc** (`01` en Confort ; `05` ne démarre rien) et
   **dernier mot (`A032`) = `0000`** quand la consigne dépasse l'ambiante (`00C6` sinon).
3. La chaudière n'en tient compte qu'**environ 50 s après** l'écriture du bloc : renvoyer la consigne vers 35 s et 70 s.

La demande (état `28`) suit en quelques secondes ; la consigne de départ (`A033`) passe de ≈ 20 °C à 53 – 60 °C ; la flamme
1 à 2 min plus tard. `A032` est en fait l'écho, par le satellite, de la consigne de départ lue à l'échange précédent (`021F`
= 54,3 après une chauffe, `00C6` = 19,8 au repos). Le vrai satellite garde son propre bloc jusqu'à ce qu'il accepte l'écriture
normale (6.3) ; avant, sa consigne de `:x0` peut couper la demande, qu'il faut alors réaffirmer.

**Ne jamais remplacer un satellite muet** : sans trame du satellite pendant ≈ 20 min, la chaudière se met en défaut et coupe le
chauffage (hors-gel compris) ; des trames en son nom masqueraient cette sécurité. N'émettre en son nom que si sa propre trame (ou la
réponse de la chaudière à celle-ci, `08 80 <assoc> <id> 81 17 2A …`) a été reçue récemment (25 min ici).

**Alertes du satellite [d'après captures externes]** : type **`43`**, texte en ASCII terminé par `00` :
`80 08 00 B4 01 43 00 1C "SATELLITE Z1\0CHANGER PILES \0"`, accusé `08 80 00 B4 81 43` ; « aucune alerte » = `43 00 00`
(au démarrage du satellite, et chaque jour à 04:00 par la sonde extérieure).

---

## 10. Calendrier et temporisations

| Quand | Qui | Quoi |
|---|---|---|
| `:x9:30` – `:x2:30` | satellites | **fenêtre réservée** : le Connect n'émet que ses accusés |
| `:x0:08` | chaudière → satellite Z1 | réponse d'état (page `A029`) |
| `:x3`, `:x8` | chaudière | rafraîchissement de l'instantané `79F2` |
| `:x4:37`, `:x9:14` | satellite Z1 | écoute des écritures de zone (créneaux observés ici, propres à l'installation) ; aussi juste après un appui sur sa touche « i » |
| `:03` à `:09`, chaque minute | Connect (ce dépôt) | lecture `A029` (horloge, état) |
| toutes les 15 s hors fenêtre | Connect (ce dépôt) | lecture `A05E` (brûleur) |
| `:x3`, `:x8` | Connect | lecture `79F2` (et `79E6` à `:x3`, fumées de l'instantané de `:x2`) |
| `:05` de l'heure | Connect | `7A18`, `A063` |
| `:07` de l'heure | Connect | `A0FC` |

L'heure de référence est celle de la chaudière (lue dans `A029` ou dans la page envoyée au satellite). Une tentative par créneau,
sans rattrapage.

---

## 11. Pièges (tous rencontrés)

1. Ne pas accuser les relais de zone ⇒ « anomalie avec Connect » au bout de quelques heures.
2. Rester sur le réseau `FF FF FF FF` après une association ratée ⇒ plus aucune communication.
3. Écrire un bloc de zone reconstruit ou périmé ⇒ écrase programme et consignes du satellite. Toujours partir du dernier relais.
4. Prendre l'écho d'une écriture de zone pour une preuve d'application.
5. Décoder le mode diffusé avec le bit `0x04` (il varie sans raison) ; `05` sans `0x20` est **Confort**, pas « Auto confort ».
6. Croire que `79F2` est en direct : c'est un instantané (fumées comprises).
7. Lire par grands blocs une zone mémoire inconnue : un seul mot invalide fait refuser tout le bloc.
8. Inventer les bits `0x81` du mode ECS au lieu de les relire.
9. Après une ré-association, l'octet d'association change : l'ancien Connect (ou firmware) ne fonctionne plus sans être réassocié.
10. Mesurer le RSSI après une capture de longueur fixe.

---

## 12. Autres implémentations et divergences

**[frisquet-bridge](https://github.com/markkvdb/frisquet-bridge)** (MIT, Python + modem RFM69, même chaudière Visio) apporte des
observations faites sur d'autres installations, non vérifiées ici (**[bridge]**) :

- **Poussées de la chaudière vers le Connect**, quand on change un réglage **sur le panneau de la chaudière** : réception `05`,
  type **`10` (MEMORY_PUSH)** (`adresse:2 | taille:2 | données`, comme une réponse de lecture ; vu pour `A0F0`) ou **`45`
  (BOILER_EVENT)** (21 octets `type | étiquette:3 | horodatage:4 | 13 octets`, étiquette `0A <appareil> <paramètre>`, émis 3 fois ;
  vu au changement de la température maximale). Le Connect officiel les accuse : adresses permutées, même id, **réception `85`**,
  même type, données vides (ou `adresse + taille` pour `10`). **`10` vérifié ici le 01/11/2026** : changement du mode ECS au
  panneau → `7E 80 D5 <id> 05 10 A0 F0 00 0D 1A <26 octets>` (zéros, `AE80 66D3` en `A0F8`, **mode ECS en `A0FC`**, dernier mot :
  `0080` Max, `0090` Eco Horaires). Sans accusé, répété 4 fois à 1 s (nouvel id) puis abandonné ; avec l'accusé
  `80 7E D5 <id> 85 10 A0 F0 00 0D` (même id, envoyé en 10 ms), une seule émission. `45` jamais observé.
- Autres types : `43` (le bridge l'appelle SONDE_INIT : c'est en fait une **alerte texte**, voir 9 ; `43 00 00` = aucune
  alerte), `44` (émis par le Connect, données `0100`/`0200`, sens inconnu). Chaudière à l'adresse **`84`** sur certaines installations (alors adresses mémoire `+ 0xC8`).
- `79E0` (bloc « capteurs » complet) : en plus de ce qui figure en 7.1, **puissance ECS / chauffage** (mots 7 et 8), **ECS
  instantanée** (mot 12, `79EC`), ambiantes, consignes de départ et consignes des zones, extérieure. Rafraîchi au rythme propre de la
  chaudière (≈ 5 min, plus souvent brûleur allumé). `7A34` = consommation totale chez eux (illisible, `FFFF`, ici).
- Le satellite se réveille plus souvent (toutes les deux minutes environ) pendant qu'on modifie la zone, et **un appui sur son bouton
  « info » le réveille**, ce qui fait aboutir aussitôt une écriture de zone en attente.
- Id de message du Connect officiel incrémenté de **4** à chaque requête.
- **Pilotage par satellite virtuel** : prendre le rôle du satellite (`08`, son association) et écrire la consigne `A02F` directement
  (réponse immédiate, pas d'attente de réveil), avec l'ambiante d'un capteur extérieur ; le satellite physique doit alors être retiré.
- **Température de départ imposée** : chaudière en régulation « extérieure seule », la passerelle envoie (comme sonde `20`) la
  température extérieure virtuelle pour laquelle la courbe de chauffe donne le départ voulu.

**Divergence sur l'octet d'options** : le bridge écrit `10` (Auto suivant le programme), `20` (hors gel), `24` (réduit fixe),
`25` (confort fixe), une dérogation étant « mode `05` + options fixes », **sans bit dérogation**. Ici, les écritures « dérogation »
`…06` / `…07` (bit `0x02`) ont été acceptées et appliquées, diffusées `26` / `27`, et la normalisation du satellite (`25` → `21`,
`30` → `10`) ne correspond pas à leur table. Les deux codages semblent acceptés ; partir du dernier relais reste la règle sûre.

---

## 13. Inconnu ou non vérifié

- Poussée `45` (BOILER_EVENT) : jamais observée ici ; poussées `10` d'autres adresses que `A0F0`.
- Trames de la sonde extérieure et des satellites Z2/Z3 (formats [MQTT] seulement).
- Zones 2 et 3, chaudières d'une autre famille (`+ 0xC8`).
- Version du Connect dans la chaudière : `7A30` ne change pas après une association avec version (3.3).
- Octet 5 du bloc de zone, octets inconnus de la page `A029`, paramètres `793E – 7956`, identifiant `AE80 66D3`.
- Détection du brûleur en chauffage plus rapide qu'un instantané de fumées toutes les 5 minutes.
- Codes d'anomalie de la chaudière (seul le bit « anomalie » est connu).
