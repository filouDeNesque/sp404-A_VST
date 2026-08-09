# Plan de tests de performance (manuel, en conditions réelles DAW)

Ce document **ne décrit pas une suite automatisée** : les scénarios ci-dessous sont pensés
pour être exécutés à la main, dans un vrai DAW (Ableton Live, Logic, Reaper...), avec un vrai
projet MIDI et — idéalement — une vraie carte SD SP-404SX branchée. L'objectif est de repérer
les points chauds réels (thread audio, thread `BankLoader`, polling WebView, DSP hors-ligne)
plutôt que de mesurer des micro-benchmarks isolés qui ne refléteraient pas l'usage réel.

Chaque scénario liste : ce qu'il faut faire dans le DAW, ce qu'il faut observer, avec quel
outil, et un seuil indicatif de "ça sent le problème". Les seuils sont des points de départ à
ajuster une fois les premières mesures en main, pas des specs figées.

## Outils de mesure

- **CPU meter du DAW** (par piste + total) : première ligne de défense, donne le signal le
  plus proche de ce qu'un vrai utilisateur verrait (voyant rouge de surcharge, dropouts audio
  audibles/loggés par le DAW).
- **Instruments (Xcode)**, profil *Time Profiler* attaché au process du DAW (ou à `auval` pour
  l'AU en standalone) : pour distinguer le temps passé dans `processBlock`
  ([`plugin/PluginProcessor.cpp`](../plugin/PluginProcessor.cpp)) de celui passé dans le
  thread `BankLoader` ou le thread WebView.
- **Instruments, profil *System Trace*** : utile spécifiquement pour voir si le thread audio
  se fait jamais bloquer (priority inversion, wait sur un lock) par le `juce::SpinLock` de
  `BankLoader::bankLock` ou par une écriture disque déclenchée depuis `WebUIBridge`.
- **`log stream` / console.log dans la WebView** (DevTools JUCE 8, si activables) : pour
  horodater côté JS le moment d'un clic/drop et comparer à l'horodatage du callback natif —
  utile pour les scénarios de latence perçue (gate, bank switch, import).
- **Chronomètre humain / vidéo à 60fps du contour orange d'un pad** : pour le scénario gate,
  le plus fiable reste de filmer l'écran en relâchant une note et de compter les frames jusqu'à
  l'extinction du contour.
- **`ls -la` / `du -sh` sur la carte SD ou le miroir offline** avant/après, pour confirmer
  qu'une opération d'écriture a bien la taille attendue sans avoir à instrumenter le code.

## Scénarios

### 1. Charge du thread audio — polyphonie et voice stealing

**Pourquoi** : `kMaxPolyphony = 2` ([`PluginProcessor.h:25`](../plugin/PluginProcessor.h))
signifie qu'un 3ᵉ pad déclenché coupe le plus ancien avec un fade — c'est le chemin le plus
sensible de `processBlock`/`renderVoices`.

- Poser le plugin sur une piste instrument, jouer un pattern MIDI qui déclenche rapidement 3+
  pads différents en boucle serrée (16 croches à 160+ BPM) pendant plusieurs minutes.
- Faire tourner en parallèle plusieurs pistes utilisant d'autres plugins gourmands, pour
  simuler un vrai projet chargé plutôt qu'un projet vide avec un seul plugin.
- Observer : CPU meter du DAW, présence de clics/dropouts audibles, `isClipping()` du plugin
  lui-même (contour rouge de l'écran) vs. clipping réel du DAW.
- Tester à plusieurs tailles de buffer (64, 128, 256, 512, 1024 samples) et deux sample rates
  (44.1kHz natif, 48kHz pour vérifier l'absence de resampling à la lecture mentionnée dans le
  README).
- Seuil indicatif : aucun dropout audible en dessous de 256 samples de buffer sur une machine
  de milieu de gamme, avec 8 instances du plugin actives simultanément.

### 2. Bank switch — latence thread `BankLoader`

**Pourquoi** : `requestBank()` déclenche une relecture disque complète de 12 pads depuis le
thread `BankLoader` ([`BankLoader.h:47`](../plugin/BankLoader.h)), potentiellement depuis une
vraie carte SD sur lecteur USB lent.

- Envoyer un Program Change MIDI (changement de banque) pendant qu'un pad de la banque
  précédente est encore en train de jouer (pad sans `gate`, donc qui devrait continuer à
  sonner) — vérifier que le changement de banque n'interrompt pas ce qui joue déjà.
- Chronométrer, depuis un vrai lecteur de carte SD USB (pas un SSD interne rapide), le délai
  entre le Program Change et le moment où un pad de la nouvelle banque répond correctement au
  premier trigger MIDI qui suit.
- Répéter en mode offline (miroir local) pour comparer : le miroir devrait être nettement plus
  rapide, ce qui confirme que le goulot est bien l'I/O carte et pas le parsing.
- Seuil indicatif : < 150 ms pour un miroir local SSD ; à mesurer sans a priori pour une vraie
  carte SD (documenter le chiffre trouvé plutôt que de deviner).

### 3. Réactivité `gate` — relâchement de note

**Pourquoi** : le README affirme explicitement que le relâchement d'un pad en mode `gate` doit
couper "quasi immédiatement" et que ça a été vérifié niveau audio contre le vrai hardware — un
bon candidat pour une régression silencieuse de latence perçue.

- Sur un pad `gate=true`, jouer une note longue puis la relâcher ; filmer l'écran (60fps) pour
  mesurer en frames le délai entre le relâchement MIDI et l'extinction du contour orange
  (`getActivePadMask()` pollé toutes les 100ms côté UI, voir `pollActivePads` dans
  [`app.js`](../plugin/web/app.js)) **et**, séparément, écouter/mesurer la coupure audio réelle
  (qui ne dépend pas du polling UI).
- Bien distinguer les deux : la coupure **audio** doit être quasi instantanée (c'est le thread
  audio) ; l'extinction du **contour UI** est plafonnée par le polling à 100ms
  (`setInterval(pollActivePads, 100)`, [`app.js:990`](../plugin/web/app.js)) et ce n'est pas un
  bug si elle prend jusqu'à ~100ms de plus.
- Seuil indicatif : coupure audio perceptible en dessous de ~10ms après note-off ; extinction
  visuelle en dessous de ~150ms (100ms de poll + marge).

### 4. Coût du polling WebView en régime permanent

**Pourquoi** : quatre `setInterval` tournent en continu dès que l'éditeur est ouvert —
`pollActivePads`/`pollClipping` à 100ms, `pollKnobs` à 150ms, `pollConnection` à 2000ms
(voir [`app.js:990-993`](../plugin/web/app.js)) — chacun fait un aller-retour natif.

- Laisser l'éditeur du plugin ouvert (fenêtre visible) plusieurs dizaines de minutes sans
  interaction, dans un projet par ailleurs actif (lecture en cours), et observer si le CPU de
  l'instance dérive dans le temps (fuite, accumulation) plutôt que de rester plat.
- Comparer le CPU avec éditeur fermé vs. ouvert : l'écart doit correspondre uniquement au coût
  du polling, pas à autre chose.
- Ouvrir simultanément plusieurs instances du plugin (si le DAW le permet dans un même projet)
  pour vérifier que le coût du polling scale linéairement et ne dégénère pas (contention sur un
  lock partagé, par exemple).
- Seuil indicatif : CPU plat dans le temps (pas de dérive) ; coût marginal par instance
  raisonnable au regard du reste du projet.

### 5. Import / drag & drop d'un gros fichier

**Pourquoi** : le chemin "vrai fichier OS" encode en base64 côté JS par blocs
(`arrayBufferToBase64`) avec retour périodique à la boucle d'événements pour ne pas geler
l'hôte, et suspend le polling UI pendant le survol du drag (voir README point 10).

- Glisser un fichier volumineux (WAV stéréo 24-bit, plusieurs minutes — bien au-delà d'un
  sample de pad typique) depuis le Finder sur un pad, pendant que d'autres pistes du projet
  jouent.
- Observer si l'audio des autres pistes reste fluide pendant l'encodage/import (le point
  sensible est justement que ce chemin ne doit pas geler l'hôte).
- Comparer le temps total d'import selon le format source (wav/aiff déjà au bon sample rate vs.
  mp3/m4a nécessitant décodage CoreAudioFormat + resampling vers 44.1kHz).
- Refaire le test avec le chemin "texte seulement" (`swapPadSampleFromPath`, lecture directe
  disque côté natif) pour comparer les deux chemins.
- Seuil indicatif : zéro dropout audio perceptible sur les autres pistes pendant l'import,
  quelle que soit la taille du fichier importé.

### 6. Panneau DSP — opérations coûteuses (BPM, Rubber Band)

**Pourquoi** : la détection BPM (autocorrélation) et surtout le pitch/time-stretch via Rubber
Band (moteur R3/"Finer", voir [`SampleDsp.h`](../plugin/SampleDsp.h)) sont les traitements
offline les plus lourds du plugin.

- Lancer un traitement DSP (normaliser, trim silence, BPM, pitch/stretch) sur un pad pendant
  que le transport du DAW joue activement d'autres pistes — vérifier qu'il n'y a pas de
  blocage du thread message/UI perceptible ailleurs dans le DAW (menus qui freezent, etc.).
- Chronométrer chaque opération sur des échantillons de longueurs croissantes (1s, 5s, 30s —
  au-delà, ce n'est plus un usage réaliste de pad SP-404) pour voir si le temps scale
  linéairement ou dégénère.
- Cas spécifique Rubber Band : mesurer un stretch extrême (ratio très éloigné de 1, plusieurs
  demi-tons de pitch) qui est probablement le pire cas de temps de calcul.
- Seuil indicatif : pas de gel perceptible de l'UI du DAW pendant le traitement ; temps de
  traitement Rubber Band de l'ordre de quelques centaines de ms à quelques secondes pour un
  sample de pad typique (2-10s), pas de dérive au-delà du raisonnable.

### 7. Sauvegarde/restauration de banks (zip)

**Pourquoi** : "Sauvegarder toutes les banks" zippe l'intégralité de `SMPL/`
(`sp404::saveAllBanksToZip`, voir [`BankArchive.h`](../plugin/BankArchive.h)) — potentiellement
gros volume de données, via `juce::FileChooser` async.

- Avec une carte SD réellement remplie (les 10 banques, samples variés), lancer "Sauvegarder
  toutes les banks" et chronométrer, en observant si le DAW reste réactif pendant l'opération
  (transport, autres plugins).
- Refaire pour "Charger toutes les banks" (écriture, plus lourd que la lecture) et pour les
  variantes par bank unique (plus rapide, sert de point de comparaison).
- Tester aussi "Sync Mirror → Card" en mode offline avec un vrai transfert vers carte SD USB.
- Seuil indicatif : le DAW ne doit jamais devenir totalement non-réactif ; documenter le temps
  réel observé plutôt que de fixer un seuil arbitraire tant qu'aucune mesure de référence
  n'existe.

### 8. Ouverture/fermeture répétée de l'éditeur

**Pourquoi** : la WebView (JUCE 8 `WebBrowserComponent`) a un coût d'initialisation ; ouvrir/
fermer l'éditeur en boucle est un scénario réaliste (utilisateur qui navigue entre plusieurs
plugins dans son DAW) et un bon test de fuite mémoire.

- Ouvrir/fermer l'éditeur du plugin une trentaine de fois de suite pendant que le projet joue,
  en observant la mémoire du process DAW (Activity Monitor ou Instruments *Allocations*) pour
  détecter une fuite (mémoire qui ne redescend jamais après fermeture).
- Seuil indicatif : mémoire stable après fermeture (retour proche du niveau avant ouverture, à
  la fragmentation près) sur les 30 cycles.

### 9. Passage à l'échelle — plusieurs instances dans un même projet

- Charger le plugin sur 8-16 pistes MIDI différentes dans un même projet DAW, chacune jouant un
  pattern différent, et vérifier que le CPU total scale à peu près linéairement plutôt que de
  dégénérer (contention sur un état partagé quelconque — improbable vu que chaque instance a
  son propre `BankLoader`/`PluginProcessor`, mais à vérifier empiriquement plutôt que supposé).

## Fiche de résultat (à dupliquer par run)

| Scénario | Machine / OS | Buffer / SR | Résultat mesuré | Dropout/glitch ? | Notes |
|---|---|---|---|---|---|
| 1. Polyphonie/voice stealing | | | | | |
| 2. Bank switch | | | | | |
| 3. Latence gate | | | | | |
| 4. Polling WebView | | | | | |
| 5. Import gros fichier | | | | | |
| 6. DSP (BPM/Rubber Band) | | | | | |
| 7. Zip banks | | | | | |
| 8. Open/close éditeur | | | | | |
| 9. Multi-instances | | | | | |

## Hors scope de ce plan

- Tout benchmark micro/synthétique qui ne passe pas par un vrai DAW hôte (biaiserait la mesure
  de ce qui compte vraiment : l'expérience réelle en session).
- La comparaison inter-DAW (Ableton vs Logic vs Reaper) n'est pas un objectif premier — utile
  seulement si un problème de performance s'avère spécifique à un hôte donné.
- L'automatisation de ces scénarios (CI de perf) n'est pas envisagée ici : la nature même des
  points sensibles (I/O carte SD réelle, thread WebView, DAW hôte) rend un test manuel plus
  représentatif qu'un mock pour l'instant.
