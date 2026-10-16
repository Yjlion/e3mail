# 0019 — OAuth 2 for Google and Microsoft

**Status:** Accepted — 2026-10-16

## Context
Gmail and Outlook.com sign in with OAuth 2. Passwords work only as app
passwords, which need two-step verification and which Microsoft is retiring
for its consumer mail. e3mail had passwords only (ADR 0012).

## Decision
- **The mail servers take a token** through SASL: XOAUTH2, which Google and
  Microsoft both offer, or OAUTHBEARER (RFC 7628) where only that is offered.
  This applies to IMAP, POP3 and SMTP. A refused token ends the exchange the
  way the mechanisms ask: an empty line for XOAUTH2, `^A` for OAUTHBEARER.
  The server's JSON goes into the error.
- **Signing in happens in the system's browser.** It uses the authorization
  code flow with PKCE (S256) and a `state` check, and the browser comes back
  to a one-time listener on 127.0.0.1. QtNetworkAuth
  (`QOAuth2AuthorizationCodeFlow`, `QOAuthHttpServerReplyHandler`) does
  this, in `src/oauthflow` (`e3oauthflow`), which the app and the CLI share.
  e3mail never shows the provider's page itself, so the QML engine's network
  barrier (ADR 0009) is untouched.
- **The core renews tokens on its own**, with QtNetwork alone, so
  `libe3core` keeps its dependencies. It renews on the mail worker's thread
  (`net::oauth::TokenSource`), a minute before expiry. When a server refuses
  a token early, it renews once at once and tries again. When the provider
  refuses to renew (`invalid_grant`), the account's status says to sign in
  again, and the sidebar's status line does that when clicked. Rotated
  refresh tokens (Microsoft) replace the stored one.
- **The refresh token is a secret like the passwords.** It goes in the OS
  keyring (`account-<id>-oauth`), with ADR 0012's fallback (`oauth_refresh`,
  device-only). It travels to a paired device in the pairing manifest, as
  the passwords do. The account keeps the client ID, secret and token URL it
  was issued under, so another build or device can still renew it.
- **The address is checked.** The ID token's email must be the address being
  added, so signing in with another Google account is refused.
- **Client IDs belong to the person publishing the build.** They are CMake
  options taken from the environment (`E3MAIL_OAUTH_GOOGLE_ID`,
  `E3MAIL_OAUTH_GOOGLE_SECRET`, `E3MAIL_OAUTH_MICROSOFT_ID`), which the
  release workflow fills from repository secrets. The same variables at run
  time let someone use their own. Without an ID, that provider is offered
  nowhere, and passwords work as before.
- **When it is offered:** where ISPDB names the issuer (`<oAuth2><issuer>`)
  or the domain is a known Google or Microsoft one, setup shows "Sign in
  with Google/Microsoft" above an "App password" field.

## Registration, outside the code
- **Google:** a Google Cloud project with a "Desktop app" OAuth client.
  `https://mail.google.com/` is a restricted scope: until Google verifies
  the app (including a security assessment), sign-in works only for up to
  100 listed test users.
- **Microsoft:** a Microsoft Entra application for "personal Microsoft
  accounts and work or school accounts", a public client with the redirect
  URI `http://localhost` (any port), and delegated permissions
  `IMAP.AccessAsUser.All`, `POP.AccessAsUser.All` and `SMTP.Send` on Office
  365 Exchange Online, plus `offline_access`, `openid` and `email`.

## Consequences
- On Android the browser also comes back to 127.0.0.1, in the app's own
  process. Microsoft accepts this. Google's guidance for Android apps is its
  Play Services authorization API, and its review may require that, which
  would mean a Play Services dependency. Until then a "Desktop app" client
  serves Android too.
- No real Google or Microsoft account has signed in yet: everything above is
  tested against fakes until the client IDs exist.
