# 0015 — Finding server settings: who is asked, and whom to believe

**Status:** Accepted — 2026-10-07

When an address is entered, `Autoconfig` asks every source at once. It takes
the first result in this order that names both an incoming server and SMTP:

1. **The provider's own autoconfig file**, at
   `https://autoconfig.<domain>/mail/config-v1.1.xml` and
   `https://<domain>/.well-known/autoconfig/mail/config-v1.1.xml`.
2. **Thunderbird's ISPDB** (`autoconfig.thunderbird.net`).
3. **Microsoft Autodiscover** (POX): an anonymous POST to `autodiscover.<domain>`,
   to `<domain>`, and to the target of `_autodiscover._tcp.<domain>`. It follows
   one `redirectAddr`.
4. **DNS SRV** (RFC 6186): `_imaps`, `_imap`, `_pop3s`, `_pop3`, `_submissions`
   and `_submission`. Implicit TLS is preferred (RFC 8314).
5. **ISPDB for the domain's mail exchanger**: the lowest-preference MX host's
   parent domains, which covers domains hosted at a large provider.
6. **A guess**: `imap.`, `pop.` and `smtp.<domain>`.

A source ranked higher is waited for, up to ten seconds in all.

**Why this order.** The first three are HTTPS from the domain's owner or from
a curated database, and a certificate authenticates them. DNS is not
authenticated, so it ranks below them, and an MX lookup only infers which
provider handles the domain. A guess is right often enough to try, and the
connection check before saving catches it when it is wrong.

**Rules that hold for every source:**
- A cleartext connection is never suggested. The setup form still allows one
  by hand.
- No password or credential is sent during discovery. Autodiscover asks
  anonymously, a 401 counts as no answer, and only HTTPS is used. Its
  plain-HTTP redirect step is left out.
- What is found is a suggestion. `Account::check` connects with it before
  anything is saved.
- When a DNS result (SRV or MX) names hosts outside the address's domain, the
  form opens its server settings so they are seen before connecting
  (RFC 6186 §6). It does not connect to them unseen.
- What leaves the machine: the address goes to the domain's own hosts (as
  Thunderbird does), and the domain, or its MX host's domain, goes to ISPDB.

**Consequences.** Setup shows where the settings came from. Discovery is tested
against fakes (`tests/tst_autoconfig.cpp`); the real sources are only as
reliable as the providers that publish them.
