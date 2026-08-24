# Revivetendo 3DS Setup Guide

Covers both a real 3DS/2DS and the Azahar emulator (Android or desktop). Both use the
same Nimbus fork; Azahar additionally needs one extra file since it doesn't do the
same DNS-level redirection real hardware relies on.

## Prerequisites

- A homebrew-capable 3DS (real hardware) **or** [Azahar](https://github.com/azahar-emu/azahar)
- `nimbus.cia` from [PretendoNetwork's own Nimbus releases](https://github.com/PretendoNetwork/nimbus/releases) —
  **unmodified**, get it from upstream, not from this repo
- The latest **Revivetendo 3DS Patches** release (the patch folder only) from
  [nimbus-nico_christmann releases](https://github.com/Happynico7504/nimbus-nico_christmann/releases)
- For Azahar only: the **Azahar URL Redirection File** release (`http_hle_replace_rules.txt`)
  from the same releases page

## Real Hardware

1. Extract the update folder from the latest **Revivetendo 3DS Patches** release.
2. Drop it into `sd:/3ds/nimbus`.
3. Open Nimbus. When it shows "nimbus has updated," press Start and wait for the
   console to reboot.
4. Open Miiverse to test. If it loads successfully, you're good to go.

That's it — real hardware picks up the redirects automatically once Nimbus is patched.

## Azahar (Emulator)

1. Install the unmodified `nimbus.cia` (from PretendoNetwork's own releases, same
   file as above) in Azahar the same way you would on real hardware (File →
   Install CIA).
2. In Azahar, enable the **"Enable required LLE modules to use online services"**
   option (Emulation → Configure → System) — without it, online features won't work
   at all regardless of any patches.
3. Boot the Home Menu, run Nimbus, and let it apply the patches (same as real
   hardware above).
4. Download `http_hle_replace_rules.txt` from the **Azahar URL Redirection File**
   release and place it at:
   ```
   <your Azahar storage folder>/sysdata/http_hle_replace_rules.txt
   ```
   No emulator setting needs to be toggled — Azahar loads this file automatically on
   next launch. (If you ever edit this file yourself: it must be *exactly* alternating
   pattern/replacement lines with **no blank lines between rules** — Azahar's parser
   reads two lines at a time with no separator, and a stray blank line desyncs every
   rule after it.)
5. Launch Miiverse to test.

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
our own servers — Miiverse, BOSS/SpotPass, conntest, account login, and Swap Doodle's
NASC login + HPP relay. 3DS Friends/NASC for *other* titles is deliberately left
pointed at real Pretendo Network, untouched.
