# Plusieurs plannings avec leurs températures

L'onglet **Plannings** permet de conserver plusieurs **profils nommés dans Home Assistant**, partagés entre les navigateurs et restaurés
au redémarrage de HA. Chaque profil contient une semaine et trois consignes : Confort, Réduit et Hors-gel.
Les noms sont limités à 40 caractères. Quatre emplacements sont proposés ; ajoutez des helpers pour en avoir davantage.

La chaudière ne possède qu'un programme actif. **Charger** un profil dans la carte ne l'active pas : le bouton
**Appliquer le programme et les températures** envoie l'ensemble en une seule écriture du bloc de zone.
Le mode actuel, les dérogations et les octets inconnus sont conservés. Le programme horaire agit seulement en **Auto**.
En mode Confort / Réduit / Hors-gel permanent, la nouvelle consigne de ce mode peut agir immédiatement.
La carte ne lance aucune automatisation périodique HA : le programme reste exécuté par la chaudière.

## 1. Stockage dans HA

Copier [program-profiles-helpers.yaml](../homeassistant/program-profiles-helpers.yaml) dans
`/config/packages/frisquet_program_profiles.yaml`. Si les packages HA ne sont pas déjà activés, fusionner ceci dans
le bloc `homeassistant:` existant de `/config/configuration.yaml`, sans créer de seconde clé `homeassistant` :

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Valider la configuration puis redémarrer HA. Les quatre entités sont `input_text.frisquet_planning_1` à
`input_text.frisquet_planning_4`. Ne pas définir `initial` : HA doit restaurer les profils enregistrés.
Autre possibilité : créer quatre helpers Texte dans l'interface HA, avec **255 caractères maximum**, et utiliser
leurs identifiants réels dans la carte. Leur contenu est du JSON : ne pas le modifier à la main.
Les sauvegardes HA et les droits des utilisateurs sur les helpers s'appliquent.

## 2. Commande atomique ESPHome

Il faut mettre à jour le composant et **reflasher l'ESP** pour ajouter la commande combinée. Envoyer quatre commandes
séparées (programme et trois températures) n'est pas équivalent : elles peuvent se remplacer pendant l'attente radio.

Copier [program-profiles-esphome.yaml](../homeassistant/program-profiles-esphome.yaml) à côté du YAML ESPHome de l'appareil,
puis ajouter ou fusionner :

```yaml
packages:
  program_profiles: !include program-profiles-esphome.yaml
```

Le composant principal doit avoir `id: frisquet_component` et `tx: write`. Conserver les secrets, le nom de l'appareil et
les autres réglages. Si les sources GitHub sont mises en cache, demander un rafraîchissement du composant
(`refresh: always` sur l'entrée `external_components`) avant la compilation.

Après installation, chercher l'action `esphome.<nom_appareil>_apply_heating_profile` dans Outils de développement > Actions.
Par exemple `esphome.frisquette_apply_heating_profile`. La carte doit utiliser le nom réellement exposé par HA.
Cette commande ne dépend pas du package optionnel de sonde extérieure et ne l'active pas.

## 3. Carte

Remplacer le JavaScript dans `/config/www/frisquet-program-card.js`, puis changer l'URL de ressource en
`/local/frisquet-program-card.js?v=4` (**Module JavaScript**) et recharger complètement le navigateur.
La carte affiche une ligne compacte par jour, avec les changements d'horaire sous la bande, sans répéter 00:00 et 24:00.
Pour les journées très fragmentées, des repères 06:00, 12:00 et 18:00 remplacent les horaires qui se chevaucheraient ;
les périodes exactes restent accessibles en ouvrant la journée. Cliquer sur le jour ouvre son édition ;
les explications détaillées restent accessibles dans **Aide et fonctionnement**, repliée par défaut.

Le [dashboard complet](../homeassistant/dashboard.yaml) comporte deux onglets : **Chauffage** pour charger et activer un
profil, **Plannings** pour le gérer sans commande chaudière. Adapter l'action ESPHome et les identifiants dans **les deux
cartes**. Pour l'installation utilisant le nom d'appareil `frisquette`, l'action
peut être `esphome.frisquette_apply_heating_profile` ; les entités existantes peuvent garder le préfixe `frisquet_`.

Pour adapter un dashboard existant, utiliser [program-profiles-card.yaml](../homeassistant/program-profiles-card.yaml)
dans l'onglet Chauffage, puis copier cette carte dans un nouvel onglet Plannings avec `profile_management: true`.
Cette option affiche le nom, les températures, l'enregistrement et la suppression, mais interdit l'activation chaudière.
Sans cette option, la carte permet le chargement et l'activation, sans modification des profils sauvegardés.
Sans les options `profiles`, `profile_action` et les trois `*_entity`, la carte reste un éditeur de programme simple.

## Utilisation

1. Dans **Plannings**, choisir un emplacement vide. Modifier les journées, donner un nom, et choisir les trois températures.
2. **Enregistrer le profil dans HA** sauvegarde le brouillon dans cet emplacement, sans toucher à la chaudière.
   Une confirmation est requise pour remplacer un emplacement déjà rempli.
3. Dans **Chauffage**, pour changer de profil : choisir l'emplacement, cliquer **Charger le profil**, vérifier puis **Appliquer le programme
   et les températures**. La confirmation rappelle que le mode est conservé.
4. Attendre la publication du programme **et des trois consignes** par l'ESP. La carte ne considère pas le succès de
   l'appel API comme une confirmation chaudière. Consulter l'état d'écriture (ou les logs si le capteur n'est pas configuré).
5. **Supprimer le profil** efface seulement sa sauvegarde HA ; le programme de la chaudière reste inchangé.

Les limites restent : pas de 0,5 °C, 5–30 °C pour Confort / Réduit, 5–15 °C pour Hors-gel, avec
Hors-gel ≤ Réduit ≤ Confort. Aucune valeur n'est arrondie ou remplacée silencieusement.
Un changement externe du programme ou des températures pendant l'édition bloque l'activation jusqu'à
**Annuler les modifications**. L'édition d'une journée doit être validée avant de sauvegarder ou d'appliquer un profil.

Une activation est refusée si un Boost ou une écriture de zone est en cours, si le bloc n'est pas encore confirmé,
si les écritures sont désactivées, en dry-run ou avec l'arrêt d'urgence. Désactiver le Boost ou attendre la fin de l'écriture
avant de réessayer. L'application peut prendre jusqu'au prochain échange du satellite.
Le nom du profil n'est pas enregistré dans la chaudière : une édition manuelle de ses paramètres ne modifie pas les
profils sauvegardés, et le menu des emplacements n'est pas un indicateur du « profil actif ».
