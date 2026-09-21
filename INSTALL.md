# Revivetendo 3DS Setup Guide

Covers both a real 3DS/2DS and the Azahar emulator (Android or desktop). Both use the
same Nimbus fork; Azahar additionally needs one extra file since it doesn't do the
same DNS-level redirection real hardware relies on.

## Prerequisites

- A homebrew-capable 3DS (real hardware) **or** [Azahar](https://github.com/azahar-emu/azahar)
- A CIA installer on real hardware if you don't already have one — [FBI](https://github.com/Steveice10/FBI)
  is the standard choice
- The latest **Nimbus app** release (`nimbus.cia`, tag `app-v*`) from
  [nimbus-nico_christmann releases](https://github.com/Happynico7504/nimbus-nico_christmann/releases).
  Use this one rather than upstream's: the patches are no longer copied to your SD card
  by hand — the app downloads them itself (see below).
- A working internet connection on the console (the app downloads from GitHub)
- For Azahar only: the **Azahar URL Redirection File** release (`http_hle_replace_rules.txt`)
  from the same releases page

There are two kinds of releases in the repository:

| Tag | Contents | Who uses it |
|---|---|---|
| `app-v*` | `nimbus.cia`, the updater app (CIA only) | you, once — the app updates itself afterwards |
| `patches-v*` | `nimbus-patches.zip`, our patch set | downloaded by the app; you never need to fetch it |

## Real Hardware

1. Download `nimbus.cia` from the latest `app-v*` release and put it on your SD card.
2. Using FBI (or your CIA installer of choice), install it.
3. Open Nimbus from the Home Menu and press **SELECT** to open the **Patch services** page.
4. Set the **Network** row to **Ours** (Revivetendo) — press A on the row to change it —
   then press **X** to apply. Nimbus downloads the newest patches, installs them and asks
   you to press Start to reboot.
5. Open Miiverse to test. If it loads successfully, you're good to go.

Real hardware picks up the redirects automatically once Nimbus is patched.

### The Patch services page

One page lists every service — Account, Friends, HTTP redirects, Socket, SSL, Miiverse,
eShop / Badge Arcade and the Plugin — with the provider on the right of each row.

- **Network**: *Pretendo* (the base) or *Ours*. Our network is a full replacement (it
  routes what we host to us and forwards everything else to Pretendo), so choosing it
  sets every service and cannot be mixed with anything else.
- **On Pretendo**, a service can take an overlay that ships it. *Roseverse* replaces
  Account, HTTP and Miiverse together (its own patcher always installs them as one set);
  the eShop row shows "Ours only" because only our network provides it.
- **X** applies the page: for each provider in use, Nimbus checks whether a newer tag
  exists, uses its saved copy if not, downloads it otherwise, and installs the result.
  Anything you did not select is removed.
- The first time you use a third-party source (Pretendo, Roseverse) Nimbus asks you to
  trust it — those files patch system modules, and Nimbus cannot verify them.
- The bottom row updates the Nimbus app itself from the newest `app-v*` release.

Patches are no longer read from `/3ds/nimbus/update` on the SD card.

## Azahar (Emulator)

Azahar's emulated SD card is the `sdmc` folder inside your chosen Azahar storage
folder. Wherever real hardware instructions say `sd:/...`, on Azahar that's
`<your Azahar storage folder>/sdmc/...` instead.

1. Download `nimbus.cia` from the latest `app-v*` release.
2. In Azahar, install it the same way you would any CIA (File → Install CIA).
3. Enable the **"Enable required LLE modules to use online services"** option
   (Emulation → Configure → System) — without it, online features won't work at all
   regardless of any patches.
4. Boot the Home Menu, run Nimbus, press SELECT, choose the network and apply the
   patches (same as real hardware above).
5. Download `http_hle_replace_rules.txt` from the **Azahar URL Redirection File**
   release and place it at:
   ```
   <your Azahar storage folder>/sysdata/http_hle_replace_rules.txt
   ```
   No emulator setting needs to be toggled — Azahar loads this file automatically on
   next launch. (If you ever edit this file yourself: it must be *exactly* alternating
   pattern/replacement lines with **no blank lines between rules** — Azahar's parser
   reads two lines at a time with no separator, and a stray blank line desyncs every
   rule after it.)
7. Launch Miiverse to test.

### Verifying it worked

Check Azahar's log (Emulation menu, or the log file next to `sysdata`) for a line
like:

```
Service.HTTP <Warning> ... Apply:2258: rule "<pattern>" has replaced URL "<original>" to "<new>"
```

If you see that with a sane single-hostname replacement, the redirect is live. If
Miiverse still fails after that, the next thing to check is real cert validation —
`ssl:C` runs as real (LLE) code on Azahar by default, so it validates certificates
the same way real hardware does.

## What's Actually Redirected

Both paths ultimately redirect the same set of real Nintendo/Pretendo hostnames to
our own servers — Miiverse, BOSS/SpotPass, conntest, account login, Swap Doodle's
NASC login + HPP relay, and Nintendo Badge Arcade's eShop-style "buy plays" flow
(nim's NUS/ECS SOAP calls and the shop applet's ninja balance-check, both patched
to point at our own shop backend instead of Nintendo's real, long-dead servers).
3DS Friends/NASC for *other* titles is deliberately left pointed at real Pretendo
Network, untouched.
