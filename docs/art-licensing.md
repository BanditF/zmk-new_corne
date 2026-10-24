# Art licensing register

Every piece of screen art we hold or ship, where it came from, its licence, the credit it needs
and whether it may go into this public repo or into builds.

**The rule:** only art with a confirmed licence that allows redistribution goes into this repo or
into firmware and app builds. Credit it in
`boards/shields/nice_view_custom/ART_CREDITS.md` and in the app's about/credits when the app ships it.
Anything marked UNCLEAR or DO NOT SHIP stays out until its flag below is resolved. This is a
working register, not legal advice.

Local sources live in `C:\Users\fuchs\splitkeeb\art-src` (not in any repo) unless noted.

| Source | Where | Origin | Licence | Attribution | Status | Notes |
|---|---|---|---|---|---|---|
| ZMK nice!view balloon and mountain (art by Hammerbeam) | firmware built-ins `ZMK_1`, `ZMK_2` | [zmkfirmware/zmk `app/boards/shields/nice_view/widgets/art.c`](https://github.com/zmkfirmware/zmk/blob/v0.3.0/app/boards/shields/nice_view/widgets/art.c) | MIT (ZMK) | ZMK contributors; credit Hammerbeam | **OK to ship** (shipping) | Part of ZMK under its MIT licence. |
| Corro the urchin animation | firmware built-in `Corro_urchin`; `untrusted/GPeye_urchin-peripheral-animation` | [GPeye/urchin-peripheral-animation](https://github.com/GPeye/urchin-peripheral-animation) | MIT (repo) | GPeye | **OK with conditions** (shipping) | The animation is GPeye's own pixel work under MIT. Corro is the unofficial Rust "unsafe" mascot, a community character with no stated licence. See flag F4. |
| Gem crystal (art by Trixelized) | firmware built-in `Gem_crystal`; `untrusted/M165437_nice-view-gem` | [M165437/nice-view-gem](https://github.com/M165437/nice-view-gem) | MIT (repo) | M165437; credit Trixelized | **Not shipped** (replaced by our own Bandit_Gem, #110) | The README says the crystal art was "lent" to the project by Trixelized. It is distributed inside the MIT repo, but there's no separate grant from the artist. See flag F3. |
| Hammerbeam slideshow (30 signed pieces) | `untrusted/GPeye_hammerbeam-slideshow` | [GPeye/hammerbeam-slideshow](https://github.com/GPeye/hammerbeam-slideshow) | MIT (repo); art used with the artist's consent | Hammerbeam (signature in each image must stay) | **DO NOT SHIP** (removed 2026-10-21) | Removed by the owner's choice. The signature is the attribution, so the art may only be used as-is, never edited. |
| Flipper Zero dolphin animations | `official/` (folders like `L1_Doom_128x64`); `flipper/` (PNG stills of the same) | [flipperdevices/flipperzero-firmware `assets/dolphin/external`](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/assets/dolphin/external) | GPL-3.0 | Flipper Devices | **OK with conditions** (not shipping today) | Copyleft: see the notes below. They were removed from firmware because 128x64 art reads sideways on the portrait screens. |
| kainsamara animations (Garage, Soulout) | `licensed/kainsamara` | [kainsamara/Flipper-Zero-Animations](https://github.com/kainsamara/Flipper-Zero-Animations) | MPL-2.0 | kainsamara | **OK with conditions** (not shipping) | File-level copyleft: see the notes below. |
| PORKCHOP asset pack | `licensed/porkchop` | [pfefferle/PORKCHOP-flipper-asset-pack](https://github.com/pfefferle/PORKCHOP-flipper-asset-pack) | GPL-3.0 | pfefferle | **OK with conditions** (not shipping) | Copyleft: see the notes below. |
| "bip3r" animations (happy, happy2, herd, roll, wave) | `licensed/bip3r` | unknown | unknown | unknown | **UNCLEAR / DO NOT SHIP** (kept locally) | Filed under `licensed/` but no source or licence was recorded, and none was found online. See flag F1. |
| Community test animations (80sCar, KofH) | `community-test/` | unknown | unknown | unknown | **UNCLEAR / DO NOT SHIP** (kept locally) | Test downloads with no recorded source. Large community packs such as [Kf637/Animations-for-Flipper-Zero](https://github.com/Kf637/Animations-for-Flipper-Zero) mix many creators and licences. See flag F2. |
| nice-view-elemental | `untrusted/kevinpastor_nice-view-elemental` | [kevinpastor/nice-view-elemental](https://github.com/kevinpastor/nice-view-elemental) | MIT | Kevin Pastor | OK to ship (not used) | Background images and fonts. Check the font licences before reusing those (the fonts are bundled from elsewhere). |
| zmk-nice-oled: Pokémon art | `untrusted/mctechnology17_zmk-nice-oled` (`assets/pokemon*.c`) | [mctechnology17/zmk-nice-oled](https://github.com/mctechnology17/zmk-nice-oled) | Repo MIT, but the characters are Nintendo / Game Freak IP | n/a | **DO NOT SHIP** | A repo's MIT licence can't license someone else's trademarked characters. |
| zmk-nice-oled: other art (spaceman, samurai, head, crystal, luna) | same | same | MIT (repo); origin of each image not stated | mctechnology17 | **UNCLEAR** (not used) | Each piece's origin would need checking. The "crystal" looks like nice-view-gem's. Luna is the QMK "Luna" pet, whose origin is also unstated. |
| Bandit fox motif and the Bandit HUD library | `art-src/bandit`; `BanditF/bandit-wallpapers` `library/bandit` | the repo owner | © the owner, all rights reserved | the owner | **OK to ship** (owner's; the 13 scenes are firmware built-ins since 2026-10-23) | See F5. |
| FURY linocut prints | `BanditF/bandit-wallpapers` `input_media/FURY prints` (private repo) | the repo owner | © the owner, all rights reserved | the owner | **OK to ship** (owner's) | Traced vector and keyboard derivatives live in the private wallpaper repo. See flag F5. |
| Wallpaper keyboard art (12 sets) | `BanditF/bandit-wallpapers` → `dist/keyboard/<name>/right.bin` | generated from the two sources above | owner's own work | the owner | **OK to ship** (owner's) | Mostly the owner's art, plus procedural scenes made in this project. |
| Plugin icons and the Plugins-tab empty state | `app/resources/plugins/` (generator `scripts/plugin_icons.py`) | drawn in this project from the Bandit fox motif | © the owner, all rights reserved | the owner | **OK to ship** (owner's) | Generic glyphs, no vendor logos: see [plugin-icons.md](plugin-icons.md) and F5. |
| Plugin screen assets (Pomodoro, mic-mute panels; Bandit_Focus right art) | `assets/plugins/` (generator `scripts/plugin_panels.py`; right art from `art-src/bandit`) | drawn in this project in the Bandit style | © the owner, all rights reserved | the owner | **OK to ship** (owner's) | Not covered by the plugin crates' MIT licence: see [plugin-screens.md](plugin-screens.md) and F5. |

## Flags

- **F1, bip3r (UNCLEAR / DO NOT SHIP):** no known source or licence. Owner decision (2026-10-23):
  keep them in art-src, but never ship them or copy them into a repo.
- **F2, community-test (UNCLEAR / DO NOT SHIP):** same as F1 and the same decision.
- **F3, Gem crystal (resolved):** the art was "lent" by Trixelized inside an MIT repo. It was replaced
  by our own `Bandit_Gem` at the same index (#110), so it no longer ships.
- **F4, Corro (shipping, OK with conditions):** GPeye's animation is MIT, but Corro is a community
  mascot character. Low risk; same remedy as F3 if we want a fully clean set.
- **F5, the owner's own art (decided):** the Bandit and FURY art is **all rights reserved**
  (owner decision, 2026-10-23). It ships in our builds, but nobody else gets a licence to reuse it.
  That includes the Bandit scenes in `custom_art.c`, which this repo's code licences don't cover.
- **F6, the Pokémon art (DO NOT SHIP):** never copy it into a repo or a build.

## Copyleft notes

- **GPL-3.0 art** (the Flipper dolphins, PORKCHOP): GPL-3.0 can cover art as well as code. Art
  shipped under GPL-3.0 makes the work it's compiled into (`custom_art.c`, then the firmware
  binary) a GPL-3.0 combination, so anyone receiving that firmware is entitled to the
  corresponding source. Foxglove's firmware is public source, so that's met as long as the
  repo's licensing is stated per directory and the art keeps its notices. Keep GPL art out of the
  MIT `crates/digitalis` and out of anything we'd want to stay permissive.
- **MPL-2.0 art** (kainsamara): file-level copyleft. The MPL files themselves stay MPL and must be
  made available, unmodified or with modifications shared. Other files in the same build are not
  affected.
- **Pushed art (NVA1 `.bin` from the app's library):** distributing a blob is distributing the art,
  so the same licence and credit rules apply to anything the app bundles or offers for download.
