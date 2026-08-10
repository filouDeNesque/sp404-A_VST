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
(vérifiée contre une vraie carte SD SP-404SX).

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
    supplémentaire. **Volume et gate repartent systématiquement à 100/activé** (2026-08-10, sur
    demande explicite) — y compris en remplaçant un pad déjà configuré, pas seulement un pad
    vide : un import est censé partir d'un point de départ audible et prévisible, plutôt
    qu'hériter du volume=0 par défaut d'un slot jamais utilisé (silencieux jusqu'à réglage manuel
    — c'était le bug signalé) ou des réglages d'un ancien sample sans rapport. loop/reverse/lofi
    restent conservés. `sp404::replacePadSample` prend désormais un paramètre explicite
    `resetPlaybackDefaults` pour ça (`true` ici, dans `doSwapPadSample`) — **délibérément pas**
    la même valeur pour le panneau DSP (voir item 13 plus bas), où repartir à 100/gate activé à
    chaque Normalize/Trim/Fade serait une surprise plutôt qu'un service : c'est le même sample,
    juste retouché, pas un nouvel import. Champs dérivés du fichier (offsets, channels, tempo)
    toujours recalculés. Écriture immédiate, comme les autres éditions de pad — pas de bouton
    "Sauver", pas d'undo. Deux chemins côté UI
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
    Limitations : seuls wav/aif/aiff/flac/mp3/mp4/m4a sont acceptés (rejeté côté UI sinon).
    Le fichier écrit inclut désormais le chunk `RLND` requis par le vrai hardware (voir la
    section Roadmap "Chunk WAV `RLND`" plus bas pour le détail) — un swap logiciel ne devrait
    plus être ignoré/rejeté par un SP-404SX/A réel, sous réserve du test bout-en-bout sur le vrai
    appareil documenté comme non fait ci-dessous.
    **Point d'attention côté Ableton Live (constaté par l'utilisateur, 2026-08-10)** : glisser un
    clip audio *warpé* depuis Ableton directement sur un pad semble importer une version déjà
    étirée au tempo du projet, pas le fichier `.wav` d'origine sur le disque. Ce n'est **pas**
    un comportement de ce plugin : `swapPadSample`/`importAudioToWav` reçoivent tels quels les
    octets que le glisser-déposer HTML5 leur donne (voir `wirePadDragDrop` dans `app.js`), sans
    aucune visibilité sur leur provenance — ce plugin ne fait que ré-échantillonner si le taux
    diffère de 44100 Hz (voir plus haut), jamais d'étirement temporel à l'import. C'est un
    comportement documenté d'Ableton Live lui-même : glisser un clip warpé hors de Live exporte
    le rendu tel qu'il sonne actuellement (warp appliqué), pas le fichier source brut — pour que
    ce qui est glissé corresponde à ce qui est entendu dans la session. À garder en tête plutôt
    qu'à corriger : pour importer le fichier `.wav` d'origine non modifié, glisser depuis le
    Finder plutôt que directement depuis la timeline Ableton (même contournement que pour
    Splice ci-dessus).
11. Menu de gestion (icône en haut à gauche, miroir de l'icône carte SD) : réorganisé
    (2026-08-10) en sections **Bank** et **Pattern** avec des libellés de groupe non cliquables
    (`.menu-section-label` dans `styles.css`), pour rester lisible maintenant que les deux
    familles d'actions comptent chacune 4-6 boutons — les actions elles-mêmes (`data-action`) et
    leur comportement sont inchangés, seul le regroupement visuel a bougé.
    - **Bank** : Sauvegarder/Charger toutes les banks (zippe/restaure `SMPL/` en entier, voir
      `sp404::saveAllBanksToZip`/`loadAllBanksFromZip`, `plugin/BankArchive.h`), Sauvegarder
      cette bank/Charger une bank (archive d'une seule bank avec sa tranche de 384 octets de
      `PAD_INFO.BIN` + ses samples sous des noms positionnels `pad01`..`pad12`, ce qui permet de
      restaurer vers une bank différente de l'origine — dupliquer/réarranger des banks — sans
      renommage manuel, voir `sp404::saveBankToZip`/`loadBankFromZip`), Vider cette bank/Vider
      toutes les banks (`sp404::clearBank`/`clearAllBanks` — supprime les fichiers samples et
      remet `PadInfo` à zéro).
    - **Pattern** : Patterns… (ouvre le panneau détaillé au niveau 15 plus bas), Export All
      Patterns (MIDI)… (bulk : exporte tous les slots de pattern occupés, sur les 10 banks, en un
      seul `.zip` de fichiers `.mid` nommés `<Banque><2 chiffres>.mid`, ex. `A01.mid` — voir
      `sp404::exportAllPatternsToMidiZip`, `plugin/PatternArchive.h`, la version "toutes les
      banks" du bouton "Export MIDI…" du panneau Patterns), Load All Patterns (MIDI)… (l'inverse
      : lit un `.zip` produit par Export All Patterns, importe chaque entrée `<Banque><2
      chiffres>.mid` reconnue sur son slot d'origine via `sp404::importPatternFromMidi`/
      `writePattern` — voir `sp404::loadAllPatternsFromMidiZip`. Validé *avant* de toucher la
      carte : si le zip ne contient aucune entrée reconnaissable, rien n'est effacé et
      l'opération échoue proprement, comme `loadAllBanksFromZip`. Sinon, **tous** les patterns
      existants sont d'abord effacés — `sp404::clearAllPatterns` — puis remplacés par le contenu
      du zip, même logique "remplace entièrement, ne fusionne jamais" que le reste des
      chargements de cet app ; un slot absent du zip finit donc vide, pas laissé tel quel),
      Delete All Patterns (`sp404::clearAllPatterns`, `core/include/sp404/SdCard.h` — supprime
      les 120 `PTNxxxxx.BIN` possibles, slots déjà vides silencieusement ignorés ; contrepartie
      destructive d'Export All Patterns, même confirmation modale que Vider toutes les banks).
    - En dehors de ces deux sections (le menu ne les force pas dans un groupe, ni Bank ni
      Pattern à proprement parler) : le bascule offline/live et Sync Mirror → Card (item 12
      ci-dessous), puis Theme… (sélecteur de thème, item 14 plus bas).

    Le "Save As"/"Open" passe par un vrai sélecteur de fichier natif
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
    `sp404::replacePadSample(..., resetPlaybackDefaults=false)` (voir `plugin/SampleDsp.h`) :
    donc le trim utilisateur du pad se réinitialise sur le résultat (nouveau fichier, nouvelles
    bornes), mais **contrairement à un import par glisser-déposer** (item 10 plus haut, qui lui
    réinitialise volume/gate depuis 2026-08-10), volume/loop/gate/reverse/lofi sont ici toujours
    conservés — une action DSP retouche le sample déjà en place, ce n'est pas un nouvel import,
    et réinitialiser le volume à chaque Normalize/Trim/Fade serait une surprise plutôt qu'un
    service. Écriture immédiate, sans undo. Une ligne de
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
15. Patterns (menu "Patterns…") : liste les 12 slots de la banque actuellement affichée à
    l'écran (`WebUIBridge::listPatterns`, voir Roadmap ci-dessous pour le détail du format
    `PTN`/`sp404::Pattern`). Chaque slot avec un pattern enregistré affiche son nombre de
    mesures et les pads distincts qu'il référence ; un pad référencé mais sans sample est
    signalé (bordure rouge + `!`). L'adressage slot → fichier (`patternSlotPath`) reste une
    hypothèse best-effort au-delà de la banque A (voir `docs/sp404sx-format.md`), donc les
    patterns d'autres banques pourraient s'afficher au mauvais slot tant qu'un vrai pattern
    hors banque A n'a pas été vérifié. Chaque ligne a un bouton "Save…" (si un pattern existe)
    qui exporte un `.zip` bundlant le pattern et ses pads dépendants (`savePatternToZip`), et un
    bouton "Load…" qui restaure un tel `.zip` dans **cette** ligne précise — le slot cible peut
    différer du slot d'origine (voir Roadmap), mais les pads dépendants sont toujours restaurés
    à leur banque/pad d'origine, potentiellement différente de la banque affichée. "Copy…"/
    "Move…" ouvrent un petit sélecteur (grille de pads 1-12, occupés marqués `N!`) pour
    dupliquer/déplacer vers un autre slot **de la même banque** ; "Delete" supprime le
    `PTNxxxxx.BIN` du slot. Écriture immédiate, chaque action destructive (Load…/Move…/Delete,
    et Copy… vers un slot déjà occupé) confirmée par une modale (même règle "pas d'undo" que le
    reste). Sous le détail texte de chaque slot occupé, une frise ("aperçu") place un repère par
    événement réel à sa position temporelle dans le pattern, rouge si le pad qu'il joue n'a pas
    de sample — pratique pour repérer un trou avant de charger/déclencher le pattern. "Export
    MIDI…" (slot occupé) écrit un `.mid` standard (potards/pads mappés en note/canal MIDI,
    lisible dans n'importe quel DAW) ; "Import MIDI…" fait l'inverse — quantise un `.mid`
    quelconque sur la grille du pattern et écrit le résultat dans le slot cliqué (confirmé si le
    slot est déjà occupé). "▶ Play" déclenche la lecture temps réel du pattern (tempo-synchronisée
    à l'hôte, gelée si le transport hôte est en pause) — voir la section Roadmap "Triggering de
    pattern" ci-dessous pour le détail de ce qui est fait/pas fait, **notamment le fait que la
    lecture audio bout-en-bout n'a pas pu être vérifiée dans un vrai DAW dans cet environnement**.
    La ligne en cours de lecture se met en surbrillance (bordure orange) et son bouton devient
    "■ Stop" ; un seul pattern peut jouer à la fois.
16. Persistance de session DAW : changer de banque, activer le mode offline, bouger/mapper un
    potard, sauvegarder le projet DAW, le fermer et le rouvrir devrait restaurer exactement cet
    état (`PluginProcessor::getStateInformation`/`setStateInformation`, voir Roadmap ci-dessous).
    Ne restaure pas le contenu de la carte/des patterns eux-mêmes, qui vivent sur disque
    indépendamment du projet DAW. Non testé sur un vrai cycle save/close/reopen faute d'hôte
    disponible dans cet environnement — seulement vérifié par `auval` et relecture du code.

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
core/     bibliothèque C++ pure (PadInfo, Bank/Pad, SdCard, WavInfo, RlndChunk, Pattern,
          PatternPlayer) — zéro dépendance audio/GUI JUCE, testée indépendamment (PatternPlayer
          en particulier : scheduler de lecture de pattern temps réel, mais lui-même sans aucune
          dépendance JUCE ni allocation, donc testable offline malgré son usage audio-thread réel
          — voir la section Roadmap "Triggering de pattern").
plugin/   cible JUCE (VST3 + AU, synthé), éditeur hébergeant une WebView (JUCE 8
          WebBrowserComponent) qui appelle du code natif via des NativeFunction ;
          BankLoader (thread d'arrière-plan + lecture audio JUCE) et le routage MIDI/mixage
          dans PluginProcessor assurent la lecture réelle des pads. Un étage de sortie master
          (gain + EQ 3 bandes low/mid/high, module juce_dsp, voir plus haut) tourne dans
          PluginProcessor::processBlock, après le mixage des voix et avant la détection de
          clipping. SampleImport.h/.cpp décode/
          ré-échantillonne/ré-encode un fichier audio importé (voir plus haut) ; BankArchive.h/.cpp
          gère les sauvegardes/restaurations zip (menu de gestion des banks) ; PatternArchive.h/
          .cpp fait de même pour un pattern individuel (bundlé avec ses pads dépendants) ;
          PatternMidi.h/.cpp exporte/importe un pattern en fichier MIDI standard
          (juce::MidiFile) ; SampleDsp.h/.cpp
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
- Vélocité MIDI live ignorée : chaque pad joue toujours à son volume configuré (`PAD_INFO.BIN`),
  quelle que soit la vélocité envoyée par un contrôleur/clavier externe. La lecture de pattern,
  elle, modélise la vélocité enregistrée dans le pattern (voir la section Roadmap "Triggering de
  pattern") — une différence assumée : la vélocité d'un pattern est une donnée déjà enregistrée,
  pas quelque chose qu'un contrôleur doit envoyer correctement en temps réel.

## Roadmap (hors scope de cette mise en place)


- Détection de carte SD sur Windows/Linux si le projet s'étend au-delà de macOS (pour l'instant
  `findConnectedCardRoot()` ne scanne que `/Volumes`).
- ✅ **Chunk WAV `RLND`** (2026-08-10) : vérifié byte-for-byte contre une vraie carte SD SP-404SX
  (device, les 4 octets "unknown", `SampleIndex` — voir `docs/sp404sx-format.md`) puis branché
  dans le pipeline d'écriture (`sp404::encodeWavWithRlndChunk`, `core/include/sp404/WavRlnd.h`/
  `.cpp`, appelé depuis `plugin/SampleImport.cpp::encodeToWav`). **Cause racine identifiée et
  corrigée** d'un vrai problème utilisateur signalé pendant cette session (un sample importé via
  ce plugin apparaissait bien sur la carte SD mais le SP-404SX refusait de le charger sur un pad,
  avec une erreur à l'écran) : `encodeToWav` utilisait jusqu'ici `juce::WavAudioFormat`, un writer
  WAV générique qui n'écrit pas ce chunk custom Roland (et insère à la place son propre chunk
  `JUNK` d'alignement) — confirmé en trouvant, sur la carte réelle utilisée pour cette
  vérification, deux fichiers portant exactement cette signature (`A0000002.WAV`/`A0000006.WAV`),
  qui se sont avérés être des samples que ce plugin avait lui-même écrits lors de tests
  précédents dans cette session, sans le chunk requis. `encodeToWav` réutilise toujours le writer
  JUCE pour la conversion audio (float→16-bit PCM, déjà fiable) mais en extrait ensuite les
  octets PCM bruts (via `sp404::readWavInfo`, qui parcourt les chunks génériquement — peu importe
  la forme exacte du conteneur que JUCE a produit) pour les ré-empaqueter dans un fichier qui
  reproduit exactement la disposition d'un vrai fichier de la carte : chunk `fmt ` de 18 octets
  (16 champs PCM standards + extension `cbSize` à 0, comme sur la carte réelle plutôt que les 16
  octets minimaux de JUCE), puis le chunk `RLND` paddé pour que les données audio démarrent
  exactement à l'offset 512 (vérifié constant quel que soit le nombre de canaux/taux
  d'échantillonnage). En bonus, corrige aussi un second écart avec le matériel réel repéré au
  passage : `origSampleStart`/`origSampleEnd` dans `PAD_INFO.BIN` (calculés par
  `replacePadSample` à partir de l'offset du chunk `data`) valent maintenant 512 comme sur une
  vraie carte, au lieu d'un offset dépendant de la forme du conteneur JUCE.
  `sp404::padSampleIndex(bankChar, indexInBank)` (nouveau, `core/include/sp404/SdCard.h`)
  centralise le calcul de l'index 0-119 attendu par `SampleIndex`, utilisé par
  `plugin/SampleImport.cpp` et `plugin/SampleDsp.cpp`.
  **Vérification** : `encodeWavWithRlndChunk` comparé byte-for-byte à un vrai fichier `A0000001.WAV`
  de la carte réelle (fixture embarquée dans `core/tests/WavRlndTests.cpp`, comme les fixtures
  `PTN` déjà utilisées ailleurs) — **8 nouveaux tests** (byte-for-byte contre la fixture réelle,
  position de `data` à l'offset 512, padding d'un `pcmData` de longueur impaire, champ de taille
  RIFF patché correctement, retour vide pour des paramètres invalides) plus 2 pour
  `padSampleIndex`. Un harnais offline jetable (retiré après usage, comme d'habitude dans cette
  session) a en plus vérifié le pipeline `plugin/` complet bout-en-bout : `encodeToWav()` sur un
  signal de test → structure de fichier correcte (RLND/device/SampleIndex/`data` aux bons
  offsets) → relu avec succès par le lecteur WAV natif de JUCE lui-même (confirme qu'ajouter ce
  chunk custom ne casse pas la compatibilité WAV standard) → contenu audio identique à l'original
  à la quantification 16-bit près. 66/66 `ctest`, build propre, 3/3 `auval`. ✅ **Testé sur le vrai
  matériel (2026-08-10, confirmé par l'utilisateur)** : un sample réimporté via le plugin sur le
  pad A2 (qui était auparavant en échec, écrit avant ce correctif) se charge maintenant
  correctement sur un SP-404SX physique, là où l'ancien fichier échouait avec une erreur à
  l'écran.
- 🟡 **Équivalent RLND pour les patterns ?** (question posée le 2026-08-10) : le format `PTN`
  a bien sa propre "signature" (pied de fichier, octet 1 = `140` constant — voir la section
  format ci-dessous) et `sp404::encode(Pattern)`/`writePattern` l'écrivent déjà correctement
  (vérifié contre les 3 patterns réels), donc **pas** de lacune connue équivalente au bug `RLND`
  à ce niveau. Reste un fichier non analysé, `SMPL/STPINFO.BIN` (124 octets), que `writePattern`
  n'écrit/ne met jamais à jour — un premier dump ne montre aucune corrélation évidente avec les
  slots de pattern occupés, mais rien ne prouve non plus qu'il n'a aucun rôle (voir
  `docs/sp404sx-format.md`, section `STPINFO.BIN`, pour le détail de ce qui a été essayé).
  **Non fait** : trancher si importer un pattern MIDI sur un nouveau slot via le plugin (bouton
  "Import MIDI…") est bien reconnu par un SP-404SX physique sans toucher `STPINFO.BIN` — seul un
  test sur le vrai matériel peut le confirmer, comme ça l'a été pour `RLND`.
- ✅ Persistance d'état dans la session DAW (`PluginProcessor::getStateInformation`/
  `setStateInformation`, XML via `AudioProcessor::copyXmlToBinary`/`getXmlFromBinary` — la
  convention JUCE standard pour ça, plutôt que le JSON déjà utilisé ailleurs dans ce projet pour
  `Prefs.json`, pour rester dans les clous de ce que les hôtes/outils s'attendent à trouver dans
  l'état d'un plugin). Sauvegarde/restaure : la banque active (la *dernière demandée*, via un
  nouveau `lastRequestedBank`, pas forcément celle déjà chargée par `BankLoader` en tâche de
  fond au moment de la sauvegarde — sinon on sauvegarderait parfois la banque précédente) ; le
  mode offline/live ; la valeur et le mapping CC de chacun des 4 potards (nouveau `setKnobCc`
  pour restaurer un mapping sans repasser par le flow d'apprentissage MIDI). Volontairement
  **pas** sauvegardé : le contenu de la carte/des patterns, qui vit déjà sur disque (la carte
  réelle ou le miroir offline), pas dans l'état du projet DAW — un peu comme les patches d'un
  sampler matériel ne font pas partie d'un projet DAW non plus. Vérifié par `auval` (qui exerce
  l'aller-retour état lors de sa suite standard) et relecture attentive du code — contrairement
  au scheduler de pattern, ce mécanisme est un simple aller-retour d'attributs XML sans logique
  temporelle, donc un risque de régression nettement plus faible ; **non testé en revanche** sur
  un vrai cycle sauvegarde-projet/fermeture/réouverture dans un DAW réel (pas d'hôte disponible
  dans cet environnement).
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
  - ✅ Visualisation dans le plugin : menu "Patterns…" (nouveau `WebUIBridge::listPatterns`,
    args `[bankChar]`) liste les 12 slots de la banque actuellement affichée à l'écran, avec
    leur longueur en mesures et chaque pad distinct qu'ils référencent ; un pad référencé mais
    sans sample est signalé en rouge (bordure de la ligne + `!` sur le pad concerné). Relit le
    disque à chaque ouverture du panneau (même philosophie "toujours à jour" que `listPads` —
    pas seulement au moment où le pattern a été importé/enregistré, donc un sample supprimé/
    remplacé après coup apparaît comme manquant dès la prochaine ouverture). Vérifié contre les
    3 vrais patterns de la carte (voir `docs/sp404sx-format.md`) : `A1` → 14 mesures/C6-C12,
    `A9` → 4 mesures/D9-D11, `A12` → 14 mesures/I6-I8, tous les pads référencés bien présents.
  - ✅ Sauvegarder/charger un pattern (`plugin/PatternArchive.h`/`.cpp`, boutons "Save…"/"Load…"
    sur chaque ligne du panneau "Patterns…") : `savePatternToZip` bundle le `PTNxxxxx.BIN` brut
    du slot avec, pour chaque pad distinct qu'il référence, sa tranche `PAD_INFO.BIN` (32
    octets) et son fichier échantillon — jamais le `PTNxxxxx.BIN` seul, sinon restaurer ailleurs
    rejoue n'importe quoi. Contrairement à `BankArchive::saveBankToZip`, les pads référencés
    gardent leur identité banque+pad absolue dans le zip (pas de repositionnement) : les octets
    bruts du pattern codent en dur quelle banque/pad chaque événement joue (voir
    `PatternEvent::bank()`/`padIndexInBank()`), donc charger ce bundle dans un *autre* slot ne
    change pas ce qu'il joue — seul le remplacement des pads dépendants (même banque+pad
    qu'à l'origine, écrasement complet comme `clearPad`) est nécessaire si ces pads manquent sur
    la carte cible. `loadPatternFromZip` retourne les banques touchées (slot cible + banques des
    pads dépendants, potentiellement différentes) pour que l'UI sache quoi rafraîchir. Testé par
    un round-trip complet (sauvegarde de `A1` depuis la vraie carte, chargement dans un slot
    différent `B5` sur une carte synthétique, vérification des octets/samples/PadInfo restaurés)
    via un harnais offline jetable avant intégration à l'UI.
  - ✅ Dupliquer/déplacer/supprimer un pattern entre slots (`sp404::copyPatternSlot`/
    `clearPatternSlot`, `core/include/sp404/SdCard.h`) — boutons "Copy…"/"Move…"/"Delete" sur
    chaque ligne du panneau "Patterns…", avec un sélecteur compact (grille 1-12) pour choisir le
    pad cible **au sein de la même banque** (les slots déjà occupés sont marqués `N!` dans le
    sélecteur). Réorganiser vers une **autre banque** fonctionne déjà via Save…+Load… (pas de
    nouveau sélecteur banque+pad construit pour ce tour, volontairement — Save…/Load… couvre déjà
    ce cas, y compris entre deux cartes). Pas de renommage : le format `PTN` n'a pas de champ nom
    (voir `docs/sp404sx-format.md`), donc contrairement aux banks il n'y a rien à renommer.
    "Move…" est composé côté JS (`copyPattern` puis `deletePattern`, pas de nouvel endpoint
    natif dédié) ; "Copy…"/déplacer ne touchent jamais aux pads dépendants (contrairement à
    Save…/Load…) puisque source et cible restent sur la même carte.
  - ✅ Aperçu en lecture seule (mini-timeline) : chaque ligne du panneau "Patterns…" affiche une
    frise sous le détail texte — un repère par événement réel, positionné à sa position absolue
    en ticks (`sp404::absoluteEventTicks`/`Pattern::totalTicks()`, nouveaux dans
    `core/include/sp404/Pattern.h`, testés contre les fixtures réelles), coloré en rouge si le
    pad qu'il joue n'a pas de sample (même code couleur que les refs textuelles). Lignes de
    mesure en fond (`repeating-linear-gradient`, une par mesure). `WebUIBridge::listPatterns`
    expose `events`/`totalTicks` par slot pour ça, en plus de `referencedPads` déjà présent.
    Limité pour l'instant aux patterns déjà sur la carte (le panneau "Patterns…") — pas encore
    d'aperçu du contenu d'un `.zip` avant confirmation de "Load…" (nécessiterait de parser
    `PATTERN.BIN` directement depuis le zip en mémoire, non fait dans ce tour).
  - ✅ Export/import MIDI (`plugin/PatternMidi.h`/`.cpp`, boutons "Export MIDI…"/"Import
    MIDI…" sur chaque ligne du panneau "Patterns…") via `juce::MidiFile`/`MidiMessageSequence`
    (pas de parseur SMF maison). Chaque pad est mappé à la note MIDI (`kBasePadNote + pad - 1`,
    même convention que le triggering live existant, voir `PluginProcessor::kBasePadNote`) sur
    le canal `1 + banque` (A=canal 1 … J=canal 10) — la banque passe par le canal plutôt que par
    la note pour que les 120 pads restent tous adressables sans dépasser la plage MIDI 0-127.
    Import quantise sur la grille native de 384 ticks/bar (`sp404::kTicksPerBar`) et écrit
    directement le pattern résultant sur le slot cible (`writePattern`, nouveau : `core/` avait
    jusqu'ici seulement la lecture — `PatternEvent::encode`/`sp404::encode(Pattern)` ajoutés en
    même temps, avec un test qui réencode un vrai pattern et compare octet à octet à l'original).
    Round-trip vérifié via un harnais offline jetable sur les 3 patterns réels (bytes des
    fixtures déjà embarquées dans `PatternTests.cpp`, carte physique déconnectée entre-temps) :
    tick/banque/pad/vélocité identiques à 100% après export→import ; seule la *durée* d'un hit
    peut légèrement différer quand le même pad est re-déclenché avant la fin du hit précédent
    (chevauchement) — limitation inhérente à l'appariement note-on/note-off du format MIDI
    standard, sans impact réel puisque ce plugin ne joue qu'une seule voix par pad de toute
    façon (un nouveau déclenchement coupe déjà le précédent, voir Limitations plus haut). Un
    marqueur de fin de piste explicite (`juce::MidiMessage::endOfTrack()`, positionné au vrai
    nombre de mesures) préserve le silence de fin d'un pattern à l'export/import, qui serait
    sinon perdu (aucune note n'ancre sa position).
  - ✅ **Export MIDI en masse** (2026-08-10, `sp404::exportAllPatternsToMidiZip`,
    `plugin/PatternArchive.h`/`.cpp`, menu "Export All Patterns (MIDI)…" — voir item 11 plus
    haut) : bulk de l'export MIDI par slot ci-dessus, plutôt qu'un nouveau mécanisme — parcourt
    les 120 slots possibles (10 banks × 12), réutilise `exportPatternToMidi` pour chaque slot
    occupé (silencieusement ignoré si vide, pas une erreur) et bundle le tout dans un seul
    `.zip` (`juce::ZipFile::Builder`, entrées `<Banque><2 chiffres>.mid`). Les `.mid`
    intermédiaires sont écrits dans un dossier temporaire (l'API `exportPatternToMidi` existante
    n'a pas de variante en mémoire) puis supprimés une fois le zip écrit, succès ou échec.
    Vérifié via un harnais offline jetable **contre la carte réelle reconnectée** : exportait
    exactement les 3 patterns réels présents à l'époque (`A01.mid`/`A09.mid`/`A12.mid`,
    correspondant aux 3 fichiers `PTN` déjà vérifiés dans `docs/sp404sx-format.md`), zéro faux
    positif/négatif sur les slots vides restants. 66/66 `ctest` (inchangé — logique de bundle
    simple au-dessus d'une fonction déjà testée, pas de nouveau test dédié), build propre, 3/3
    `auval`.
  - ✅ **Load MIDI en masse** (2026-08-10, `sp404::loadAllPatternsFromMidiZip`,
    `plugin/PatternArchive.h`/`.cpp`, menu "Load All Patterns (MIDI)…") : inverse de l'export
    ci-dessus. Repère chaque entrée du zip nommée `<Banque><2 chiffres>.mid` (réutilise le même
    analyseur de nom que `loadPatternFromZip`, `parsePadEntryName`), **valide qu'il y en a au
    moins une avant de toucher quoi que ce soit sur la carte** (même principe que
    `BankArchive::loadAllBanksFromZip` : un zip non reconnaissable ne doit rien effacer), puis
    efface tous les patterns existants (`sp404::clearAllPatterns`, ajoutée avec Delete All
    Patterns ci-dessous) avant d'importer chaque entrée trouvée sur son slot via
    `importPatternFromMidi`/`writePattern` — "remplace entièrement", pas une fusion, même
    philosophie que tous les autres "Load" de cette app. Vérifié via un harnais offline jetable
    **contre la carte réelle reconnectée** (devenue 5 patterns entre-temps, l'utilisateur en
    ayant ajouté depuis la vérification de l'export ci-dessus) : export→chargement dans une
    carte synthétique de test avec un pattern-leurre pré-existant sur un slot absent du zip (B5)
    — les 5 patterns réels restaurés sur leurs bons slots, le leurre bien effacé (confirme le
    remplacement complet plutôt qu'une fusion), nombre de mesures identique à l'original pour
    chaque pattern vérifié. 67/67 `ctest` (dont 1 nouveau, `clearAllPatterns` — le reste de la
    logique bundle/validation n'a pas de nouveau test dédié, même raisonnement que l'export),
    build propre, 3/3 `auval`.
  - 🟡 Triggering d'un pattern entier depuis le DAW, synchronisé tempo/transport hôte — fait
    partiellement, voir le détail complet ci-dessous.

### Triggering de pattern (`sp404::PatternPlayer`, `PluginProcessor`) — état détaillé

**Fait** : un bouton "▶ Play" sur chaque ligne du panneau "Patterns…" déclenche la lecture
temps réel du pattern, **synchronisée au tempo (BPM) de l'hôte**, **asservie à son
lecteur** (play/pause de l'hôte = avance/gèle le pattern), et **quantifiée au démarrage sur la
prochaine mesure de l'hôte** (voir le nouveau bullet ci-dessous) — ce n'est **pas** encore un
alignement *continu* sur la *position* du transport hôte pendant toute la lecture (voir "Non
fait" plus bas).

- `core/include/sp404/PatternPlayer.h`/`.cpp` (nouveau, 100% C++ pur, zéro dépendance JUCE) :
  scheduler qui convertit les ticks du pattern en échantillons via le BPM courant
  (`kTicksPerBar`/4 = ticks/temps), boucle indéfiniment jusqu'à l'arrêt, et gère les
  changements de tempo en cours de lecture sans réinterpréter rétroactivement les échantillons
  déjà avancés (`setTempo` "commit" la position atteinte avant de changer de taux). Conçu pour
  être piloté une fois par bloc audio (`advance(numSamples, ...)`) mais sans aucune allocation
  ni I/O propre, donc testable offline — **10 tests** dans `PatternPlayerTests.cpp`, dont deux
  tests de dérive (un sur ~700 blocs de taille impaire couvrant une boucle, un sur 200 boucles à
  une taille de bloc réaliste de 512 échantillons) qui ont effectivement détecté un vrai bug
  d'accumulation d'erreur flottante lors de l'écriture initiale (corrigé en recalculant la
  position en ticks à chaque appel depuis un compteur d'échantillons entier exact plutôt qu'en
  accumulant des résultats déjà arrondis).
- `PluginProcessor` intègre le scheduler dans `processBlock` : lit `getPlayHead()` une fois par
  bloc (BPM + `isPlaying`), consomme un pattern nouvellement déclenché via un handoff
  SpinLock+shared_ptr identique à celui déjà utilisé par `BankLoader` pour `currentBank` (voir
  `BankLoader.h`), puis fusionne les événements du pattern avec le vrai buffer MIDI (tri par
  position d'échantillon, sans allocation) pour déclencher les pads au bon instant.
- **Pool de voix séparé** (`patternVoices`, polyphonie propre de 4) plutôt que de réutiliser le
  tableau `voices` existant (12 emplacements, un par pad de la banque actuellement armée) : un
  pattern peut référencer une banque différente de celle armée pour le MIDI live (voir
  `docs/sp404sx-format.md`), donc un déclenchement de pattern ne doit jamais entrer en conflit
  avec — ni être limité par — la lecture live/preview en cours. `triggerVoice()` généralise la
  logique de vol de voix déjà existante dans `handleMidiMessage` (mêmes règles : retrigger du
  même pad ne compte pas dans la polyphonie, le plus ancien est coupé en premier) sans toucher
  au chemin MIDI live d'origine, pour garantir zéro risque de régression sur ce qui marchait déjà.
- ✅ **Note-off/gate** (ajouté après le tour initial) : `PatternPlayer` calcule, pour chaque
  note-on avec une durée (`lengthTicks`) non nulle, le tick absolu de sa fin et programme un
  événement de note-off correspondant (`PatternTriggerEvent::isNoteOff`) — potentiellement
  plusieurs blocs (voire plusieurs boucles) plus tard, suivi via une petite liste interne
  (`pendingNoteOffs`, vidée par `stop()`/`start()` pour ne jamais laisser une note-off d'un
  ancien pattern resurgir sur le suivant). `PluginProcessor` applique alors exactement la même
  règle que le MIDI live (`handleMidiMessage`) : un pad en mode `gate` est coupé à ce moment,
  un pad sans `gate` continue jusqu'à sa fin naturelle. Comme `advance()` peut désormais
  produire des événements pas strictement triés par tick (une note très courte peut voir sa
  note-off dépasser une note-on suivante dans l'ordre du vecteur), `processBlock` trie
  `patternTriggerScratch` par position d'échantillon avant la fusion avec le buffer MIDI. À ce
  stade, la vélocité n'était pas encore modélisée (comme le MIDI live, qui l'ignore — voir le
  bullet dédié plus bas pour ce qui a changé depuis côté patterns).
- **Corrections annexes découvertes en implémentant ceci** : "Stop All" (bouton Cancel/Stop
  général) ne coupait pas les voix de pattern ni le scheduler lui-même — corrigé, un Stop All
  arrête maintenant tout sans exception. "Stop" sur un pattern spécifique coupe désormais aussi,
  sans condition de `gate`, toute voix de pattern encore active au moment de l'arrêt — sinon un
  pad `gate`+`loop` resterait bloqué à boucler indéfiniment puisque plus aucune note-off future
  ne viendrait jamais le couper une fois le scheduler arrêté.
- ✅ **Quantification du démarrage sur la prochaine mesure hôte** (ajouté après le tour
  note-off) : plutôt qu'un `PatternPlayer::start()` immédiat, `triggerPattern()` "arme" le
  pattern (`PluginProcessor::pendingLaunchPattern`) et calcule, via la nouvelle fonction pure
  `sp404::nextBarBoundaryPpq(currentPpq, timeSigNumerator, timeSigDenominator)`
  (`core/include/sp404/PatternPlayer.h`/`.cpp`, zéro dépendance JUCE comme le reste du
  scheduler), la position PPQ de la prochaine limite de mesure hôte à partir de
  `AudioPlayHead::PositionInfo::getPpqPosition()`/`getTimeSignature()`. Chaque bloc audio suivant
  vérifie si la position PPQ courante de l'hôte a atteint ce point pour démarrer réellement le
  `PatternPlayer` à ce moment précis — ce qui fait qu'un pattern déclenché en plein milieu d'une
  mesure hôte attend la limite de mesure suivante plutôt que de démarrer immédiatement en
  décalage, comme le fait un clip launcher classique (Ableton Live et équivalents). Repli sur un
  démarrage immédiat si l'hôte ne rapporte pas de position PPQ du tout (hôte minimal/standalone
  sans concept de transport) — le comportement précédent, inchangé dans ce cas. `Stop All` et
  `Stop` (sur un pattern) annulent désormais aussi un déclenchement armé mais pas encore
  démarré (`pendingLaunchPattern = nullptr`), pour qu'un Stop juste après un Play ne fasse pas
  démarrer le pattern par surprise à la mesure suivante. `patternPlayingFlag` (état affiché côté
  UI) passe à vrai dès l'armement, pas seulement au démarrage effectif, pour que l'utilisateur
  ait un retour immédiat que le déclenchement a bien été pris en compte pendant l'attente.
  `nextBarBoundaryPpq` couvre aussi les mesures non-4/4 (testé en 3/4 et 6/8) et retombe sur 4/4
  si l'hôte ne rapporte pas de signature rythmique valide. **4 nouveaux tests** dans
  `PatternPlayerTests.cpp` : position déjà exactement sur une limite de mesure (retour immédiat,
  pas d'attente d'une mesure entière inutile), arrondi vers le haut en plein mesure (4/4),
  signatures non-4/4, repli sur 4/4 pour une signature invalide.
- ✅ **Vélocité des événements de pattern** (ajouté après le tour de quantification) :
  `Voice` (`PluginProcessor.h`) gagne un `velocityGain` (gain linéaire, 1.0 par défaut) appliqué
  en plus du volume configuré du pad (`renderVoices` : `gain = pad.info.volume/127 *
  voice.velocityGain`). `triggerVoice()` (le chemin *uniquement* utilisé par la lecture de
  pattern, jamais par le MIDI live, voir plus haut) prend désormais un paramètre `velocity`
  (0-127, directement `PatternTriggerEvent::velocity`, lui-même recopié de l'octet vélocité de
  l'événement `PTN` d'origine — déjà décodé sans être utilisé jusqu'ici) et fixe
  `velocityGain = velocity/127`. **Volontairement scindé du MIDI live** : `handleMidiMessage`
  continue de fixer `voice.velocityGain = 1.0f` explicitement sur son propre chemin
  (`voices`, jamais `triggerVoice()`) — changer aussi le comportement du MIDI live n'était pas
  demandé et casserait un comportement déjà établi (chaque pad joue à son volume configuré, quel
  que soit ce qu'envoie un contrôleur), voir Limitations. Sur les 3 patterns réels déjà utilisés
  pour vérifier le format (voir plus haut), l'octet vélocité vaut `127` sur tous les événements
  réels (`0` seulement sur les placeholders, qui ne déclenchent rien) — donc ce changement ne
  modifie le volume perçu d'aucun pattern déjà testé contre du matériel réel, mais prend
  maintenant en compte une vélocité différente si un pattern en contient une (import MIDI avec
  vélocité variable, ou futur pattern réel enregistré avec une frappe plus légère sur certains
  hits).
- **Vérification** : modification côté `plugin/` uniquement (pas de nouveau code dans `core/`,
  donc pas de nouveau test `ctest` — `PluginProcessor` ne peut pas être testé hors du plugin
  réel, voir la note sur `JucePlugin_Name` plus haut dans ce fichier) : 59/59 `ctest` (inchangé),
  build complet propre, 3/3 `auval`. Relecture attentive du code (le calcul est un simple produit
  de deux gains linéaires, même famille de risque que le gain de volume déjà en place) plutôt
  qu'un test dédié. **Même limite qu'avant** : ni ce changement ni le reste de la lecture de
  pattern n'ont pu être vérifiés à l'oreille dans un vrai DAW dans cet environnement (pas d'hôte
  disponible) — à tester manuellement.

**Non fait** : alignement *continu* sur la *position* du transport hôte pendant toute la durée
de la lecture (un pattern déjà lancé ne se recale jamais sur la timeline hôte si celle-ci saute/
boucle/est déplacée manuellement en cours de route — seul le *démarrage* est quantifié sur la
prochaine mesure, voir ci-dessus ; gérer un saut/bouclage arbitraire de l'hôte en cours de
lecture nécessiterait de repenser le modèle de suivi des note-off en attente, qui suppose une
progression monotone) ; jouer plusieurs patterns simultanément (un seul `PatternPlayer` par
instance de plugin actuellement).

