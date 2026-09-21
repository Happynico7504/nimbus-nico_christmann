# Nimbus
## Pretendo account manager for the 3DS

## Usage

1. Grab the latest app and IPS patches from the [Releases](https://github.com/PretendoNetwork/nimbus/releases) page
2. Extract to the root of your 3DS SD card
3. Install the Nimbus homebrew using FBI (or FBI Reloaded) if using the CIA build
4. Run the Nimbus homebrew and select either to use a Pretendo or Nintendo account
     - If it doesn't work, reboot your 3DS while holding SELECT and ensure that "Enable loading external FIRMs and modules" and "Enable game patching" are both turned on, as well as ensuring that your Luma3DS version is 13.0 or higher.
5. Enable the Nimbus plugin by entering into the Rosalina menu and setting the "Plugin Loader" to "Enabled"

## Building

1. Clone the repository recursively using `git clone https://github.com/PretendoNetwork/nimbus --recursive`
    - If you have cloned the repository previously, please run `git pull` and `make clean` while in the nimbus folder to avoid errors and broken files
    - On top of that, if you cloned it before 1.0.2 released, you might also need to run `git submodule update --init --recursive` while in the nimbus folder
2. Install devkitARM, libctru 2.5.0 or later, `3ds-curl` and `3ds-mbedtls` (the app bundles its own TLS; `app/data/cacert.bin` is the Mozilla CA bundle from curl.se), [CTRPluginFramework](https://gitlab.com/thepixellizeross/ctrpluginframework), [3gxtool](https://gitlab.com/thepixellizeross/3gxtool), [armips](https://github.com/Kingcom/armips), [makerom](https://github.com/3DSGuy/Project_CTR), [bannertool](https://github.com/Steveice10/bannertool) and [flips](https://github.com/Alcaro/Flips)
3. Copy [decompressed `code.bin`](https://github.com/PretendoNetwork/nimbus/blob/main/DECOMPRESSING.md) files from the act, friends, http, miiverse, socket, ssl, nim and mint modules in their respective `patches` directories (any Miiverse code.bin works for the miiverse module; `mint` uses the EUR applet). The dumps are never committed.
4. Run `make patches` (builds `out/nimbus-patches.zip`) and/or `make app` (builds `out/nimbus.cia`; `make APP_VERSION=2.3.1 app` stamps the version). `patches/ssl` is a git submodule (`git submodule update --init`).

## Releases and CI

Two independent releases, both built by GitHub Actions when you push a tag:

- `patches-vMAJOR.MINOR.MICRO` builds and publishes `nimbus-patches.zip` (workflow `patches.yml`). It reads the module dumps from a **private** repository: set the repository variable `DUMPS_REPO` (e.g. `Happynico7504/nimbus-dumps`) and the secret `DUMPS_DEPLOY_KEY` (the private half of a read-only deploy key added to that repo; it can read only that repository). The dumps sit at the repo root as `<TitleID>.dec.code`; see `.github/scripts/place-dumps.sh` for the exact names.
- `app-vMAJOR.MINOR.MICRO` builds and publishes `nimbus.cia` (workflow `app.yml`); the app version is taken from the tag. Only a CIA is shipped.
- `toolchain-cache.yml` keeps the compiled tools (makerom, bannertool, armips, flips) in the Actions cache: it runs weekly to keep the cache alive and rebuilds once per month.

The app checks these tags itself: it installs the newest `patches-v*` (using its saved copy if that is already current) and can update itself from the newest `app-v*`.

## Tests

`make -C app/tests test DATA=<dir>` runs the portable logic (archives, release parsing, cache and update flow, installer, service rules) on the host. Put `3dsx.*.zip`, `sdfiles.tar.gz`, `latest.json` and `nimbus-patches.zip` in `<dir>` to also test against real archives.

## Credits

Thanks to:

- [pinklimes](https://github.com/gitlimes) for the CIA version banner
- [TraceEntertains](https://github.com/TraceEntertains) for making a CIA version of Nimbus and maintaining the project
- [DaniElectra](https://github.com/DaniElectra) for making the 3DS HTTP and Socket patches and maintaining the project
- [SciresM](https://github.com/SciresM) for making the 3DS SSL patches
- [zaksabeast](https://github.com/zaksabeast) for the original 3ds-Friend-Account-Manager and all the research into the friends and act system titles
- [shutterbug2000](https://github.com/shutterbug2000) for the GUI
- [libctru](https://github.com/devkitPro/libctru) for the `frda.c` base, homebrew template, and other library functions (and thanks to citro2d for part of a system font function)
- [Universal-Core](https://github.com/Universal-Team/Universal-Core) for the string drawing functions
- [Fangal-Airbag](https://github.com/Fangal-Airbag) for making the account switcher GUI support button controls
- All other 3DS researchers
