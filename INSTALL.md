# Revivetendo 3DS Setup Guide

Covers both a real 3DS/2DS and the Azahar emulator (Android or desktop). Both use the
same Nimbus fork; Azahar additionally needs one extra file since it doesn't do the
same DNS-level redirection real hardware relies on.

## Prerequisites

- A homebrew-capable 3DS (real hardware) **or** [Azahar](https://github.com/azahar-emu/azahar)
- A CIA installer on real hardware if you don't already have one — [FBI](https://github.com/Steveice10/FBI)
  is the standard choice
- The latest **Revivetendo 3DS Patches** release (`sdfiles.tar.gz`) from
  [nimbus-nico_christmann releases](https://github.com/Happynico7504/nimbus-nico_christmann/releases) —
  this now bundles our own pre-built `nimbus.cia` alongside the patches, so you no longer
  need to separately fetch anything from PretendoNetwork's upstream Nimbus releases. Our
  fork's app has its own fixes (correctly installing every patch this fork adds) that
  the stock upstream app doesn't have, so use this one, not upstream's.
- For Azahar only: the **Azahar URL Redirection File** release (`http_hle_replace_rules.txt`)
  from the same releases page

## Real Hardware

1. Download and extract `sdfiles.tar.gz` from the latest release. It contains an
   `sdfiles` folder with everything needed, laid out exactly as it should sit on your
   SD card.
2. Copy everything **inside** `sdfiles` (the `3ds` and `cias` folders) to the **root**
   of your SD card, merging with whatever's already there.
3. Using FBI (or your CIA installer of choice), install `sd:/cias/nimbus.cia`.
4. Open Nimbus from the Home Menu. When it shows "nimbus has updated," press Start and
   wait for the console to reboot.
5. Open Miiverse to test. If it loads successfully, you're good to go.

That's it — real hardware picks up the redirects automatically once Nimbus is patched.

## Azahar (Emulator)

Azahar's emulated SD card is the `sdmc` folder inside your chosen Azahar storage
folder. Wherever real hardware instructions say `sd:/...`, on Azahar that's
`<your Azahar storage folder>/sdmc/...` instead.

1. Download and extract `sdfiles.tar.gz` from the latest release, same as above.
2. Copy everything inside the `sdfiles` folder (the `3ds` and `cias` folders) into
   `<your Azahar storage folder>/sdmc/`, merging with whatever's already there.
3. In Azahar, install `sdmc/cias/nimbus.cia` the same way you would any CIA (File →
   Install CIA).
4. Enable the **"Enable required LLE modules to use online services"** option
   (Emulation → Configure → System) — without it, online features won't work at all
   regardless of any patches.
5. Boot the Home Menu, run Nimbus, and let it apply the patches (same as real
   hardware above).
6. Download `http_hle_replace_rules.txt` from the **Azahar URL Redirection File**
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
