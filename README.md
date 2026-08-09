# SP404 Companion

Plugin natif (VST3/AU) pour gérer les samples d'une Roland SP-404SX directement depuis un
DAW — fortement inspiré de [super-pads](https://github.com/MatthewCallis/super-pads), mais
sous forme d'extension audio plutôt que d'app Electron séparée.

Le plugin sait déjà lire une vraie carte SD SP-404SX (parsing binaire vérifié contre du
matériel réel), afficher ses banques/pads dans une WebView (pont JS↔C++ JUCE 8), et **jouer
les pads en MIDI depuis une piste du DAW** (Program Change pour changer de banque, notes
chromatiques pour déclencher les pads). Encore un stade précoce : pas d'édition/écriture sur
la carte, UI HTML/JS minimale (pas encore React) — voir Roadmap.

## Prérequis

- macOS avec Xcode (ou les Command Line Tools) installé.
- CMake ≥ 3.22.
- Une connexion réseau au premier `cmake configure` (JUCE, Catch2 et Rubber Band Library sont
  récupérés via `FetchContent`).
- Rubber Band Library (pitch/time-stretch, voir `plugin/SampleDsp.cpp`) est sous licence **GPL
  v2 ou ultérieure**, ou licence commerciale — voir
  `build/_deps/rubberband-src/COPYING` après le premier configure. Pertinent seulement si ce
  plugin est un jour distribué.

## Build

```sh
cmake -B build -G Xcode
cmake --build build --config Debug
```

(ou un générateur Ninja si préféré : `cmake -B build -G Ninja && cmake --build build`)

Les artefacts VST3/AU sont produits sous
`build/plugin/SP404Companion_artefacts/Debug/{VST3,AU}/`.

## Tests

```sh
cmake --build build --target sp404_core_tests
ctest --test-dir build
```

Les tests couvrent `core/` (parsing `PAD_INFO.BIN` et chunk `RLND`), voir
[`docs/sp404sx-format.md`](docs/sp404sx-format.md) pour la spec du format et ses sources
(partiellement vérifiée contre une vraie carte SD SP-404SX).

## Vérifier le plugin manuellement

1. Charger le `.component` (AU) ou `.vst3` produit dans un DAW installé localement, ou valider
   l'AU en CLI avec `auval` (`auval -v aumu Cmp1 Sp4C` — type `aumu` car c'est un
   instrument/synthé depuis l'ajout du triggering MIDI ; ajuster les codes si modifiés dans
   [`plugin/CMakeLists.txt`](plugin/CMakeLists.txt)). Si `auval` ne trouve pas le composant
   juste après un rebuild, relancer le cache AU : `killall -9 AudioComponentRegistrar`.
2. Ouvrir la fenêtre du plugin : elle doit afficher 10 cases (banques A→J) obtenues via un
   appel natif `listBanks()` depuis la WebView — cela valide le pont `window.__JUCE__` de bout
   en bout ([`plugin/WebUIBridge.cpp`](plugin/WebUIBridge.cpp)).
3. Si une carte SD SP-404SX est montée (scan automatique de `/Volumes`, voir
   `sp404::findConnectedCardRoot()`), le statut passe à "SD card connected" et cliquer sur une
   banque appelle `listPads()` pour afficher les 12 pads (sample, volume, loop/gate/reverse,
   tempo, durée) lus depuis `PAD_INFO.BIN`. Sans carte connectée, les banques s'affichent quand
   même (juste leurs noms) mais sans détail de pad. Comme sur la vraie SP-404A, il n'y a que 5
   boutons de banque (A/F, B/G, C/H, D/I, E/J) + un bouton Shift qui choisit la moitié (A-E ou
   F-J) — l'écran rond en haut affiche la lettre réellement armée.
4. Cliquer sur une banque l'arme aussi pour la lecture MIDI (`selectBank`, voir
   [`plugin/WebUIBridge.cpp`](plugin/WebUIBridge.cpp)). Poser le plugin sur une piste
   MIDI/instrument (c'est un synthé — `IS_SYNTH TRUE`, pas d'entrée audio) et jouer les notes
   36 à 47 (C1 à B1, convention Ableton/FL Studio) déclenche les pads 1 à 12 de la banque
   active — voir [`plugin/PluginProcessor.h`](plugin/PluginProcessor.h) pour `kBasePadNote`. Un
   Program Change (numéro 0 à 9) change la banque active depuis la piste MIDI elle-même (0=A,
   1=B, ..., 9=J), en plus du clic dans l'UI. Chaque pad respecte ses réglages `PAD_INFO.BIN` :
   volume, `loop` (rebouclage), `gate` (coupe au relâchement de la note si actif, sinon la note
   off est ignorée), `reverse` (lecture depuis la fin). Au plus 2 pads distincts sonnent en même
   temps (`kMaxPolyphony`, voir [`plugin/PluginProcessor.h`](plugin/PluginProcessor.h)) — en
   déclencher un 3ᵉ coupe (avec un léger fade) le plus ancien encore actif.
5. Cliquer-glisser (mousedown/mouseup) directement sur un pad dans l'UI le pré-écoute — même
   chemin de déclenchement que le MIDI (`previewPadOn`/`previewPadOff`, voir
   [`plugin/WebUIBridge.cpp`](plugin/WebUIBridge.cpp)), donc respecte aussi gate/loop/reverse et
   la même limite de polyphonie.
6. Cocher/décocher loop/gate/rev/lofi ou bouger le curseur de volume sur un pad écrit
   **immédiatement** sur la carte SD réelle (`updatePad`, voir `sp404::savePadInfo`) — pas de
   bouton "Sauver", pas d'undo. La banque active est rechargée aussitôt pour que le changement
   s'entende dès la prochaine lecture du pad.
7. Un pad en train de jouer (MIDI ou pré-écoute) s'allume d'un contour orange dans l'UI —
   `getActivePads()` interroge `PluginProcessor::getActivePadMask()` toutes les 100ms
   ([`plugin/web/index.html`](plugin/web/index.html), `pollActivePads`). C'est ce qui rend
   visible l'effet de `gate` : relâcher un pad en mode gate doit éteindre son contour
   quasi-immédiatement, alors qu'un pad sans gate reste allumé jusqu'à la fin du sample.
   Comportement vérifié directement contre la vraie carte (le niveau de sortie retombe au
   silence après relâchement en gate, contre un niveau inchangé sans gate).
8. Les 4 potards (Volume/Low/Mid/High) sont de vrais `AudioProcessorParameter` (0-127) reliés à
   un étage de sortie **master** (gain + EQ 3 bandes), appliqué au mix de tous les pads juste
   avant qu'il ne quitte le plugin — pas par pad, un seul étage pour toute la sortie. Position 64
   = neutre (0dB) sur les 4 ; en dessous/au-dessus ça baisse/monte. Volume va de -60dB à +6dB ;
   Low (shelf 200Hz), Mid (peak 1kHz), High (shelf 4kHz) vont chacun de -12dB à +12dB. Voir
   `PluginProcessor::updateOutputEq()`/`processBlock` (implémenté avec `juce::dsp::IIR`, module
   `juce_dsp`). Glisser verticalement sur un potard change sa valeur ; clic droit arme un
   MIDI-learn (le prochain CC reçu s'y mappe, voir `PluginProcessor::startKnobLearn`) — le
   mapping CC n'est pas encore persistant entre sessions (`getStateInformation` reste un no-op).
9. Icône carte SD en haut à droite : verte si une carte est montée, grise sinon
   (`pollConnection`, revérifié toutes les 2s pour suivre un branchement/débranchement à chaud).
10. Glisser un fichier audio (`.wav`/`.aif`/`.aiff`/`.flac`/`.mp3`/`.mp4`/`.m4a`) sur un pad
    remplace son échantillon sur la carte SD réelle en conservant son numéro. Le fichier source
    est décodé, ré-échantillonné vers le taux natif de la SP-404SX (44100 Hz, vérifié sur une
    vraie carte — voir `docs/sp404sx-format.md`) s'il diffère, et ré-encodé en wav PCM 16-bit
    (voir `sp404::importAudioToWav`, `plugin/SampleImport.h`) avant d'être écrit via
    `sp404::replacePadSample`. Le décodage mp3/mp4/m4a passe par `CoreAudioFormat` (AudioToolbox,
    macOS uniquement) ; flac/wav/aiff sont décodés nativement par JUCE — aucune dépendance
    supplémentaire. Les réglages du pad (volume/loop/gate/reverse/lofi) sont conservés, seuls les
    champs dérivés du fichier (offsets, channels, tempo) sont recalculés. Écriture immédiate,
    comme les autres éditions de pad — pas de bouton "Sauver", pas d'undo. Deux chemins côté UI
    (voir `wirePadDragDrop`/`extractDroppedFilePath` dans `app.js`) selon ce que la source du
    drag expose réellement :
    - Un vrai fichier OS (typiquement un drag depuis le Finder) : lu en JS
      (`file.arrayBuffer()`), envoyé en base64 au natif (`swapPadSample`). L'encodage se fait par
      blocs avec un retour périodique à la boucle d'événements (`arrayBufferToBase64`) pour ne
      pas geler l'hôte sur un gros fichier ; le polling UI (pads actifs/clipping/connexion/
      potards) est aussi suspendu pendant qu'un drag survole la fenêtre.
    - Uniquement un texte (`text/plain`/`text/uri-list`), pas de vrai `File` — observé avec le
      panneau Splice d'Ableton Live, qui n'expose pas de fichier OS au drag HTML5. Si ce texte
      (ou un champ dans son JSON, cherché récursivement par `findPathLikeField`) ressemble à un
      chemin ou une URI `file://` se terminant par une extension supportée, il est passé tel quel à
      `swapPadSampleFromPath`, qui le lit directement depuis le disque côté natif. Si rien
      d'exploitable n'est trouvé, la barre de statut affiche ce que `dataTransfer` contenait pour
      diagnostiquer.
      **Limitation constatée avec Splice** : son payload de drag (vérifié en pratique) ne contient
      que des métadonnées de lecture (`fileName` nu sans dossier, `assetId` opaque, tempo,
      durée...), jamais de chemin réel — Splice ne semble pas exposer de fichier OS au drag HTML5
      dans ce contexte, donc ce cas précis n'est pas récupérable depuis l'UI web. Contournement
      fiable : glisser d'abord depuis Splice vers le Finder/bureau, puis glisser ce fichier depuis
      le Finder sur le pad (chemin testé et fonctionnel).
    Limitations : seuls wav/aif/aiff/flac/mp3/mp4/m4a sont acceptés (rejeté côté UI sinon) ; pas
    de chunk `RLND` dans le fichier écrit, donc la compatibilité avec le vrai hardware après un
    swap logiciel n'est pas garantie (notre propre lecture n'en a pas besoin, voir
    `docs/sp404sx-format.md`).
11. Menu de gestion des banks (icône en haut à gauche, miroir de l'icône carte SD) :
    Sauvegarder/Charger toutes les banks (zippe/restaure `SMPL/` en entier, voir
    `sp404::saveAllBanksToZip`/`loadAllBanksFromZip`, `plugin/BankArchive.h`), Sauvegarder cette
    bank/Charger une bank (archive d'une seule bank avec sa tranche de 384 octets de
    `PAD_INFO.BIN` + ses samples sous des noms positionnels `pad01`..`pad12`, ce qui permet de
    restaurer vers une bank différente de l'origine — dupliquer/réarranger des banks — sans
    renommage manuel, voir `sp404::saveBankToZip`/`loadBankFromZip`), Vider cette bank/Vider tout
    (`sp404::clearBank`/`clearAllBanks` — supprime les fichiers samples et remet `PadInfo` à
    zéro). Le "Save As"/"Open" passe par un vrai sélecteur de fichier natif
    (`juce::FileChooser`, async — seul dialogue natif de l'app, pas d'équivalent web pour un
    Save As vers un emplacement arbitraire) ; toute action destructive (vider, charger qui
    remplace) est confirmée par une modale **dans la WebView** (pas de dialogue système) pour
    rester visuellement cohérente avec le reste du panneau. Écriture immédiate, sans undo, comme
    le reste de l'app.
12. Mode offline/live (même menu que ci-dessus) : au démarrage le plugin est **offline par
    défaut** (sur la Bank A, comme `BankLoader`) — pas d'écriture accidentelle sur une carte
    connectée avant que l'utilisateur ne le demande explicitement. En mode live, toute
    lecture/écriture vise la vraie carte SD connectée ; en offline, tout vise à la place un
    miroir local fixe (`~/Library/Application Support/SP404Companion/OfflineMirror`) — voir
    `PluginProcessor::resolveCardRoot()`, le seul endroit qui décide live/offline (`BankLoader`
    et tous les handlers de `WebUIBridge.cpp` passent par cette méthode plutôt que d'appeler
    `findConnectedCardRoot()` directement). "Go Offline…" amorce le miroir depuis la carte
    actuellement connectée s'il n'existe pas encore (échoue sans carte connectée la première
    fois) ; "Sync Mirror → Card" (visible seulement en mode offline, confirmé) pousse le contenu
    du miroir vers la carte réelle connectée, l'écrasant (`sp404::syncCard`, utilisé dans les
    deux sens). Le badge `LIVE`/`OFFLINE` en haut à gauche reflète l'état courant.
13. Panneau DSP par pad (bouton "DSP…" dans les contrôles de chaque pad) — chaque action lit
    l'échantillon **actuel** du pad, le transforme, et le réimporte via
    `sp404::replacePadSample` (voir `plugin/SampleDsp.h`) : donc, comme un swap par
    drag & drop, le trim utilisateur du pad se réinitialise sur le résultat, mais
    volume/loop/gate/reverse/lofi sont conservés. Écriture immédiate, sans undo. Une ligne de
    statut en haut du panneau (`sp404::getPadDspStatus`, lecture seule) affiche en permanence
    l'état réel du pad — canaux (mono/stéréo), niveau de crête en dBFS, silence de bord restant —
    rafraîchie à l'ouverture et après chaque action, pour que le retour visuel distingue "rien à
    faire" (déjà normalisé, déjà stéréo, pas de silence à couper) d'un changement réel :
    - Normaliser (`juce::AudioBuffer::getMagnitude`/`applyGain`, cible -1 dBFS) — le message
      affiché compare le pic avant/après (`peakBeforeDb`/`peakAfterDb`) plutôt qu'un simple "fait".
    - Mono/Stéréo (moyenne des canaux ou duplication — pas d'utilitaire JUCE tout fait pour ça) —
      le bouton correspondant au format actuel du pad reste allumé (orange) et désactivé, pour
      qu'on voie l'état sans avoir à le déduire.
    - Fade in/out (`applyGainRamp`).
    - Trim silence automatique (scan du premier/dernier échantillon au-dessus d'un seuil) — la
      quantité réellement coupée (`leadingSilenceSecondsBefore`/`trailingSilenceSecondsBefore`,
      mesurée juste avant l'action) est affichée en millisecondes, ou "No silence found to trim."
      si le pad était déjà tendu.
    - Détection BPM (lecture seule sur l'audio, n'écrit rien sur le sample) : autocorrélation de
      l'enveloppe d'énergie (fenêtres ~10ms, recherche sur la plage 60-200 BPM) — approche
      volontairement simple, adaptée à des samples/loops courts (usage réel d'un pad SP-404)
      plutôt qu'à un morceau entier. Le résultat est sauvegardé dans `userTempo`/`tempoMode` du
      `PAD_INFO.BIN` du pad (mêmes champs que le hardware utilise pour un tempo réglé
      manuellement — voir `core/include/sp404/PadInfo.h`), donc réapparaît pré-rempli ("saved")
      la prochaine fois que le panneau DSP de ce pad est ouvert, sans avoir à relancer la
      détection. Pas de détection de tonalité/clé musicale (aucune option raisonnable trouvée
      sans un projet bien plus lourd type Essentia).
    - Pitch (demi-tons) / time-stretch (ratio) via **Rubber Band Library** (mode offline, moteur
      R3/"Finer", nouvelle dépendance FetchContent — voir Prérequis pour la licence). Compilé via
      le fichier unique officiel `single/RubberBandSingle.cpp` (le dépôt n'a pas de
      `CMakeLists.txt`, son système de build est Meson) ; utilise la FFT vDSP
      (`Accelerate.framework`) sur macOS.
    - Pendant qu'une action est en cours, tous les boutons du panneau (sauf Close) sont désactivés
      pour éviter un double-clic qui lancerait deux écritures concurrentes sur le même fichier.
14. Thèmes (menu "Theme…") : 10 thèmes changeant couleurs/fond/forme des pads/style des potards
    d'un clic, persistés (`~/Library/Application Support/SP404Companion/Prefs.json`, voir
    `sp404::getTheme`/`setTheme` dans `WebUIBridge.cpp`). Chaque thème n'est qu'un bloc de
    variables CSS custom-properties dans `plugin/web/themes.css` (voir ce fichier pour la liste
    des 10 et leurs valeurs) ; les règles qui les consomment vivent dans `styles.css` et ne
    changent jamais. Le thème "Hardware" (défaut) garde le potard photographié original
    (`knob0-6.png`) ; les 9 autres utilisent un potard procédural en SVG (`.knob-procedural`,
    cercle + indicateur qui tourne réellement via `transform: rotate()`) — les deux
    représentations existent en permanence dans le DOM, seul le thème actif décide laquelle est
    visible (`applyKnobVisual` dans `app.js` met les deux à jour à chaque fois). Deux thèmes
    ("Kawaii Stickers", "Japandi Zen") affichent des stickers détourés en arrière-plan
    (`plugin/web/stickers/`) — voir "Provenance des stickers" ci-dessous. **Limite assumée** :
    seules les surfaces principales (fond de page, pads, potards, accent/texte/bordure) sont
    ré-habillées par thème ; les composants imbriqués (modales, menu déroulant, boutons de
    bascule) gardent leur habillage sombre d'origine sur tous les thèmes — ré-habiller chaque
    dégradé imbriqué à la main était hors de portée raisonnable de ce chantier.

### Provenance des stickers

Les stickers des thèmes "Kawaii"/"Japandi" viennent de planches fournies par l'utilisateur
(`/Users/paul/Downloads/backgrounsp/stickers 3.jpeg` et `stickers 4.jpeg`, motifs génériques
japonais/kawaii, sans personnage ni logo de marque), détourées automatiquement (flood-fill depuis
les bords pour trouver le fond + étiquetage de composantes connexes pour isoler chaque sticker,
script jetable non commité) puis sélectionnées à la main. **Deux autres fichiers fournis dans le
même dossier n'ont volontairement pas été utilisés** : `background 1.jpeg`/`background 2.jpg`
(collages denses de personnages/logos protégés — Bart Simpson, personnages de South Park, logos
Superman/The Flash/Grand Theft Auto IV, Stewie, le singe BAPE, Taz...) et `stickers 2.jpeg`/
`stickers 5.jpeg` (logo déposé Thrasher Magazine ; design dérivé "Mona Lisa DJ" probablement
protégé) — les compiler dans les assets du plugin aurait constitué une reproduction non
autorisée dans un binaire potentiellement redistribuable, au-delà du cadre d'un simple fond
d'écran personnel.

## Architecture

```
core/     bibliothèque C++ pure (PadInfo, Bank/Pad, SdCard, WavInfo, RlndChunk, Pattern) — zéro
          dépendance audio/GUI JUCE, testée indépendamment.
plugin/   cible JUCE (VST3 + AU, synthé), éditeur hébergeant une WebView (JUCE 8
          WebBrowserComponent) qui appelle du code natif via des NativeFunction ;
          BankLoader (thread d'arrière-plan + lecture audio JUCE) et le routage MIDI/mixage
          dans PluginProcessor assurent la lecture réelle des pads. Un étage de sortie master
          (gain + EQ 3 bandes low/mid/high, module juce_dsp, voir plus haut) tourne dans
          PluginProcessor::processBlock, après le mixage des voix et avant la détection de
          clipping. SampleImport.h/.cpp décode/
          ré-échantillonne/ré-encode un fichier audio importé (voir plus haut) ; BankArchive.h/.cpp
          gère les sauvegardes/restaurations zip (menu de gestion des banks) ; SampleDsp.h/.cpp
          implémente le panneau DSP par pad (normalisation, mono/stéréo, fade, trim, BPM,
          pitch/time-stretch via Rubber Band). Tous dépendent de JUCE (et, pour SampleDsp,
          Rubber Band) et vivent donc dans plugin/, pas core/ (qui reste 100% C++ pur).
          PluginProcessor::prefsPath() + les NativeFunction getTheme/setTheme de WebUIBridge.cpp
          persistent la préférence de thème (Prefs.json, même schéma que mirrorRoot()/
          OfflineMirror pour le mode offline).
plugin/web/   UI de la WebView, en fichiers séparés (pas de bundler pour l'instant) :
          index.html (structure), styles.css, themes.css (10 thèmes, voir plus haut), app.js,
          header.png/screen.png/knob0-6.png
          (7 pre-rendered knob rotation frames, swapped by value rather than CSS-rotated so
          the artwork's baked-in perspective stays correct at every position ; le sprite ne
          couvrant que la moitié 0-50% du débattement, l'autre moitié est obtenue en retournant
          horizontalement les mêmes frames — voir kKnobFrameTable dans app.js), stickers/
          (25 PNG transparents détourés pour les thèmes Kawaii/Japandi, voir plus haut) —
          chacun servi par WebUIBridge::provideResource() qui route par chemin vers la
          BinaryData correspondante.
docs/     spécification du format de carte SD SP-404SX et ses sources.
```

## Limitations connues du triggering MIDI (MVP)

- Pas de resampling **à la lecture** : un `.WAV` dont le sample rate diffère de celui de la
  session DAW jouera à une vitesse/hauteur légèrement décalée (le cas courant 44.1kHz — format
  natif de la SP-404SX, et taux cible de l'import/DSP, voir plus haut — est généralement
  correct). Le pitch/time-stretch du panneau DSP est un traitement **hors-ligne, appliqué au
  fichier sur la carte** (voir plus haut) — ce n'est pas un moteur de pitch/time-stretch en
  temps réel piloté par la note MIDI reçue ; chaque pad continue de jouer à vitesse fixe quelle
  que soit la note (comportement "pad", pas synthé multi-échantillonné).
- Une seule voix par pad : rejouer une note pendant qu'elle sonne déjà coupe et relance (léger
  fade de ~1ms pour éviter un clic), pas de chevauchement polyphonique du même pad.
- Polyphonie limitée à 2 pads distincts simultanés (`kMaxPolyphony`) ; un 3ᵉ pad coupe le plus
  ancien.
- Pas de détection de tonalité/clé musicale (voir panneau DSP) — seul le BPM est estimé.

## Roadmap (hors scope de cette mise en place)


- Validation du chunk WAV `RLND` contre le code source complet de `uttori-audio-wave` et/ou un
  vrai fichier `.WAV` de carte SD (la table `PAD_INFO.BIN`, elle, a déjà été vérifiée contre du
  matériel réel — voir `docs/sp404sx-format.md`).
- Détection de carte SD sur Windows/Linux si le projet s'étend au-delà de macOS (pour l'instant
  `findConnectedCardRoot()` ne scanne que `/Volumes`).
- Persistance d'état (banque active, mode offline/live) dans la session DAW
  (`getStateInformation` est actuellement un no-op — le mode offline/live et le miroir
  survivent sur disque, mais pas la sélection "on était en offline" au rechargement du plugin).
- Gestion des patterns (`ROLAND/SP-404SX/PTN/PTNxxxxx.BIN`, `STPINFO.BIN`).
  - ✅ **Préalable obligatoire vérifié (2026-08-09)** : format `PTN` vérifié contre 3 vrais
    fichiers d'une carte SD SP-404SX réelle (croisé avec la classe `AudioPattern` d'
    [uttori-audio-padinfo](https://github.com/uttori/uttori-audio-padinfo),
    [spEdit404](https://github.com/bobgonzalez/spEdit404) et [la doc de
    byteflip.club](http://byteflip.club/sp-edit/roland-sp404sx-ptn-format)) — voir la section
    "`PTN/PTNxxxxx.BIN`" de `docs/sp404sx-format.md` pour la structure complète (8 octets/
    événement, footer 16 octets, adressage pad, et l'octet du nombre de mesures propre au
    SP-404SX qu'aucune des sources communautaires ne documentait). Sur les 3 patterns réels,
    chaque pattern ne référence que les pads d'**une seule banque** — donc, en pratique, la
    gestion des samples liés ci-dessous peut raisonnablement supposer "un pattern = une banque",
    tout en restant vigilant si un contre-exemple apparaît (voir "Ce qui reste ouvert" dans le
    doc). `STPINFO.BIN` (124 octets sur la carte réelle) reste sans documentation trouvée.
  - ✅ Lecture seule (`core/`) : `core/include/sp404/Pattern.h` / `core/src/Pattern.cpp`
    parsent un pattern en liste d'événements (tick, pad référencé via `bank()`/
    `padIndexInBank()`, vélocité, durée) + nombre de mesures, testé dans
    `core/tests/PatternTests.cpp` contre les 3 mêmes fixtures réelles utilisées pour la
    vérification du format ci-dessus.
  - ✅ Adressage slot → fichier : `sp404::patternSlotPath()`/`patternDir()`
    (`core/include/sp404/SdCard.h`) donnent le chemin `PTNxxxxx.BIN` d'un slot banque/pad,
    suivant la même grille que les pads d'échantillons (banque × 12 + pad, décimal 5 chiffres).
    Hypothèse **corroborée par deux sources indépendantes** (nos 3 fichiers réels + la formule
    de `spEdit404`, voir `docs/sp404sx-format.md`) mais non prouvée au-delà de la banque A —
    aucun pattern réel disponible dans une autre banque pour confirmer au-delà du slot 12.
  - Visualisation dans le plugin : lister les patterns d'une banque (en s'appuyant sur
    `patternSlotPath`/`readPattern` ci-dessus), afficher leur longueur et les pads qu'ils
    référencent ; détecter et signaler en continu (pas seulement à l'import) un pad référencé
    sans sample, ou dont le sample a changé depuis. Reste à faire : le handler
    `WebUIBridge`/panneau JS lui-même — les deux briques `core/` nécessaires existent déjà.
  - Sauvegarder/charger un pattern : ne jamais exporter le `PTNxxxxx.BIN` seul — le bundler avec
    les samples des pads référencés et leur tranche `PAD_INFO.BIN`, même logique que
    `BankArchive::saveBankToZip`, sinon restaurer ailleurs (autre banque, autre carte) rejoue
    n'importe quoi.
  - Renommer/dupliquer/réorganiser un pattern entre slots, et suppression — symétrique de ce qui
    existe déjà pour les banks (menu de gestion des banks).
  - Aperçu en lecture seule d'un pattern dans l'UI (mini-timeline) avant chargement.
  - Export pattern → fichier MIDI standard (conversion déjà implémentée côté `AudioPattern`,
    réutilisable comme référence) et import MIDI → pattern SP-404 en sens inverse (quantisé sur
    la grille de 384 ticks/bar, résolution note MIDI → pad à définir).
  - Triggering d'un pattern entier depuis le DAW, synchronisé tempo/transport hôte (au-delà du
    triggering pad-par-pad actuel) — nécessite un scheduler interne aligné sur
    `juce::AudioPlayHead`, le plus gros morceau de cette liste.

