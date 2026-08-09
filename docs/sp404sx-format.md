# Format de la carte SD Roland SP-404SX

Statut : **partiellement vérifié contre une vraie carte SD SP-404SX** (2026-08-08, PTN ajouté
2026-08-09). L'emplacement du fichier d'index a été corrigé suite à cette vérification (voir plus
bas) ; la table d'octets de `PAD_INFO.BIN` a été confirmée valeur par valeur sur un vrai
enregistrement de pad, et le format `PTN/PTNxxxxx.BIN` (patterns) a été vérifié contre 3 vrais
fichiers de la carte. Le chunk WAV `RLND` reste non vérifié (voir "Points à vérifier").

## Sources

- [Gist de Paul Battley — "Roland SP-404SX sample file format"](https://gist.github.com/threedaymonk/701ca30e5d363caa288986ad972ab3e0)
  (source primaire, cité par `super-pads`).
- [`uttori-audio-padinfo`](https://github.com/uttori/uttori-audio-padinfo) — lib JS qui lit/écrit `PADINFO.BIN`.
- [`uttori-audio-wave`](https://github.com/uttori/uttori-audio-wave) — lib JS qui lit/écrit le chunk `RLND`.
- [`super-pads`](https://github.com/MatthewCallis/super-pads) — application Electron qui utilise les deux libs ci-dessus.

## Organisation des fichiers sur la carte

```
<racine carte SD>/
  ROLAND/
    SP-404SX/
      PTN/
        PTN00001.BIN
        ...
      SMPL/
        PAD_INFO.BIN
        STPINFO.BIN
        A0000001.WAV
        A0000002.WAV
        ...
        J0000012.WAV
```

- Un fichier son par pad, nommé `<Banque><7 chiffres>.WAV` (ou `.AIF`), où `<Banque>` est une
  lettre `A`–`J` et les 7 chiffres encodent le numéro de pad (zero-padded) — **vérifié**
  identique sur une vraie carte (`A0000001.WAV`, `B0000001.WAV`, ...).
- 10 banques (A à J) × 12 pads = 120 pads au total.
- **Correction** : le fichier d'index s'appelle `PAD_INFO.BIN` (avec underscore) et vit dans
  `SMPL/`, pas `ROLAND/SP-404SX/PADINFO.BIN` comme l'affirmaient le gist de Paul Battley et
  `uttori-audio-padinfo` (ou comme on les a compris). Constaté directement sur une carte SD
  SP-404SX réelle montée localement.

## `SMPL/PAD_INFO.BIN`

120 enregistrements de 32 octets chacun, un par pad, dans l'ordre A1, A2, ..., A12, B1, ...,
J12 — taille de fichier vérifiée (3840 octets = 120 × 32) sur une vraie carte. Toutes les
valeurs multi-octets sont en **big-endian**.

| Offset | Champ | Type | Notes |
|---|---|---|---|
| 0–3 | `OrigSampleStart` | u32 | Point de départ d'origine dans les données audio |
| 4–7 | `OrigSampleEnd` | u32 | Point de fin d'origine |
| 8–11 | `UserSampleStart` | u32 | Point de départ ajusté par l'utilisateur |
| 12–15 | `UserSampleEnd` | u32 | Point de fin ajusté par l'utilisateur |
| 16 | `Volume` | u8 | 0–127 |
| 17 | `Lofi` | u8 | 0 ou 1 |
| 18 | `Loop` | u8 | 0 ou 1 |
| 19 | `Gate` | u8 | 0 ou 1 |
| 20 | `Reverse` | u8 | 0 ou 1 |
| 21 | `Format` | u8 | 0 = AIFF, 1 = WAVE |
| 22 | `Channels` | u8 | 1 ou 2 |
| 23 | `TempoMode` | u8 | 0 = off, 1 = pattern, 2 = user |
| 24–27 | `OrigTempo` | u32 | BPM × 10 (ex. `0x4B0` = 1200 = 120 BPM) |
| 28–31 | `UserTempo` | u32 | BPM × 10 |

## Chunk WAV `RLND`

Chunk custom Roland ajouté aux fichiers `.WAV` exportés vers la carte, en plus des chunks
standards (`fmt `, `data`, ...).

- `ChunkID` : `"RLND"` (4 octets ASCII).
- `ChunkSize` : 4 octets.
- `Device` : identifiant de l'appareil, ex. `"roifspsx"` pour le SP-404SX (8 octets ASCII).
- Octets non identifiés (rôle exact à confirmer contre le code source `uttori-audio-wave`).
- `SampleIndex` : index de pad — 0 pour A1, incrémenté de 1 par pad, +12 par banque (donc J12
  = 119).
- Le chunk est paddé avec des zéros de façon à ce que les données audio (`data` chunk)
  démarrent exactement à l'offset 512 dans le fichier.

## Vérification effectuée (2026-08-08)

Sur le premier enregistrement (pad A1) d'une vraie carte SD SP-404SX (`SMPL/PAD_INFO.BIN`,
3840 octets) :

```
00 00 02 00  00 06 13 e0  00 00 02 00  00 06 13 e0
7f 00 01 00  00 01 02 00  00 00 04 28  00 00 04 28
```

décodé avec la table ci-dessus : `origSampleStart=512, origSampleEnd=399328,
userSampleStart=512, userSampleEnd=399328, volume=127, lofi=false, loop=true, gate=false,
reverse=false, format=Wave, channels=2, tempoMode=Off, origTempo=1064 (106.4 BPM),
userTempo=1064`. Toutes les valeurs sont cohérentes avec un vrai pad WAV stéréo en boucle —
la table d'offsets `PAD_INFO.BIN` est donc considérée fiable.

## Taux d'échantillonnage natif (vérifié 2026-08-09)

Lecture brute de l'en-tête `fmt ` de `SMPL/A0000001.WAV` sur la vraie carte : **44100 Hz,
stéréo, 16-bit** (`audioFormat=1` PCM). C'est la valeur utilisée comme cible de
ré-échantillonnage lors de l'import d'un sample externe sur un pad (voir
`plugin/SampleImport.h`) — un fichier importé à un autre taux (typiquement celui de la session
DAW, ou un mp3/flac/mp4 à un taux quelconque) est converti vers 44100 Hz avant d'être écrit sur
la carte, plutôt que d'être copié tel quel à un taux potentiellement différent du matériel réel.

## `PTN/PTNxxxxx.BIN` (patterns)

**Vérifié contre 3 vrais fichiers `PTN` d'une carte SD SP-404SX réelle** (2026-08-09,
`PTN00001.BIN`/`PTN00009.BIN`/`PTN00012.BIN`, montée en `/Volumes/SP-404SX`) en croisant leurs
octets avec la classe `AudioPattern` d'
[uttori-audio-padinfo](https://github.com/uttori/uttori-audio-padinfo/blob/master/src/audio-pattern.js)
(elle-même partiellement basée sur [spEdit404](https://github.com/bobgonzalez/spEdit404) et [la
doc de byteflip.club](http://byteflip.club/sp-edit/roland-sp404sx-ptn-format)). Implémenté dans
`core/include/sp404/Pattern.h` / `core/src/Pattern.cpp`, testé contre ces 3 mêmes fichiers dans
`core/tests/PatternTests.cpp` (octets bruts embarqués dans le test — ce ne sont que des
événements numériques (tick/note/vélocité), pas de contenu audio).

`AudioPattern` documente deux préréglages, "OG" (SP-404 original, 12 pads/banque) et "MKii" (16
pads/banque) ; le SP-404SX n'est ni l'un ni l'autre exactement — 12 pads/banque comme l'OG, mais
avec les octets de pied de fichier (footer) et la sémantique `bankSwitch` du MKii. Écart
documenté ci-dessous.

Structure : N enregistrements de 8 octets ("événements"), suivis d'un pied de fichier fixe de
16 octets. `N = (taille du fichier / 8) - 2` (les 2 "slots" en moins correspondant exactement
aux 16 octets du footer) — vérifié exact sur les 3 fichiers (232, 344 et 296 octets).

### Événement (8 octets)

| Offset | Champ | Type | Notes |
|---|---|---|---|
| 0 | `Ticks` | u8 | Délai depuis l'événement précédent, en ticks (96 PPQN → 384 ticks/mesure en 4/4) |
| 1 | `MidiNote` | u8 | 47–106 pour un vrai événement ; **128 = placeholder/silence** (bourrage utilisé quand l'écart avec l'événement suivant dépasse 255 ticks, ou pour compléter jusqu'à la mesure) |
| 2 | `BankSwitch` | u8 | `0` ou `64` = banques A–E ; `1` ou `65` = banques F–J (les deux orthographes vues sur de vrais patterns — voir plus bas) |
| 3 | `PitchMode` | u8 | Mode Step Sequencer uniquement ; `0` sur tous les événements réels observés (aucun pattern en mode séquenceur vérifié) |
| 4 | `Velocity` | u8 | 0–127 ; toujours `127` sur les événements réels observés |
| 5 | `Unknown` | u8 | Toujours `64` (`0x40`) sur les événements réels observés, `0` sur les placeholders |
| 6–7 | `LengthTicks` | u16 | Durée en ticks, **big-endian** (contrairement à ce que suggère `readUInt16(true)` dans `audio-pattern.js`, dont la signature exacte de `DataBuffer` n'a pas été vérifiée — l'interprétation big-endian est celle qui donne des valeurs cohérentes avec les écarts de `Ticks` observés) |

Adressage pad depuis `MidiNote`/`BankSwitch` (12 pads/banque, comme l'OG) :
`sampleNumber = MidiNote - 46` (1-based dans sa moitié), puis `+60` si `BankSwitch` indique la
seconde moitié (F–J). `A1` = `MidiNote=47, BankSwitch=0` ; `J12` = `MidiNote=106,
BankSwitch=65`. Vérifié : les 3 patterns réels retombent exactement sur les pads effectivement
présents sur la carte (banques C, D et I respectivement, qui contiennent bien des samples).

### Pied de fichier (16 octets)

| Offset | Valeur observée | Notes |
|---|---|---|
| 0 | `0` | constant sur les 3 fichiers |
| 1 | `140` (`0x8C`) | constant sur les 3 fichiers |
| 2–7 | `0` | constant sur les 3 fichiers |
| 8 | `0` | **contrairement à `audio-pattern.js`** (qui attend le nombre de mesures ici pour un MKii) — toujours `0` sur le SP-404SX |
| **9** | **nombre de mesures (entier)** | **découverte propre au SP-404SX**, absente de la doc `audio-pattern.js` : vérifié exact sur les 3 fichiers via `somme(Ticks de tous les événements, y compris placeholders) / 384 = footer[9]` (14, 4 et 14 respectivement — correspond exactement) |
| 10–11 | `0` | constant sur les 3 fichiers |
| 12 | signature rythmique | `0`=4/4, `1`=3/4, `2`=2/4, `3`=1/4, `4`=5/4, `5`=6/4, `7`=7/4 (valeur brute, `0` sur les 3 fichiers réels — non vérifié pour les autres valeurs) |
| 13–15 | `0` | constant sur les 3 fichiers |

### Ce qui reste ouvert

- **Un pattern ne référence-t-il que sa propre banque ?** Sur les 3 patterns réels vérifiés,
  chaque pattern référence des pads d'une seule et même banque (tous en C, tous en D, ou tous en
  I) — y compris quand `BankSwitch` alterne entre `0` et `64` pour le *même* pad au sein d'un
  même pattern (`PTN00001.BIN`, banque C, voit les deux). Ça suggère que `0`/`64` (ou `1`/`65`)
  ne distinguent pas deux banques différentes mais deux variantes d'encodage de la même moitié
  (peut-être liées à un second tap pour arrêter une note, comme le note `audio-pattern.js`).
  Non vérifié en revanche : un pattern peut-il mélanger plusieurs banques *différentes* au sein
  de la même moitié A–E (ou F–J) ? Aucun des 3 échantillons ne le fait, mais rien dans le format
  ne l'interdit explicitement. À revérifier si un contre-exemple apparaît sur une vraie carte.
- Signification exacte de `BankSwitch` en dehors des 4 valeurs vues (`0`, `1`, `64`, `65`) —
  aucune autre valeur observée sur les 3 fichiers réels.
- `PitchMode` en mode Step Sequencer (valeurs 129–152 documentées par `audio-pattern.js`) —
  aucun pattern en mode séquenceur vérifié ici, ce parseur les transmet tels quels sans les
  interpréter.
- Numérotation des fichiers `PTNxxxxx.BIN` eux-mêmes (quel pad/bank physique déclenche quel
  pattern) : toujours non vérifiée contre une vraie carte — voir Roadmap dans le README.
  Observation en passant : dans nos 3 échantillons, le numéro du fichier (1, 9, 12) ne
  correspond à *aucun* rapport évident avec la banque référencée par son contenu (C, D, I) —
  cohérent avec l'hypothèse que le slot de stockage du pattern et les pads qu'il joue sont deux
  espaces d'adressage indépendants.
- `SMPL/STPINFO.BIN` (124 octets sur la carte réelle) reste sans documentation trouvée et n'a
  pas été analysé.

## Points restant à vérifier

1. Chunk WAV `RLND` : toujours non vérifié contre un vrai fichier `.WAV` de la carte (les
   octets "unknown" restent des placeholders). À faire : lire le code source complet de
   `uttori-audio-wave` et/ou dumper les premiers octets d'un `.WAV` réel de `SMPL/`.
2. Voir la section "Ce qui reste ouvert" ci-dessus pour les inconnues restantes sur le format
   `PTN`.
