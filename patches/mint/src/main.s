.3ds

.open "code.bin", "build/patched_code.bin", 0x100000

; Redirect mint's hardcoded real ninja.ctr.shop.nintendo.net URLs to our
; nicoch.net equivalent (account-proxy already implements /ninja/ws/* per
; the nim shop patch). mint (the eShop-style purchase applet Badge Arcade
; launches for "buy plays") makes these calls DIRECTLY, not via nim, so
; the http:C generic nintendo.net->nicoch.net substring redirect never
; helped here even though it exists - confirmed 2026-09-17 via live GDB
; tracing that showed mint (not nim) issuing the ninja balance-check calls,
; with its own hardcoded real-Nintendo hostname, one copy per API endpoint
; string (44 occurrences found via byte search of a real dumped code.bin).
; "nintendo.net" (8) -> "nicoch.net" (6) is 2 bytes shorter each time; zero-padded.
;
; Offsets below were derived from a real EUR mint dump only, but are also
; applied to the USA/JPN title IDs in the top-level Makefile: mint ships
; localized text via a separate ROMFS partition (confirmed 2026-09-17 by
; inspecting a real dump's romfs contents), not embedded in .code, so the
; executable itself - including these plain API-URL strings - is expected
; to be byte-identical across regions. This is inferred, not verified
; against real USA/JPN dumps; if either region ever crashes specifically
; in mint after this patch, get a real dump for that region and re-derive
; offsets from it rather than assuming they still match.

.org 0x15e8d0
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x15ed30
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x15f454
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x15ff4c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1605d4
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x160d08
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1611d4
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x162690
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1e5bb0
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1e5f4c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1e63a8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1e77f8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1e7f98
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1e8b9c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1e9f68
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ea6c0
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1eac28
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1eb158
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ebd80
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ebfa8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ec544
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ed450
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ed84c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1eddd8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ee420
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1ee6d8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1eeab0
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1eedb8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1efdd4
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f14c8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f2b60
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f318c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f393c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f43f0
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f4cf0
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f53a4
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f5664
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f59ac
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f5f1c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f628c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f6804
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f6c3c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f6fd8
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.org 0x1f788c
	.asciiz "ninja.ctr.shop.nicoch.net"
	.byte 0, 0

.close
