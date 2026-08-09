# Format de la carte SD Roland SP-404SX

Statut : **partiellement vérifié contre une vraie carte SD SP-404SX** (2026-08-08). L'emplacement
du fichier d'index a été corrigé suite à cette vérification (voir plus bas) ; la table d'octets
de `PAD_INFO.BIN` a été confirmée valeur par valeur sur un vrai enregistrement de pad. Le chunk
WAV `RLND` reste non vérifié (voir "Points à vérifier").

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

## Points restant à vérifier

1. Chunk WAV `RLND` : toujours non vérifié contre un vrai fichier `.WAV` de la carte (les
   octets "unknown" restent des placeholders). À faire : lire le code source complet de
   `uttori-audio-wave` et/ou dumper les premiers octets d'un `.WAV` réel de `SMPL/`.
2. Le fichier `PTN/` (patterns) et `STPINFO.BIN` observés sur la vraie carte ne sont pas encore
   documentés ici — hors scope tant que le triggering de patterns n'est pas implémenté.
