# Sonde extérieure issue de Home Assistant (expérimental)

Le composant peut émuler une sonde extérieure Eco Radio Visio (`20`) en plus du Connect (`7E`), avec une association
distincte sauvegardée en flash sur le même réseau. La source prévue ici est
`sensor.timmerflotte_temp_hmd_sensor_temperature_5`, qui doit mesurer la température **réellement extérieure**, en °C.
Cette fonction modifie la régulation de chauffage. Le modèle exact de chaudière et sa compatibilité restent à confirmer ;
le protocole de sonde provient de recherches externes, pas d'un essai sur notre installation.

## Garde-fous et limites

- Désactivée par défaut (`outdoor_emulation: false`), association uniquement sur appui explicite.
- Nécessite `tx: write`, `pairing: true`, un Connect déjà associé, une mesure valide, sans arrêt d'urgence ni dry-run.
- Mesure attendue chaque minute, alerte dans le capteur d'état et les logs après **3 minutes** sans mesure fraîche.
- Dernière mesure conservée au maximum **180 minutes à partir de sa réception par HA**, pas du dernier appel API.
  Une température identique mais réellement rapportée à nouveau est fraîche. Le champ HA `last_reported` est utilisé,
  et non `last_changed`. Vérifier que l'intégration du capteur rapporte effectivement chaque minute.
- Après 180 minutes, plus aucune émission de sonde, y compris les retransmissions en cours. **Un défaut chaudière et
  un arrêt du chauffage sont possibles**, y compris de la protection antigel : ce n'est pas un mode autonome.
- Valeurs non finies, hors de −40 à +60 °C, dates futures et mesures déjà périmées refusées. Aucune valeur fictive de secours.
- Après redémarrage de l'ESP, aucune température n'est restaurée : une mesure valide doit revenir via HA. L'association,
  elle, est restaurée. Un redémarrage HA peut modifier les métadonnées de fraîcheur de l'intégration source : contrôler
  que celle-ci ne republie pas une ancienne valeur comme une nouvelle mesure.
- Une seule sonde extérieure par chaudière ; ne pas activer en parallèle d'une sonde physique.
- Émission au créneau `:x4`, toutes les 10 minutes ; pas de rafale de rattrapage.
- L'état « chaudière répond » confirme une réponse radio, pas la pertinence de la mesure ni la courbe de chauffe.
- Les éventuels échanges de maintenance spécifiques de la sonde physique (dont ceux signalés à 04:00 dans les recherches)
  ne sont pas implémentés. La compatibilité et le fonctionnement prolongé doivent être validés sous surveillance.

## Installation

1. Confirmer le modèle et préparer le retour arrière sur le panneau chaudière : pouvoir **supprimer la sonde extérieure**
   et rétablir les réglages de régulation antérieurs. Ne pas supprimer le Connect.
2. Copier [outdoor-sensor-esphome.yaml](../homeassistant/outdoor-sensor-esphome.yaml) à côté du YAML ESPHome de l'appareil.
   Ajouter dans celui-ci :

   ```yaml
   packages:
     outdoor_sensor: !include outdoor-sensor-esphome.yaml
   ```

   Conserver le composant principal avec `id: frisquet_component`. Ne pas remplacer ses autres paramètres, les secrets
   ou le nom de l'appareil. Le package fournit l'action API, le bouton d'association et le capteur d'état.
3. Après confirmation de compatibilité seulement, ajouter `outdoor_emulation: true` à la section `frisquet:` existante.
   Compiler et installer avec le composant mis à jour. Ajouter seulement le package sans mettre à jour le composant
   ne suffit pas.
4. Créer une automatisation HA avec [outdoor-sensor-automation.yaml](../homeassistant/outdoor-sensor-automation.yaml).
   Adapter **l'action** `esphome.frisquet_outdoor_temperature` à celle réellement proposée dans Outils de développement >
   Actions (elle dépend du nom de l'appareil, par exemple `esphome.frisquette_outdoor_temperature`).
   L'automatisation transmet la valeur et son âge réel, même si la valeur reste constante ; elle ne transmet rien lorsque
   la source est inconnue ou indisponible. L'ESP continue alors à faire vieillir la dernière mesure valide.
5. Attendre une mesure valide, puis sur la chaudière choisir **Ajouter une sonde extérieure**, pas « Remplacer Connect ».
   Appuyer sur **Associer la sonde extérieure** et terminer la procédure du panneau. L'association du Connect est conservée.
6. Vérifier la température extérieure affichée par la chaudière, le capteur d'état et les réponses dans les logs au créneau
   `:x4`. Les réglages de courbe de chauffe et de régulation dépendent du modèle ; ne pas les déduire du seul appairage.
7. Ajouter au dashboard le capteur d'état réel (préfixe de pièce possible), par exemple :

   ```yaml
   type: entities
   title: Sonde extérieure émulée
   entities:
     - entity: sensor.timmerflotte_temp_hmd_sensor_temperature_5
       name: Mesure extérieure HA
     - entity: sensor.cuisine_frisquet_sonde_exterieure_emulee_etat
       name: Transmission à la chaudière
   ```

Le capteur d'état expose les alertes ; une notification HA peut être configurée sur ses états commençant par `ALERTE`.
HA ne peut évidemment pas notifier pendant sa propre panne. Tester les coupures avec surveillance et possibilité
d'intervention sur la chaudière ; ne pas considérer la protection antigel comme assurée par ce dispositif expérimental.

## Retrait / retour arrière

Supprimer d'abord la **sonde extérieure** dans les partenaires radio de la chaudière et rétablir la régulation antérieure,
puis désactiver l'émulation et l'automatisation. Couper seulement le Wi-Fi, l'ESP ou le flag d'émulation laisse un partenaire
silencieux déclaré à la chaudière, avec le risque de défaut indiqué plus haut.
