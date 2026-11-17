# vinylclean — nettoyage des vinyles en temps réel sur un Raspberry Pi

*[English version: README.md](README.md)*

vinylclean enlève les clics et les craquements de tes disques **pendant l'écoute**, sur un Raspberry Pi placé
entre la platine et l'ampli. Il se règle depuis une page web, sur ton téléphone ou sur un écran tactile.

```
Platine → (préampli phono) → carte son USB → Pi : vc-live → sortie de la carte son → ampli
                                                ↑ page de réglage : téléphone, ordinateur ou écran tactile
```

**Fonctions**

- **Déclick** (gros clics) et **dé-crackle** (petits craquements serrés), sensibilité **0 à 5** sur un seul curseur
  (réglages séparés possibles).
- **Protection de la musique** (aucune / normale / maximale) : frappes de batterie, notes pincées et sons
  périodiques sont épargnés.
- **Réparation multibande** : seules les bandes de fréquences où le clic domine sont réparées.
- **Canaux stéréo liés** : un clic d'un seul côté est plus facile à reconnaître, et les longs clics sont
  reconstruits avec l'aide de l'autre canal.
- **Préampli phono intégré** (correction RIAA numérique à ±0,012 dB) pour une platine branchée sans préampli.
- **Enregistrement** de tes disques (son brut ou nettoyé) en **FLAC ou WAV**, 16 / 24 bits ou 32 bits flottant, à la
  fréquence de ton choix, avec boutons Télécharger et Supprimer.
- **Anti-distorsion de fin de face** : tu indiques la longueur de ton bras, ton alignement (Stevenson, Baerwald,
  Löfgren B ou sur mesure) et ta pointe ; le logiciel calcule et retire la distorsion de lecture.
- **Niveaux** : vu-mètres avec crête maintenue et saturation mémorisée, conseil de réglage, équilibre G/D, stéréo / mono /
  phase, bruit de fond entre les morceaux, et la **dynamique de la face** (DR, crête / sonie, sonie) mesurée sur le son
  original : ce disque a-t-il été masterisé « fort » ?
- **Deux cartes son différentes** (entrée sur l'une, sortie sur l'autre) : la dérive de leurs horloges est corrigée.
- **Un cœur par canal** : les deux canaux sont traités en parallèle (même son, environ 1,7 fois moins de temps par période).
- **Page de réglage bilingue** (anglais / français), synchronisée sur tous les écrans ouverts.
- **Version de bureau** pour Windows (.exe) et Linux (AppImage), avec **nettoyage de fichiers** (WAV / FLAC).

Création originale (« clean-room ») : aucun code d'un logiciel de restauration commercial n'a été utilisé,
décompilé ou imité. Seules des méthodes publiées sont utilisées ; les sources sont en fin de document.

---

## Les outils

| Outil | Rôle |
|---|---|
| `vc-live` | Écoute en temps réel + page de réglage sur `http://<nom-du-pi>.local:8080`. |
| `vc-file` | Même traitement sur un fichier WAV. |
| `vc-bench` | Charge processeur sur **ton** Pi (sensibilités 0, 1, 3 et 5). |
| `vc-tests`, `vc-tests-crackle`, `vc-tests-loudness`, `vc-tests-rec`, `vc-tests-riaa`, `vc-tests-antidist`, `vc-tests-side`, `vc-tests-drift` | Tests automatiques. |

---

## Matériel

- **Raspberry Pi 3B+** ou plus récent (Raspberry Pi OS 64 bits conseillé). Un Pi 4 ou 5 a plus de marge.
- Une **carte son USB** avec une entrée et une sortie ligne stéréo (ou une carte d'extension entrée/sortie).
- Un **préampli phono**, ou celui intégré au logiciel (voir plus bas) si ta cellule est à aimant mobile (MM).

---

## Installation sur le Pi

Sur le Pi (dans un terminal ou en SSH) :

```bash
sudo apt-get update && sudo apt-get install -y git
git clone https://github.com/Daxabeur/vinylclean.git
cd vinylclean
./scripts/install.sh --service
```

Le script compile, teste et installe les outils, puis démarre le nettoyage **automatiquement à chaque démarrage**.
À la fin, il affiche l'adresse de la page de réglage.

**Mettre à jour** vers une nouvelle version :

```bash
cd ~/vinylclean
git pull
./scripts/install.sh --service
```

Ouvre ensuite **http://\<nom-du-pi\>.local:8080** sur ton téléphone (même réseau Wi-Fi), par exemple
`http://raspberrypi.local:8080`. Si ça ne s'ouvre pas, utilise l'adresse numérique affichée par le script
(`hostname -I` sur le Pi). Astuce : « Ajouter à l'écran d'accueil » transforme la page en icône d'application.

Un seul programme peut utiliser la carte son à la fois : arrête d'abord tout autre logiciel audio qui l'utilise.

Mesurer la charge processeur sur ton Pi :

```bash
vc-bench --rate 48000 --seconds 10
```

---

## La page de réglage

- **Écoute** : Original (sans traitement) / Enlevé (ce qui est retiré) / Nettoyé.
- **Préampli phono** : Externe / Intégré (RIAA), gain phono, anti-rumble.
- **Sensibilité** : clics et craquements 0 à 5, protection de la musique.
- **Niveaux** (repliable) : vu-mètres avec crête maintenue et saturation mémorisée, conseil de réglage, équilibre, stéréo / mono,
  bruit de fond, dynamique de la face (DR, crête / sonie, sonie).
- **Enregistrement** : nom du fichier, son **brut ou nettoyé**, **FLAC ou WAV**, bits, fréquence, enregistrer / arrêter,
  liste des enregistrements avec téléchargement et suppression.
- **Anti-distorsion** : réglages de la platine et de la cellule, courbe de l'erreur d'angle.
- **Paramètres avancés** : sensibilités séparées du déclick et du dé-crackle, intensités fines, longueur max d'un
  clic, multibande / pleine bande, canaux liés / séparés.
- **Carte son et qualité** : entrée, sortie et fréquence d'échantillonnage parmi ce que les cartes acceptent
  vraiment, avec la charge estimée. Changer de carte ou de fréquence coupe le son environ 1 seconde ; si la nouvelle
  configuration échoue, l'ancienne est rétablie toute seule.
- Bouton **EN / FR** dans la barre du haut (le choix est retenu par le navigateur).
- Bouton **⇅ Organiser** dans la barre du haut : chaque carte se replie sur son titre avec des flèches **▲ ▼** ; monte
  en haut les cartes que tu utilises le plus. Sur ordinateur la page a deux colonnes : ▲ sur la première carte de la
  colonne de droite la passe à la fin de celle de gauche (et ▼ dans l'autre sens). L'ordre est enregistré par le
  programme : le même sur tous les écrans. « Ordre par défaut » remet tout comme avant.

Tous les écrans ouverts restent **synchronisés**. Les réglages sont sauvegardés dans
`~/.config/vinylclean/settings.json` et rechargés au démarrage.

La page est lue sur le disque à chaque ouverture : tu peux modifier `/usr/local/share/vinylclean/index.html` et
simplement recharger la page. `sudo cmake --install build --prefix /usr/local` remet l'original.

**Qualité** : toujours le meilleur format natif de la carte (24 ou 32 bits), sans conversion ni rééchantillonnage.
Hors des clics réparés, le signal ressort identique au bit près. Fréquences : 44,1 / 48 / 88,2 / 96 kHz, et
176,4 / 192 kHz si la carte les accepte. Sur un Pi 3B+, **48 kHz est conseillé**.

---

## Les réglages

### Sensibilité 0 à 5

Le curseur **Clics et craquements** règle le déclick et le dé-crackle ensemble. Pour les régler séparément :
**Paramètres avancés** (le curseur principal affiche alors « perso »).

| Sensibilité | En clair |
|---|---|
| **0** | Désactivé. |
| **1** | Très léger : seulement les clics évidents. |
| **2** | Léger. |
| **3** | Moyen (défaut). |
| **4** | Fort. |
| **5** | Très fort : enlève aussi les tout petits tics, au risque d'effleurer une musique très dense (guitares saturées). |

### Protection de la musique

| Protection | En clair |
|---|---|
| **Aucune** | Répare aussi sur les frappes de batterie et les notes ; sur une attaque franche, seuls les aigus (> 3 kHz) sont adoucis. Un son de synthé parfaitement périodique reste protégé. Enlève le plus de gros clics. |
| **Normale** (défaut) | Relève le seuil sur les attaques et protège les sons périodiques. |
| **Maximale** | Ne touche jamais une attaque. Peut laisser passer un clic caché *dans* une frappe de batterie. |

### Paramètres avancés

- **Intensité du déclick et du dé-crackle** (0 à 10, par demi-cran) : les sensibilités 1 à 5 sont des positions
  précises de ces curseurs (déclick 2,5 / 4,5 / 6 / 7 / 8 ; dé-crackle 2,5 / 4,5 / 6,5 / 7 / 8).
- **Longueur max d'un clic** (0,5 à 5 ms, défaut 2,5 ms).
- **Réparation** : Multibande (défaut) ou Pleine bande.
- **Stéréo** : Canaux liés (défaut) ou Canaux séparés.

### Niveaux

La carte **Niveaux** se replie (son titre affiche alors seulement le conseil).

- **Vu-mètres** avec zones (vert, puis orange au-dessus de −12 dB, rouge près de 0 dB), repères à −12 et −6 dB, et
  **crête maintenue** 2 secondes. Une saturation (0 dBFS) affiche « CLIP » et un message rouge qui **reste jusqu'à ce
  que tu le touches**.
- **Conseil** en clair d'après les 20 dernières secondes : bon niveau, entrée trop faible / trop forte (gain de la carte
  son), ou gain phono à changer quand le préampli intégré est utilisé.
- **Infos sur le disque** (mesurées sur le signal de la platine) :
  - **Équilibre G / D** sur les 30 dernières secondes de musique (« centré », ou le côté le plus fort ; orange au-delà
    de 2 dB : cellule ou câble à vérifier) ;
  - **Stéréo / mono** : Mono, Stéréo, Stéréo large, ou **Phase ?** quand les canaux s'opposent (un câble ou la cellule
    branché à l'envers d'un côté) ;
  - **Bruit de fond** : le moment le plus calme entre deux morceaux (état du disque) ;
- **Dynamique de la face** — mesurée sur le son **original** (platine après le préampli phono, avant tout nettoyage),
  de la pose du diamant à maintenant, pour la question de la « loudness war » :
  - **DR** : méthode « DR » publiée (blocs de 3 s ; deuxième plus haute crête face au niveau efficace des 20 % de blocs
    les plus forts). **DR 14 et plus** : très dynamique (vert) ; **8 à 13** : habituel ; **7 et moins** : très
    compressé (rouge).
  - **Crête / sonie** (PLR) : plus haute crête moins la sonie intégrée.
  - **Sonie** (LUFS) : sonie intégrée ITU-R BS.1770 (les silences entre les morceaux ne comptent pas).
  - Durée depuis la pose du diamant. Une **nouvelle face** démarre toute seule quand la musique revient après 8 s de
    bras levé (sous −80 dBFS), avec le bouton **Nouvelle face**, ou avec « Je pose le diamant » (anti-distorsion).
  - Les clics font partie du son original : ils peuvent un peu relever les crêtes (le DR prend la deuxième plus haute
    crête pour limiter ça). Vérifié face aux mesures DR et BS.1770 de FFmpeg sur de vrais disques (±0,4 dB).

---

## Préampli phono intégré (platine sans préampli)

Une platine ne sort pas un son « normal » : à la gravure, les graves sont baissés et les aigus montés (courbe
**RIAA**). Un préampli phono fait l'inverse. Si tu n'en as pas, vinylclean peut le faire.

**Branchement**
- Utilise l'**entrée ligne** de la carte son (mode instrument éteint, pas d'alimentation fantôme 48 V — jamais avec
  une platine).
- Visse le fil de masse de la platine sur une partie métallique (carte son ou ampli). S'il y a un ronflement, essaie
  un autre point.
- Les cellules **MM** (aimant mobile, la plupart des cellules, environ 3 à 7 mV) fonctionnent bien. Les cellules
  **MC** (bobine mobile, 0,2 à 0,5 mV) donnent trop peu de signal pour une entrée ligne : il leur faut un préampli
  ou un transformateur.

**Réglage**
1. Page de réglage → **Préampli phono** → **Intégré**.
2. Monte le **gain d'entrée de la carte son** pendant un passage fort, sans jamais saturer. La correction RIAA
   amplifie ensuite les graves de près de 20 dB : les vu-mètres d'entrée peuvent sembler faibles, c'est normal.
3. Règle le **gain phono** pour que la **sortie** monte vers −12 à −6 dB dans les passages forts. Si une alerte
   « gain phono trop fort » apparaît, baisse-le.
4. **Anti-rumble** (conseillé) : coupe sous 15 Hz les grondements du moteur et du voile du disque, que la courbe
   RIAA amplifie (−0,1 dB seulement à 40 Hz).

**Qualité** : courbe officielle (IEC 60098, 3180 / 318 / 75 µs) reproduite à **±0,012 dB** à 44,1 kHz,
±0,004 dB à 48 kHz, moins de 0,001 dB à 88,2 kHz et au-delà (20 Hz – 20 kHz), vérifié par `vc-tests-riaa`.
Activer, couper ou changer le gain se fait en fondu, sans claquement. La correction est faite **avant** le déclick,
et les enregistrements contiennent le son corrigé. Un bon préampli phono analogique reste meilleur pour le
**souffle** (il amplifie avant la conversion).

Pour un fichier enregistré sans préampli : `vc-file brut.wav corrige.wav --riaa --phono-gain 20`.

---

## Enregistrer tes disques

Sur la page de réglage, carte **Enregistrement** :
1. Écris le **nom du fichier** (par exemple « Artiste - Album - Face A »).
2. **Enregistrer**, puis **Arrêter** à la fin de la face.

- **Son à enregistrer** : **Brut** (le son de la platine sans déclick, avec la correction RIAA si le préampli intégré
  est choisi) ou **Nettoyé** (après déclick, dé-crackle et anti-distorsion, quelle que soit l'écoute choisie). Le choix
  est rappelé à côté du chronomètre pendant l'enregistrement.
- **Format du fichier** : **FLAC** (sans perte, compressé) ou **WAV** (non compressé), avec le titre, la date et un
  commentaire en étiquettes.
- **Bits** : **Maximum** (par défaut) = FLAC 24 bits ou WAV **32 bits flottant** ; ou 32 bits flottant (WAV seulement),
  24 bits, 16 bits.
  - Le maximum est vraiment le maximum : la carte son donne 24 bits, et le traitement travaille en 32 bits flottant.
    Le FLAC 24 bits garde tout ce que donne la carte ; le WAV 32 bits flottant garde chaque valeur exactement, même
    au-dessus de 0 dBFS (préampli phono intégré avec beaucoup de gain). Un fichier « 32 bits entier » ne ferait
    qu'ajouter des zéros.
  - 16 bits (qualité CD) : avec un dither TPDF, pour que les passages calmes ne soient pas déformés. Une valeur déjà
    exacte en 16 bits (carte 16 bits, son brut) est gardée telle quelle.
- **Fréquence** : celles que ta carte accepte. L'enregistrement se fait à la fréquence de la carte (sans conversion) :
  choisir une autre fréquence change celle de la carte (≈ 1 s de silence, pas pendant un enregistrement).
- Un fichier WAV est limité à 4 Go (environ 1 h 30 à 96 kHz en 32 bits flottant) : l'enregistrement s'arrête alors
  tout seul.
- Tu peux **écouter et régler le nettoyage** pendant l'enregistrement : l'enregistrement n'est pas touché.
- Les fichiers sont dans `~/recordings` sur le Pi. Un nom déjà pris reçoit « (2) », « (3) »… : rien n'est jamais
  écrasé. Sans nom : « Recording <date> <heure> ».
- **Mes enregistrements** : liste avec durée et taille, **Télécharger** et **Supprimer** (deux appuis pour confirmer).
- L'enregistrement s'arrête tout seul si le disque est presque plein, si la carte son s'arrête ou si on change de
  carte ou de fréquence.

Récupérer tous les fichiers d'un coup depuis un ordinateur :

```bash
scp "<utilisateur>@<nom-du-pi>.local:~/recordings/*.{flac,wav}" .
```

---

## Anti-distorsion de fin de face

Vers le centre du disque, le sillon défile **2,5 fois moins vite** qu'au bord (21 cm/s contre 52 cm/s en 33 tours) :
les ondulations sont plus serrées et la pointe les suit moins bien. Deux causes, toutes deux calculables :

1. **Le rayon de la pointe** (distorsion de lecture, *tracing distortion*) : une pointe ronde de rayon ρ roule sur le
   sillon et lit, au premier ordre, la vitesse `u + (ρ / v²)·u·du/dt` (v : vitesse du sillon). Plus la pointe est fine
   (elliptique, Shibata, micro-ligne), plus c'est faible.
2. **L'erreur d'angle du bras** (bras pivotant) : la pointe n'est pas exactement tangente au sillon (angle φ). Quand
   elle se déplace latéralement de D, elle lit le sillon un peu en avance ou en retard (D·tan φ / v). φ se calcule
   **exactement** à partir de la longueur effective du bras et des deux points nuls de l'alignement : angle de décalage
   sin β = (r1 + r2) / 2L, distance pivot–axe d = √(L² − r1·r2), dépassement L − d,
   φ(r) = asin((L² + r² − d²) / 2Lr) − β.

Le logiciel retrouve la vitesse du sillon (courbe RIAA inverse), calcule ces deux termes et les retire à fréquence
double (pas de repliement), puis revient au son normal.

**Réglage** (page → carte « Anti-distorsion »)
1. **Oui**.
2. « Ma platine et ma cellule » : **longueur effective** du bras (notice), **alignement** utilisé pour régler la cellule
   (Stevenson, Baerwald, Löfgren B, ou tes deux points nuls), **pointe**, format du **disque**, **durée d'une face**. La
   page affiche le dépassement, l'angle de décalage et la distance pivot–axe correspondants (à comparer avec ta platine)
   et la courbe d'erreur d'angle.
3. **Position** : automatique — appuie sur « Je pose le diamant au début de la face » (ou laisse le logiciel repérer la
   pose : au moins 5 s de silence bras levé, puis de la musique) ; le rayon est estimé d'après le temps écoulé. Ou
   manuelle : tu indiques le rayon.
4. **Intensité** : 100 % par défaut.

**Mesures** (`vc-tests-antidist`) : la distorsion est fabriquée par une **simulation physique exacte** (pointe ronde qui
roule sur le sillon, sur une grille 32 fois plus fine ; position de lecture exacte pour l'erreur d'angle), indépendante
du modèle de correction. Deux sons forts, fin de face ; produits de distorsion d'ordre 2 et 3 par rapport au son :

| Cas | Avant | Après |
|---|---|---|
| Pointe conique 15,2 µm, 61 mm, 2 × 5 cm/s | −13,6 dB | −24,7 dB |
| Elliptique 0,3 × 0,7, 61 mm, 2 × 5 cm/s | −19,1 dB | −42,9 dB |
| Elliptique fine 0,2 × 0,7 | −22,5 dB | −50,4 dB |
| Shibata | −21,1 dB | −47,9 dB |
| Micro-ligne | −27,7 dB | −55,5 dB |
| Elliptique 0,3 × 0,7, 65 mm, 2 × 2 cm/s | −27,7 dB | −61,7 dB |
| Erreur d'angle 1,98° (Baerwald, bord), son grave + aigu | −38,2 dB | −75,9 dB |

**Limites**
- Le **niveau réel du sillon** (en cm/s) n'est pas connu du logiciel : il dépend de la cellule et des gains. Il est
  **estimé** : les passages forts d'un disque sont gravés vers 10 cm/s ; le logiciel retient le niveau des passages forts
  depuis le début de la face (un passage calme reste traité comme calme) ; la correction est active après 20 s de musique.
  Si l'estimation est 2 fois trop basse ou 1,5 fois trop haute, la correction ne retire plus qu'environ 6 dB ; il
  faudrait une estimation plus de 2 fois trop haute pour empirer la distorsion. D'où le réglage d'intensité.
- Sécurité : la correction est plafonnée en douceur (l'équivalent de 8 cm/s) ; un clic oublié par le déclick ou un
  niveau mal estimé ne peut pas créer de « pop ».
- Les rayons de pointe sont les valeurs usuelles des fabricants ; une pointe usée est plus « ronde » qu'à neuf. Les
  profils réels des pointes Shibata et micro-ligne sont plus complexes qu'un cercle : le rayon indiqué est une approximation.
- Hors de portée : la **saturation de lecture** (la pointe décroche), l'usure du sillon, les défauts de gravure.
- La position automatique suppose une gravure régulière du bord vers le centre ; en manuel, c'est exact.
- Coût : environ 0,7 % d'un cœur de PC (vérifie la charge affichée sur la page avec ton Pi).

---

## Stéréo : les deux canaux s'aident

La plupart des clics de vinyle ne touchent **qu'un canal**, alors qu'un événement musical est presque toujours présent
dans les deux (à ±1 ms près, micros espacés). vinylclean s'en sert :

- un candidat **sans « jumeau »** dans l'autre canal a un seuil un peu plus bas (jamais sur une attaque : un instrument
  placé d'un seul côté n'a pas de jumeau non plus) ;
- un candidat avec un **jumeau net** a un seuil un peu plus haut (probablement musical) ;
- les **longs clics** (≥ 0,6 ms) sont reconstruits avec un prédicteur croisé appris sur l'autre canal, combiné au modèle
  AR par moindres carrés pondérés ;
- sur un disque **mono** (canaux identiques), le test stéréo se coupe tout seul.

## Réparation multibande

La correction de chaque clic est découpée en 4 bandes (< 1 kHz, 1–3, 3–7, > 7 kHz) par des filtres à phase nulle. Dans
chaque bande, un gain de Wiener garde la réparation là où le clic domine (souvent les aigus) et le son d'origine là où
la musique domine (souvent les graves) : la musique n'est pas réinventée inutilement.

---

## Écran tactile

Écran officiel Raspberry Pi (DSI), ou écran HDMI avec tactile USB. Après l'installation ci-dessus :

```bash
cd ~/vinylclean
./scripts/kiosk-install.sh
```

L'écran affiche la même page en plein écran, en deux colonnes sur un écran 800 × 480. Navigateur léger « cog »
(WPE WebKit) dans « cage », sans bureau ; Chromium si cog n'est pas disponible. L'écran tourne en priorité basse : il
ne peut pas gêner le son, qui garde sa priorité temps réel.

- Enlever l'écran : `./scripts/kiosk-install.sh --remove` (le son continue).
- Image à l'envers (écran officiel) : ajouter `display_lcd_rotate=2` ou, avec le pilote KMS,
  `video=DSI-1:800x480@60,rotate=180` à la fin de `/boot/firmware/cmdline.txt`, puis redémarrer.

---

## Clavier (si tu lances `vc-live` à la main en SSH)

Arrête d'abord le service : `sudo systemctl stop vinylclean-live`, puis `vc-live`. Les messages du terminal sont en anglais.

| Touche | Effet |
|---|---|
| `3` / `1` / `2` | Nettoyé / Original / Enlevé |
| `+` / `-` | sensibilité (déclick et dé-crackle) |
| `c` / `v` | dé-crackle seul |
| `p` | protection (aucune → normale → maximale) |
| `[` / `]` | longueur max d'un clic |
| `m` | multibande / pleine bande |
| `s` | canaux liés / séparés |
| `r` | préampli phono intégré (RIAA) oui / non |
| `d` | anti-distorsion oui / non |
| `n` | début de face (diamant posé) |
| `q` | quitter |

---

## Traiter un fichier

```bash
arecord -D hw:0,0 -f S32_LE -r 48000 -c 2 -d 180 face.wav
vc-file face.wav face_s3.wav --sens 3
vc-file face.wav face_enleve.wav --sens 3 --mode removed    # ce qui est retiré
vc-file face.wav face_ad.wav --antidist on --arm-length 222 --align stevenson --stylus elliptical   # face entière, depuis le début
```

La sortie est alignée sur l'entrée (latence compensée) : les deux fichiers se comparent dans un éditeur audio.
`vc-file --help` liste toutes les options.

---

## Version de bureau (Windows et Linux)

Le même nettoyage sur un ordinateur, avec la même page de réglage dans sa propre fenêtre, plus une carte **Nettoyer un fichier**.

**Téléchargement** (page « Releases » du dépôt GitHub) :
- **Windows** : `vinylclean-<version>-windows-x64.exe` — un seul fichier portable, rien à installer. Double-clic.
  Windows SmartScreen peut prévenir d'un éditeur inconnu (le programme n'est pas signé) : « Informations complémentaires »
  → « Exécuter quand même ».
- **Linux** : `vinylclean-<version>-x86_64.AppImage` — rends-le exécutable (`chmod +x vinylclean-*.AppImage`) et lance-le.
  Fonctionne sur les distributions avec glibc 2.27 ou plus récente (Ubuntu 18.04+, Debian 10+, Fedora…).

**Temps réel** : dans « Carte son et qualité », choisis l'entrée (ta carte son) et la sortie (enceintes ou casque). Tout le
reste fonctionne comme sur le Pi : sensibilité, protection, préampli phono intégré, anti-distorsion, dynamique de la
face, ordre des cartes, enregistrement FLAC / WAV (dans `Musique/vinylclean`). Latence d'environ 60 à 100 ms. Deux
cartes différentes pour l'entrée et la sortie : la dérive d'horloge n'est pas corrigée sur la version de bureau (petites
coupures possibles) ; utilise la même carte pour les deux quand c'est possible.

**Nettoyer un fichier** : choisis un fichier WAV ou FLAC, le format de sortie (WAV ou FLAC), puis **Nettoyer**. Les réglages
actuels sont utilisés (sensibilité, protection, préampli phono, anti-distorsion ; le fichier est traité comme une face depuis
son début). À la fin, écoute **Original / Nettoyé** au même endroit, puis **Télécharger**. Une face de 20 minutes prend environ
une minute. Tout reste sur ton ordinateur.

La page s'ouvre dans une fenêtre d'application (Edge sous Windows, Chromium / Chrome sous Linux), ou dans ton navigateur par
défaut. Fermer la fenêtre quitte le programme (sauf pendant un enregistrement). Options : `--browser` (navigateur par défaut),
`--lan` (ouvrir la page depuis un téléphone du même réseau), `--clean ENTRÉE SORTIE` (nettoyer un fichier sans fenêtre).

Le programme de bureau utilise exactement le même code de traitement (`core/`) ; un WAV nettoyé sous Windows est identique au
bit près au même fichier nettoyé par `vc-file` sur un PC. Entrée/sortie audio : [miniaudio](https://miniaud.io) (WASAPI sous
Windows, PulseAudio / PipeWire / ALSA sous Linux). Code dans `desktop/` ; `desktop/build.sh` fabrique les deux fichiers depuis
Linux. La version Raspberry Pi n'est pas modifiée.

---

## Dépannage

| Problème | Solution |
|---|---|
| La page ne s'ouvre pas | `systemctl status vinylclean-live` ; essayer l'adresse numérique (`hostname -I`) |
| J'entends toujours les clics | vérifier « Nettoyé » sur la page ; protection « Aucune », puis monter la sensibilité (4 ou 5) |
| « L'entrée sature » | baisser le gain d'entrée de la carte son |
| `Device or resource busy` | un autre programme utilise la carte son : l'arrêter |
| Les « coupures » augmentent | revenir à 48 kHz ; vérifier `vcgencmd get_throttled` (0x0) et l'alimentation |
| Comportement bizarre avec le traitement sur deux cœurs | ajouter `--threads 1` à `VC_OPTS` dans `/etc/default/vinylclean`, puis `sudo systemctl restart vinylclean-live` (tout sur un cœur, même son) |
| Des notes sont abîmées | baisser la sensibilité ; protection « Maximale » |
| L'enregistrement s'arrête tout seul | lire le message de la carte Enregistrement (disque plein, changement de fréquence…) |
| Son très aigu, maigre, sans graves | la platine est en direct : Préampli phono → **Intégré** |
| Son sourd, graves énormes | tu as un préampli phono : Préampli phono → **Externe** |
| Journal du service | `journalctl -u vinylclean-live -n 50` |

---

## Comment ça marche (résumé)

**Déclick** : modèle autorégressif (AR) d'ordre 24 de la musique, ré-estimé toutes les 10,7 ms. Deux détecteurs (le
filtre adapté, et l'erreur avant + arrière), plus un pré-détecteur par bandes qui écarte les gros clics de l'estimation
du modèle. Seuil dynamique local, test d'isolement, protection des attaques et des sons périodiques (3 niveaux), deux
passes, puis interpolation **LSAR** (ordre plus élevé pour les longs clics).

**Dé-crackle** : le même moteur en profil « craquements » : réparations ≤ 0,5 ms, réparation douce de type Wiener (on
ne retire que la part qui dépasse l'erreur attendue de l'interpolation), amplitude plafonnée.

**Anti-distorsion** : vitesse du sillon retrouvée par la courbe RIAA inverse exacte ; suréchantillonnage ×2 (filtre
demi-bande à fenêtre de Kaiser, avec un second filtre qui donne la dérivée exacte, parfaitement alignée) ; termes de
distorsion de lecture et d'erreur d'angle calculés et retirés (inverse au premier ordre) ; retour à la fréquence de base
puis courbe RIAA.

**Temps réel** : tout est alloué au démarrage. Dans le fil audio : aucune allocation, aucun verrou, aucun appel système
hors ALSA. Dénormaux à zéro, latence fixe identique dans les 3 modes d'écoute, fondu de 10 ms aux changements de mode.
La page web tourne dans un fil de priorité basse et passe ses réglages à l'audio par des variables atomiques.

---

## Structure du projet

Le code (commentaires, messages, options) est en anglais ; la page de réglage est en anglais et en français.

```
core/       DSP : ar, lsar, declick, decrackle (+ chaîne), antidist (anti-distorsion), riaa.h, presets.h
tools/      vc_live.cpp, web_server.*, sha1.h, loudness.h, sidemeter.h, drift.h, levels.h, recorder.h, vc_file.cpp, vc_bench.cpp, wav.h, offline.h
web/        index.html (page de réglage, anglais / français)
desktop/    version Windows / Linux (vc_desktop.cpp, http_server.*, extra.html, build.sh, packaging/)
tests/      test_declick.cpp, test_decrackle.cpp, test_loudness.cpp, test_recorder.cpp, test_riaa.cpp, test_antidist.cpp, test_side.cpp, test_drift.cpp, page_test_server.cpp, synth.h
scripts/    install.sh, kiosk-install.sh, services systemd, options par défaut
cmake/      compilation croisée (PC → Pi), facultatif
```

## Sources

Voir la liste complète dans [README.md](README.md#sources) : Godsill & Rayner (1998), Vaseghi & Rayner (1990),
Janssen et al. (1986), Özdoğru (2022, 2025), Rund et al. (2021), Rajmic & Klimek (2004), Bello et al. (2005),
IEC 60098, Löfgren (1938), Baerwald (1941), Stevenson (1966), Corrington (1951), Woodward & Fox (1947),
ITU-R BS.1770-4, méthode « DR » (Pleasurize Music Foundation), J. O. Smith (rééchantillonnage), Esquef et al. (2002,
prédiction déformée : essayée avec `vc-file --warp`, pas meilleure sur les bancs d'essai, donc désactivée), libFLAC,
RFC 6455, miniaudio.

## Licence

MIT — voir `LICENSE`.
