.3ds

.open "code.bin", "build/patched_code.bin", 0x100000

; NS only launches an installed update title (0004000E + unique id) for a base
; whose unique id is in 0x300-0xF7FFF or 0xFF000-0xFF3FF, plus five hardcoded
; system titles (FUN_00111454, "does this title have a usable update?"). The
; homebrew range 0xF8000-0xFEFFF is always excluded, so Revivetendo eShop apps
; there installed their updates but kept running the base code (2026-10-13).
;
; Skip that unique-id range test: jump straight from the unique id computation
; to building the update program info. NS still requires a normal application
; (programId high 0x00040000, variation 0) and an installed update whose title
; version is higher than the base's - the checks that follow at 0x1114cc.
;
; Address is the file offset + 0x100000, derived from a dumped NS
; (0004013000008002) of current firmware. Re-check against your own dump if it
; doesn't match:
;   00111484  cmp r0,#0x300        <- replaced
;   ...
;   001114cc  cpy r1,r4            <- target (FUN_00111368 call follows)

.org 0x111484
	b 0x1114cc

.close
