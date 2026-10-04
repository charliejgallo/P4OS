# The portal's security

The web portal can do anything the board can: read and write every file on
the card, see and touch the screen, change the Wi-Fi, install firmware. Until
2026-10-04 it trusted whoever reached it. This is what guards it now, and
what it does not guard against.

## Other sites in your browser

Two attacks need nothing but your browser on the same network as the board,
and both are closed (`components/aos_portal/aos_portal_access.c`):

- **Requests from another page.** A page from anywhere can make your browser
  send a "simple" request (a form, or a `fetch` with a `text/plain` body) to
  `http://p4os.local/api/...` with no preflight. The API now answers only
  what the browser marks as coming from the portal's own pages
  (`Sec-Fetch-Site`, `Origin`). curl and the scripts in `tools/` send
  neither header and are not affected.
- **DNS rebinding.** A name of the attacker's that first points to their
  server and then to the board lets their page read the board's answers.
  The portal answers only to its IP or its own name (bare, `.local`, `.lan`,
  `.home`, `.home.arpa`, `.localdomain`); anything else gets 421.

Every answer also carries `X-Content-Type-Options: nosniff` and
`X-Frame-Options: DENY`.

## Where a request comes from

The rules are set on the board, in **Settings, Portal web**, and only there:
someone who got into the portal cannot open it further. What a request needs
depends on where it came in (`components/aos_ui/aos_access.c`):

| Way in | Needs |
|---|---|
| The USB cable (`192.168.7.1`) | nothing: whoever holds the cable holds the board |
| The board's own network (`192.168.4.1`) | nothing, or the password if "at home too" is chosen. It has its own WPA2 password |
| A trusted Wi-Fi network | the same |
| Any other Wi-Fi network | closed, or the password if chosen so |

- **Trusted networks** are marked by name in Settings, Portal web. When the
  rules first appear (the first boot with this firmware), the network the
  board is on becomes trusted, so an update does not close the portal on its
  owner.
- **On an untrusted network the board also stays quiet on mDNS:** it does
  not announce `p4os.local` there, and the Red app's mDNS tab finds nothing
  on that network either. It is applied again on every connection, because
  mDNS turns itself back on when an address comes; for that second the name
  may be heard.
- **The password** is set on the board, typed twice, six characters or more.
  It is kept as PBKDF2-HMAC-SHA256 (10 000 rounds, a random salt). After
  three wrong ones the portal waits 2, 4, 8 seconds and so on, up to five
  minutes. A new password closes every session.
- **Sessions.** `POST /api/login` answers with a cookie (`HttpOnly`,
  `SameSite=Strict`) that the page's scripts cannot read and no other site
  sends. The board keeps a hash of its 16 random bytes, never the id. There
  are eight at most; Settings closes all of them.
- **The token** is for scripts: `Authorization: Bearer <token>`. Settings
  shows it and makes a new one. Every script in `tools/` that talks to the
  board sends it when it is in `P4OS_TOKEN`:

      P4OS_TOKEN=<token> tools/ota.sh p4os.local

The API for it:

    GET  /api/auth      the zone the request came in on, what it needs, whether it has it
    POST /api/login     {"password": "..."}: the session cookie
    POST /api/logout    ends this browser's session

## HTTPS

Settings, Portal web, **HTTPS (port 443)**, and a restart. The portal then
answers on 443 too (`aos_httpd_start_tls` in
`components/aos_portal/aos_httpd.c`), and on an untrusted network plain
HTTP only sends the browser there (302), so neither the password nor the
session crosses that network in clear. At home, on the board's own network
and over the cable, HTTP keeps working: the scripts in `tools/` use it.

- **The board has its own authority.** The first time, it makes a small
  certificate authority (ECDSA P-256, ten years) and, signed by it, the
  portal's certificate: `<name>.local`, the bare name, `192.168.4.1`,
  `192.168.7.1` and the Wi-Fi address, `serverAuth`, 800 days. The board
  makes the portal's certificate again by itself when it has 60 days left
  or when its name or its Wi-Fi address changed; the authority stays.
  Everything is kept in preferences, as DER.
- **Trusting it once** makes the warning go. The portal hands out the
  authority at `/api/tls/ca` (Settings, Security, in the portal; no login
  needed, it is public):
  - **Mac:** open the `.cer`, it goes into Keychain Access; open it there,
    Trust, "When using this certificate: Always Trust". Safari and Chrome
    take it.
  - **iPhone:** open the link in Safari, allow the profile, install it in
    Settings, General, VPN & Device Management; then Settings, General,
    About, Certificate Trust Settings, and switch it on.
  - Compare its SHA-256 fingerprint with the one Settings, Portal web shows
    on the board.
- **The authority can only vouch for the board's names.** Its key is on the
  board, in flash that is not encrypted, so it carries a critical
  `nameConstraints` extension: `.local` names, the board's own name and the
  private ranges (10/8, 172.16/12, 192.168/16). Someone who took the key
  from a board could not use it to pass for any site on the internet to a
  device that trusts it. (mbedTLS does not parse that extension, so the
  board reads its own authority with a callback that accepts it; browsers
  do apply it.)
- **What Apple asks of a server certificate a user trusts**, and Chrome on
  a Mac uses Apple's verifier: the name in `subjectAltName`, `serverAuth`
  in `extendedKeyUsage`, at most 825 days. The first certificate (before
  2026-10-04's second version) was a single self-signed one with none of
  this, which no Mac or iPhone would trust.
- **TLS 1.2**, ECDHE with the board's ECDSA key, AES-GCM first (the P4 does
  AES in hardware), ChaCha20-Poly1305 last. Each connection's buffers (16 KB
  in, 4 KB out) are in PSRAM, as mbedTLS allocates there on this board.
- The session cookie carries `Secure` when it was set over HTTPS.
- "Autoridad nueva" in Settings throws both away: the next start makes new
  ones, and they have to be trusted again.

## What is not covered

- **HTTPS is off until it is turned on**, and plain HTTP stays open at home,
  on the board's network and over the cable.
- **The board's flash is not encrypted**, so the Wi-Fi passwords, the
  portal's password hash and the token can be read from a board in hand.
  ESP-IDF's flash encryption and secure boot burn eFuses for good: not for
  a board under development.
- **The firmware is not signed.** Whoever gets into the portal can install
  their own. ESP-IDF can refuse images not signed with your key, without
  secure boot; it is planned.
- **The card is readable by whoever holds it.** The cameras' passwords in
  `/cameras.txt`, for one, are in clear.
