.3ds

.open "code.bin", "build/patched_code.bin", 0x100000

; Static string patches only - no hooks needed here, unlike the other
; modules in this fork. These are plain read-only C-string constants nim
; references directly by pointer (confirmed via `strings -t x` on a real
; dumped code.bin, 2026-09-16), not a runtime-constructed buffer like
; http:C's shared hostname path - so a direct byte-for-byte overwrite of
; the literal is sufficient and there's no separate call site to hook.
;
; Every replacement keeps the same "<subdomain>.nintendo(wifi).net" ->
; "<subdomain>.nicoch.net" substitution already used everywhere else in
; this fork (same CNAME to netcup-server.nicochristmann.net, same wildcard
; cert, same nginx SNI routing to account-proxy already in place - no new
; DNS/cert/routing work needed for these hosts to reach account-proxy).
;
; Each replacement is strictly SHORTER than the original and the leftover
; space is explicitly zero-padded out to the exact original length, so
; nothing after these strings in the binary shifts - same safety
; constraint documented in http/src/main.s (a same-or-longer replacement
; there caused a real ARM11 data abort on real hardware once already).
;
; Account-proxy does not yet have handlers for ecs.c.shop.nicoch.net /
; ninja.ctr.shop.nicoch.net / kagiya-*.cdn.nicoch.net - this patch only
; gets the traffic reaching our server, it does not implement the actual
; SOAP purchase-completion protocol. Capture real requests once this is
; deployed before building that.
;
; Addresses below are file offsets plus the 0x100000 base from .open above
; (same convention as every other patch in this fork, e.g. http/src/main.s's
; replace_hook_addr equ 0x113868 = file offset 0x13868 + 0x100000) - NOT raw
; file offsets. They're only valid for the exact nim version this was
; derived from - re-verify with `strings -t x` against your own dump (and
; add 0x100000 to whatever offset it reports) if these don't match (nim
; gets version-bumped by system updates like everything else).

.org 0x113f6c
	.asciiz "kagiya-ctr.cdn.nicoch.net"
	.byte 0, 0

.org 0x113fb8
	.asciiz "kagiya-dev-ctr.cdn.nicoch.net"
	.byte 0, 0

; ECommerceSOAP - the actual purchase-completion endpoint (SOAP-based,
; ecs:ServiceTicket/ecs:SessionHandle envelope). Two identical occurrences
; in the binary (likely two separate call sites) - both patched.
.org 0x1511bc
	.asciiz "https://ecs.c.shop.nicoch.net/ecs/services/ECommerceSOAP"
	.byte 0, 0, 0, 0, 0, 0

; Wallet/points balance check - likely called before ECS during a
; purchase attempt.
.org 0x1514b0
	.asciiz "https://ninja.ctr.shop.nicoch.net/ninja/ws/my/balance/current_raw"
	.byte 0, 0

; NetUpdateSOAP (title updates) - probably unrelated to the "buy plays"
; flow specifically, patched anyway since it's free/safe.
.org 0x1514f4
	.asciiz "https://nus.c.shop.nicoch.net/nus/services/NetUpdateSOAP"
	.byte 0, 0, 0, 0, 0, 0

.org 0x151533
	.asciiz "https://ecs.c.shop.nicoch.net/ecs/services/ECommerceSOAP"
	.byte 0, 0, 0, 0, 0, 0

.close
