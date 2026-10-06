#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Live end-to-end pass: e3mail-cli against the server in server/compose.

Covers what the in-process fake server cannot: real TLS (implicit and
STARTTLS) against real Postfix and Dovecot, both receive protocols, and
server retention observed from the server's side.

    cd server/compose && docker compose up -d --build
    scripts/e2e.py --cli build/dev/src/cli/e3mail-cli

The server's mailboxes should be empty when this starts; it uses alice (IMAP),
bob (POP3), carol (IMAP over STARTTLS) and dana (POP3 over STARTTLS).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

CONTAINER = "e3mail-test-mail"
SERVER = "127.0.0.1"


class Account:
    def __init__(self, cli, root, name, protocol, port, security, smtp_port, smtp_security):
        self.cli = cli
        self.addr = f"{name}@e3.test"
        self.dir = os.path.join(root, name)
        self.run("add", "--addr", self.addr, "--password", "pw", "--name", name.title(),
                 "--protocol", protocol, "--in-host", SERVER, "--in-port", str(port),
                 "--in-security", security, "--smtp-host", SERVER, "--smtp-port", str(smtp_port),
                 "--smtp-security", smtp_security, "--insecure")

    def run(self, *args, raw=False):
        env = dict(os.environ, E3MAIL_NO_KEYCHAIN="1", QT_LOGGING_RULES="e3.*.info=false")
        out = subprocess.run([self.cli, "--data-dir", self.dir, *args], env=env,
                             capture_output=True, text=True)
        if out.returncode != 0:
            raise SystemExit(f"FAIL {self.addr} {' '.join(args)}: {out.stderr.strip()}")
        return out.stdout if raw else json.loads(out.stdout)

    def sync(self):
        return self.run("sync")

    def find(self, tag, subject):
        for m in self.run("list", "--tag", tag):
            if m["subject"] == subject:
                return m
        return None


def on_server(user):
    out = subprocess.run(["docker", "exec", CONTAINER, "sh", "-c",
                          f"find /var/mail/e3.test/{user}/Maildir/new /var/mail/e3.test/{user}/Maildir/cur "
                          "-type f 2>/dev/null | wc -l"], capture_output=True, text=True)
    return int(out.stdout.strip() or 0)


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        sys.exit(1)


def eventually(fn, what, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            check(True, what)
            return
        time.sleep(1)
    check(False, what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="build/dev/src/cli/e3mail-cli")
    ap.add_argument("--keep", action="store_true", help="keep the data directories")
    args = ap.parse_args()
    root = tempfile.mkdtemp(prefix="e3mail-e2e-")
    try:
        print("1. accounts over every transport")
        alice = Account(args.cli, root, "alice", "imap", 1993, "ssl", 1465, "ssl")
        bob = Account(args.cli, root, "bob", "pop3", 1995, "ssl", 1587, "starttls")
        carol = Account(args.cli, root, "carol", "imap", 1143, "starttls", 1587, "starttls")
        dana = Account(args.cli, root, "dana", "pop3", 1110, "starttls", 1465, "ssl")
        for a in (alice, bob, carol, dana):
            a.sync()  # the first look: anything already there is never deleted
        check(True, "IMAPS, POP3S, IMAP+STARTTLS, POP3+STARTTLS, SMTPS and submission all log in")

        print("2. a Cc'd message with an attachment, in cleartext")
        note = os.path.join(root, "q3.csv")
        with open(note, "w") as f:
            f.write("account,q3\nops,1200\n")
        sent = alice.run("send", "--to", bob.addr, "--cc", carol.addr, "--subject", "Thursday's numbers",
                         "--body", "Numbers attached.", "--attach", note, "--important")
        check(not sent["encrypted"], "no keys known yet, so cleartext")
        alice.sync()
        eventually(lambda: bob.sync() and bob.find("unverified", "Thursday's numbers"),
                   "bob (POP3) holds it in Unverified")
        m = bob.find("unverified", "Thursday's numbers")
        shown = bob.run("show", str(m["id"]))
        check(shown["cc"][0]["addr"] == carol.addr, "Cc survives the round trip")
        check(shown["attachments"][0]["name"] == "q3.csv", "attachment survives")
        check(shown["importance"] == 1, "importance survives")
        check(on_server("bob") == 0, "POP3 delete-after-download emptied bob's mailbox")
        eventually(lambda: carol.sync() and carol.find("unverified", "Thursday's numbers"),
                   "carol (IMAP over STARTTLS) gets the copy")
        check(on_server("carol") == 0, "IMAP delete-after-download emptied carol's mailbox")

        print("3. accepting releases; the reply is encrypted")
        check(bob.run("accept", alice.addr)["released"] == 1, "accept releases held mail")
        reply = bob.run("send", "--reply-to", str(m["id"]), "--body", "The second column is off by one.")
        check(reply["encrypted"], "bob learned alice's key from Autocrypt and encrypts")
        bob.sync()
        eventually(lambda: alice.sync() and alice.find("inbox", "Re: Thursday's numbers"),
                   "alice (IMAP) receives it in the Inbox")
        r = alice.find("inbox", "Re: Thursday's numbers")
        check(r["encrypted"] and r["signed"] and not r["verified"],
              "encrypted and signed, and honestly not verified")
        body = alice.run("show", str(r["id"]))["body"]
        check(body.startswith("The second column is off by one."), "decrypted body")
        check(r["thread"] == alice.run("list", "--tag", "sent")[0]["thread"], "threads onto the original")
        raw = alice.run("show", str(r["id"]), "--raw", raw=True)
        check("off by one" not in raw and "Thursday" not in raw, "neither body nor subject is on the wire")

        print("4. retention is not retroactive")
        subprocess.run(["docker", "exec", CONTAINER, "sh", "-c",
                        "printf 'From: x@e3.test\\nSubject: pre\\n\\npre\\n' | /usr/lib/dovecot/dovecot-lda -d dana@e3.test"],
                       check=True)
        dana_dir = os.path.join(root, "dana")
        shutil.rmtree(dana_dir)
        dana = Account(args.cli, root, "dana", "pop3", 1110, "starttls", 1465, "ssl")
        dana.sync()
        check(dana.find("unverified", "pre") is not None, "dana sees mail that was there before")
        check(on_server("dana") == 1, "and it stays on the server")
        print("all passed")
    finally:
        if args.keep:
            print("data kept in", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
