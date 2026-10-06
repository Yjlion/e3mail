# A disposable mail server for developing and testing e3mail: Postfix for
# submission, Dovecot for IMAP, POP3 and local delivery. Self-signed TLS on
# every port; never expose it to a network you do not control.
FROM debian:bookworm-slim

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        postfix dovecot-imapd dovecot-pop3d dovecot-lmtpd openssl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/ssl/e3 \
 && openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -subj "/CN=mail.e3.test" -addext "subjectAltName=DNS:mail.e3.test,DNS:localhost,IP:127.0.0.1" \
        -keyout /etc/ssl/e3/key.pem -out /etc/ssl/e3/cert.pem \
 && chmod 600 /etc/ssl/e3/key.pem

COPY dovecot.conf /etc/dovecot/dovecot.conf
COPY postfix-main.cf /etc/postfix/main.cf
COPY postfix-master-extra.cf /tmp/master-extra.cf
COPY entrypoint.sh /entrypoint.sh
RUN cat /tmp/master-extra.cf >> /etc/postfix/master.cf \
 && groupadd -g 5000 vmail && useradd -u 5000 -g vmail -d /var/mail -s /usr/sbin/nologin vmail \
 && mkdir -p /var/mail && chown -R vmail:vmail /var/mail \
 && chmod +x /entrypoint.sh

EXPOSE 25 465 587 110 143 993 995
ENTRYPOINT ["/entrypoint.sh"]
