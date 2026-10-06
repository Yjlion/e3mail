# Test mail server

A disposable Postfix + Dovecot server for developing and testing e3mail,
serving **IMAP and POP3** from the same mailboxes so both receive paths can be
exercised against one account set.

```sh
cd server/compose
docker compose up -d --build
```

| Port (localhost) | Service |
|---|---|
| 1465 | SMTP, implicit TLS |
| 1587 | SMTP submission, STARTTLS |
| 1993 / 1143 | IMAP, implicit TLS / STARTTLS |
| 1995 / 1110 | POP3, implicit TLS / STARTTLS |

Users `alice`, `bob`, `carol`, `dana` at `e3.test`, password `pw`. The
certificate is self-signed, so clients must be told to accept it
(`e3mail-cli add --insecure`). Credentials are refused on the STARTTLS ports
until TLS is negotiated.

`scripts/e2e.py` drives two `e3mail-cli` accounts through this server — one
over IMAP and one over POP3. It is for testing only: never expose it to a
network you do not control.
