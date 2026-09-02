# Security

## Reporting a vulnerability

Please report security issues privately, through GitHub's
[private vulnerability reporting](https://github.com/GnumBix/immich-ksync/security/advisories/new),
rather than as a public issue.

Include what you did, what happened, and what you expected. A proof of concept helps but
is not required — a clear description of the flaw is enough to start.

You should get a first response within a week. If a fix is warranted, you will be
credited in the release notes unless you would rather not be.

## What this app handles

- **An Immich API key or session token.** Stored in the Secret Service
  (`org.freedesktop.secrets`) — `ksecretd` on Plasma 6, `kwalletd6` or `gnome-keyring`
  elsewhere. Never written to the config file, the log, or the SQLite database.
- **An account password**, if you sign in with one. Exchanged once for a session token
  and never persisted.
- **A TLS client certificate and its passphrase**, if your server asks for one. Stored in
  the Secret Service, and deliberately *not* cleared by Sign Out — without it the server
  may be unreachable, which would lock you out of the screen you need in order to sign
  back in.
- **Your photographs**, as ordinary files in a folder you choose.

## Security properties this app intends to hold

These are the ones worth reporting a bug against:

1. **No path accepts a TLS certificate the system would have rejected.**
   The private-CA feature adds an anchor to the system set for one configured host. It
   does not replace the system anchors, does not disable hostname or expiry checking, and
   `ignoreSslErrors()` appears nowhere in the codebase.

2. **The extra anchor applies to exactly one host.**
   Matching is an exact, case-insensitive comparison — never a prefix, suffix or
   substring test. A `contains` test would trust the anchor for `example.com` when you
   configured `immich.example.com`; the reverse would accept
   `immich.example.com.attacker.net`.

3. **Secrets never leave the Secret Service.**
   Not into the log, not into the database, not into the config file. Log lines that
   mention a credential print a redacted description.

4. **The app cannot delete an asset from your Immich library.**
   It never asks for the `asset.delete` permission and has no code that would use it. The
   strongest remote action is removing an asset from an album.

5. **The app never deletes a local file.**
   A file whose asset left its album is moved to `.immich-trash/`, where it stays until
   you decide otherwise.

6. **Uploads do not follow redirects.**
   A 307 would replay the request body, and a 30x to an HTML login page would look like a
   successful upload.

## Out of scope

- Anything requiring an attacker who already has your user account: at that point they
  can read the process's memory and the keyring alike.
- Vulnerabilities in Immich itself — report those to
  [the Immich project](https://github.com/immich-app/immich/security).
- Denial of service through an enormous photo library. It will be slow; that is not a
  vulnerability.
