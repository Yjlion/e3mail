# 0017 — Self-signed servers: pin one certificate, after showing it

**Status:** Accepted — 2026-10-11

## Context
A mail server with a self-signed certificate (a home server, a test server
such as `server/compose`, a small provider) could only be used from
`e3mail-cli add --insecure`, which turns certificate checks off for the whole
account. The app had no way at all, so Android could not reach the test
server. Turning checks off is not something to offer in a form: anyone on the
path could then read the password.

## Decision
- **Show, then pin.** When TLS refuses a server's certificate, setup shows
  why, the certificate's SHA-256 fingerprint, its subject, issuer and expiry,
  and asks the person to trust it only if the fingerprint matches what the
  server's administrator gave them. Trusting stores that one fingerprint for
  that server (`in_cert_sha256`, `smtp_cert_sha256`).
- **A pin is exact.** The pinned certificate is accepted whatever TLS says
  of it, including a name that does not match (an emulator reaching the host
  as `10.0.2.2`). Any other certificate from that server is refused, even
  one TLS would trust, so a pin cannot be bypassed by a certificate from a
  public authority.
- **A changed certificate stops the account** with status `cert` and the new
  fingerprint. Settings offers to trust it, or to forget the pin and go back
  to TLS's own judgement. Nothing is trusted without a person deciding.
- **SMTP on the same host with the same certificate** is pinned with the
  incoming server's, since the person has just trusted exactly that
  certificate for that host.
- Pins are server settings: per device, never synced (ADR 0013).
- `--insecure` stays, for tests and `scripts/e2e.py`, and is never set by
  the app. `e3mail-cli cert` prints a server's certificate, and `add
  --in-cert/--smtp-cert` pin from the command line.

## Consequences
- A server that renews a self-signed certificate stops until someone trusts
  the new one: deliberate, and the price of the pin meaning something.
- The fingerprint check is only as good as the person's comparison. The
  card says where the right value comes from.
