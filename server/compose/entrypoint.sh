#!/bin/sh
# Provisions the test users, then runs Dovecot and Postfix in the foreground.
set -e
: > /etc/dovecot/users
for user in ${E3_USERS:-alice bob carol dana}; do
    echo "${user}@e3.test:{PLAIN}${E3_PASSWORD:-pw}" >> /etc/dovecot/users
done
chmod 640 /etc/dovecot/users && chown root:dovecot /etc/dovecot/users
cp /etc/resolv.conf /var/spool/postfix/etc/resolv.conf 2>/dev/null || true
dovecot
exec postfix start-fg
