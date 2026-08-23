; NASC_URL needs to be provided at build time
; Points at our own NASC-compatible endpoint instead of Pretendo's, so
; NASC-discovered titles (e.g. Swapdoodle's HPP client) route through our
; own server-side handling instead of failing on titles Pretendo doesn't
; support. Unrecognized game_server_id/titleid lookups fall through to a
; transparent proxy to the real nasc.pretendo.cc, so 3DS Friends and
; anything else already working keeps working unchanged.
.org 0x16129a
  .area 38
    .asciiz "https://nasc.nicochristmann.net/ac/"
  .endarea
