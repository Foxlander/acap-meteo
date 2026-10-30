# ACAP Météo Caméra

Application ACAP (Axis Camera Application Platform) qui estime la météo
actuelle à partir de l'image de la caméra : **ensoleillé, nuageux, brouillard,
pluie, neige ou nuit**.

## Principe

Toutes les `IntervalSeconds` secondes (5 min par défaut) :

1. **Rafale d'images** : l'ACAP ouvre un flux YUV 640x360 via l'API VDO et
   capture `FrameCount` images espacées de `FrameSpacingMs` ms.
2. **Moyenne** : chaque image est réduite à une grille 160x90, puis on fait la
   moyenne. Les éléments fugaces (gouttes, flocons, passants) disparaissent et
   il reste la scène de fond, sur laquelle on mesure :
   - la luminosité moyenne et le contraste,
   - la saturation des couleurs (≈ 0 quand la caméra passe en mode IR la nuit),
   - la densité de contours et la netteté (le brouillard les écrase),
   - la luminosité et la couleur bleue du ciel (haut de l'image, `SkyRatio`).
3. **Différences entre images** : les petits points isolés qui changent d'une
   image à l'autre (« scintillement ») trahissent la pluie ou la neige. Les
   grosses zones en mouvement (voitures, piétons) sont ignorées.
4. **Classification** par seuils, dans cet ordre : nuit → pluie/neige →
   brouillard → ensoleillé → nuageux.
5. **Lissage dans le temps** : vote majoritaire sur les `HistorySize`
   dernières rafales, pour éviter qu'un nuage qui passe change le résultat.

## Résultats

- **Page web** de l'application (dans l'interface de la caméra, *Apps* →
  *Meteo Camera* → *Open*) avec l'icône météo et tous les indicateurs.
- **JSON** : `http://<ip-camera>/local/meteo/status`

  ```json
  {"timestamp":1793361600,"condition":"cloudy","label":"Nuageux",
   "instant":{"condition":"cloudy","label":"Nuageux","confidence":0.79},
   "precipitation":false,
   "features":{"frames":8,"luma_mean":135.0,"luma_std":51.0,"saturation":5.96,
               "edge_density":0.3769,"sharpness":39.85,"sky_luma":185.0,"sky_blue":0.00,
               "sky_saturation":0.00,"temporal_noise":0.00,"temporal_specks":0.00000}}
  ```
  `condition` est la valeur lissée, `instant` la dernière mesure seule.
- **Syslog** (*System* → *Logs*) : une ligne par mesure, et une ligne
  `Météo : …` à chaque changement.
- **Événement Axis** *Météo Caméra → Météo* (événement à état), envoyé à chaque
  changement de la météo lissée. Il contient `condition` (texte) et un booléen
  par condition (`sunny`, `cloudy`, `fog`, `rain`, `snow`, `night`). Dans
  *System* → *Events* → *Rules*, on peut par exemple choisir « Météo » comme
  condition avec `rain` actif, puis une action (e-mail, MQTT, sortie, preset
  PTZ…).

## Compiler

Il faut Docker. La cible par défaut est `aarch64` (ARTPEC-8, ARTPEC-9, CV25…) :

```sh
docker build --tag meteo .
docker cp $(docker create meteo):/opt/app ./build
# => build/Meteo_Camera_0_2_0_aarch64.eap
```

Pour une caméra ARTPEC-7 ou plus ancienne (32 bits) : `docker build --build-arg ARCH=armv7hf --tag meteo .`

Pour connaître l'architecture de ta caméra, ouvre
`http://<ip-camera>/axis-cgi/basicdeviceinfo.cgi` (ou regarde le champ
*Architecture* / *Soc* dans *System* → *Plain config*) : `aarch64` ou `armv7hf`.

## Installer

Interface web de la caméra → *Apps* → *Add app* → choisir le fichier `.eap`,
puis démarrer l'application. Sur AXIS OS 11.x/12.x, il peut falloir activer
*Allow unsigned apps*.

Ou en ligne de commande depuis le conteneur du SDK :

```sh
docker run --rm meteo eap-install.sh <ip-camera> <mot-de-passe> install
```

## Calibrer

Les seuils par défaut sont des points de départ : chaque scène est différente
(cadrage, présence de ciel, éclairage public…). Méthode conseillée :

1. Laisser tourner l'application quelques jours.
2. Noter les `features` du JSON par temps clair, couvert, brouillard, pluie.
3. Ajuster les paramètres dans *Apps* → *Meteo Camera* → *Settings* (ou via
   VAPIX `param.cgi`, groupe `meteo`).

| Paramètre | Défaut | Rôle |
|---|---|---|
| `IntervalSeconds` | 300 | Délai entre deux rafales |
| `FrameCount` | 8 | Images par rafale |
| `FrameSpacingMs` | 500 | Espacement des images d'une rafale |
| `HistorySize` | 3 | Rafales prises en compte pour le vote |
| `Channel` | 1 | Capteur vidéo (caméras multi-capteurs) |
| `SkyRatio` | 0.3 | Part haute de l'image considérée comme ciel (0 à 1) |
| `NightLuma` | 35 | Luminosité moyenne en dessous de laquelle il fait nuit |
| `IrSaturation` | 0.5 | Saturation en dessous de laquelle l'image est en mode IR (nuit) |
| `FogEdgeDensity` | 0.04 | Densité de contours en dessous de laquelle il y a brouillard… |
| `FogContrast` | 28 | …si le contraste est aussi en dessous de ce seuil |
| `PrecipSpecks` | 0.004 | Taux de scintillement au dessus duquel il pleut ou neige |
| `SnowLuma` | 150 | Précipitation + image plus claire que ça et peu saturée = neige |
| `SunnySkyLuma` | 170 | Ciel plus lumineux que ça… |
| `SunnySaturation` | 14 | …et couleurs plus saturées que ça = ensoleillé |

## Tests

L'algorithme (`app/weather.c`) ne dépend pas du SDK Axis et se teste sur PC
avec des scènes synthétiques :

```sh
make -C tests check
```

## Pistes d'amélioration

- **Modèle de deep learning** sur les caméras avec DLPU (ARTPEC-8/9, CV25) :
  un petit classifieur (MobileNet entraîné sur un jeu type *Multi-class Weather
  Dataset*, puis sur des images de la caméra) exécuté via l'API **Larod**.
  Plus robuste que les seuils, mais il faut des images annotées.
- **Croisement avec une API météo** (Open-Meteo, Météo-France) pour valider ou
  corriger l'estimation, et pour constituer automatiquement un jeu
  d'entraînement.
- **Masque de zone** pour exclure une route très passante ou un arbre qui bouge.

## Structure

```
Dockerfile           Compilation avec l'ACAP Native SDK 12.2
app/manifest.json    Description de l'ACAP, paramètres, reverse proxy HTTP
app/meteo.c          Capture VDO, paramètres, serveur HTTP, syslog, événements
app/weather.c/.h     Algorithme (moyenne, indicateurs, classification)
app/html/index.html  Page web de l'application
tests/               Tests sur PC
```
