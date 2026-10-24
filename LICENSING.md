# Licensing

This repo mixes code from several sources, so [LICENSE](LICENSE) (MIT) covers only some of it.
Where a file has an `SPDX-License-Identifier` header, that header decides.

| Part | License | Notes |
|---|---|---|
| `boards/shields/nice_view_custom/` (code), `scripts/`, `.github/`, `build.yaml`, and later `crates/` | MIT, [LICENSE](LICENSE) | Written for Foxglove. Parts of the shield started from ZMK's nice!view shield (MIT, The ZMK Contributors) |
| `boards/arm/eyelash_corne/` files with an SPDX MIT header | MIT | From the Eyelash Peripherals repo, which in turn builds on ZMK |
| Files from the Eyelash Peripherals repo without a header (the board's `.keymap`, `.yaml`, `.zmk.yml`; `config/eyelash_corne.json`) | not stated | Inherited from [a741725193/zmk-new_corne](https://github.com/a741725193/zmk-new_corne), which has no license file. They stay under their authors' terms |
| `config/eyelash_corne.keymap`, `keymap-drawer/` | mixed | The vendor's keymap (several contributors, no header), with Foxglove's changes on top. Those changes are MIT |
| `config/eyelash_corne.conf` | MIT | SPDX header |
| Right-hand art in `widgets/custom_art.c`, and pushed `.bin` sets | per artist | See [ART_CREDITS.md](boards/shields/nice_view_custom/ART_CREDITS.md). Some sets rest on the artist's consent rather than a license, so check before reusing them |
| `assets/plugins/` (plugin screen art) | © the repo owner, all rights reserved | The owner's Bandit art; see [docs/plugin-screens.md](docs/plugin-screens.md) |
| `app/resources/plugins/` (plugin icons, empty-state art) | © the repo owner, all rights reserved | The owner's Bandit art, like the right-hand Bandit scenes; see [docs/plugin-icons.md](docs/plugin-icons.md) |
| `app/` | GPL-3.0-only, [app/LICENSE](app/LICENSE) | The Foxglove app: [KeyPeek](https://github.com/srwi/keypeek) by Stephan Rumswinkel, absorbed with its history (via BanditF/keypeek) and renamed. Our changes to it are GPL-3.0 too |
| `docs/upstream/` | not stated | The vendor's original README |

**How the parts combine:** firmware builds use only MIT code and the art. The desktop app is
`app/` (GPL-3.0) plus MIT crates from `crates/`. MIT code can be used in a GPL-3.0 program,
and the app binary as a whole is then distributed under GPL-3.0. The crates keep their MIT
license when used on their own.
