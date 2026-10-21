#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Interop pass: e3mail-cli against Delta Chat's released core, over the
server in server/compose.

Checks SecureJoin (ADR 0021) both ways, then ordinary encrypted mail both
ways, between e3mail accounts and a Delta Chat account driven through
deltachat-rpc-server.

    cd server/compose && docker compose up -d --build
    python3 -m venv /tmp/dc && /tmp/dc/bin/pip install deltachat-rpc-server==2.64.0 deltachat-rpc-client==2.64.0
    /tmp/dc/bin/python scripts/interop-pass.py --cli build/dev/src/cli/e3mail-cli

It adds the users iris, jude and dchat to the test server if they are
missing; their mailboxes need not be empty.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from deltachat_rpc_client import DeltaChat, EventType, Rpc

CONTAINER = "e3mail-test-mail"
SERVER = "127.0.0.1"
DOMAIN = "e3.test"


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        sys.exit(1)


def ensure_user(name):
    """Dovecot reads its passwd-file on every login, so adding a line is enough."""
    subprocess.run(["docker", "exec", CONTAINER, "sh", "-c",
                    f"grep -q '^{name}@{DOMAIN}:' /etc/dovecot/users || "
                    f"echo '{name}@{DOMAIN}:{{PLAIN}}pw' >> /etc/dovecot/users"], check=True)


class E3:
    def __init__(self, cli, root, name):
        self.cli = cli
        self.addr = f"{name}@{DOMAIN}"
        self.dir = os.path.join(root, name)
        self.run("add", "--addr", self.addr, "--password", "pw", "--name", name.title(),
                 "--protocol", "imap", "--in-host", SERVER, "--in-port", "1993", "--in-security", "ssl",
                 "--smtp-host", SERVER, "--smtp-port", "1465", "--smtp-security", "ssl", "--insecure")

    def run(self, *args, raw=False):
        env = dict(os.environ, E3MAIL_NO_KEYCHAIN="1", QT_LOGGING_RULES="e3.*.info=false")
        out = subprocess.run([self.cli, "--data-dir", self.dir, *args], env=env, capture_output=True, text=True)
        if out.returncode != 0:
            raise SystemExit(f"FAIL {self.addr} {' '.join(args)}: {out.stderr.strip()}")
        return out.stdout if raw else json.loads(out.stdout)

    def contact(self, addr):
        for c in self.run("contacts"):
            if c["addr"] == addr:
                return c
        return None

    def find(self, subject):
        for tag in ("inbox", "unverified"):
            for m in self.run("list", "--tag", tag):
                if m["subject"] == subject:
                    return m
        return None


def keep_syncing(accounts, stop):
    """Delta Chat answers by mail; e3mail fetches and answers each round."""
    while not stop.is_set():
        for a in accounts:
            a.run("sync")
        stop.wait(1)


def until(fn, what, timeout=90):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            check(True, what)
            return
        time.sleep(1)
    check(False, what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--rpc-server", default=shutil.which("deltachat-rpc-server")
                    or os.path.join(os.path.dirname(sys.executable), "deltachat-rpc-server"))
    args = ap.parse_args()
    cli = os.path.abspath(args.cli)

    for user in ("iris", "jude", "dchat"):
        ensure_user(user)
    root = tempfile.mkdtemp(prefix="e3-interop-")
    iris = E3(cli, root, "iris")
    jude = E3(cli, root, "jude")
    for a in (iris, jude):
        a.run("sync")  # mail left from an earlier run is pre-existing, not news

    with Rpc(accounts_dir=os.path.join(root, "dc"), rpc_server_path=args.rpc_server) as rpc:
        dc = DeltaChat(rpc)
        print("delta chat", dc.get_system_info().deltachat_core_version, flush=True)
        acc = dc.add_account()
        acc.set_config("displayname", "Dee Chat")
        acc.add_or_update_transport({
            "addr": f"dchat@{DOMAIN}", "password": "pw",
            "imapServer": SERVER, "imapPort": 1993, "imapSecurity": "ssl",
            "smtpServer": SERVER, "smtpPort": 1465, "smtpSecurity": "ssl",
            "certificateChecks": "acceptInvalidCertificates",
        })
        acc.start_io()
        stop = threading.Event()
        syncer = threading.Thread(target=keep_syncing, args=([iris, jude], stop), daemon=True)
        syncer.start()
        try:
            print("e3mail scans Delta Chat's code")
            code = acc.get_qr_code()
            check(code.startswith("https://i.delta.chat/") or code.upper().startswith("OPENPGP4FPR:"),
                  f"Delta Chat shows a code ({code.split('#')[0]})")
            iris.run("verify-join", code)
            until(lambda: any(j["addr"] == f"dchat@{DOMAIN}" and j["state"] == "done"
                              for j in iris.run("verify-status")),
                  "Delta Chat confirmed iris")
            c = iris.contact(f"dchat@{DOMAIN}")
            check(c and c["verified"], "iris holds Delta Chat's key verified")

            print("Delta Chat scans e3mail's code")
            invite = jude.run("verify-invite", raw=True).strip()
            check(invite.startswith("OPENPGP4FPR:"), "e3mail shows a code")
            acc.secure_join(invite)
            event = None
            deadline = time.time() + 90
            while time.time() < deadline:
                event = acc.wait_for_event()
                if event.kind == EventType.SECUREJOIN_JOINER_PROGRESS and event.progress == 1000:
                    break
                event = None
            check(event is not None, "Delta Chat finished joining jude")
            c = jude.contact(f"dchat@{DOMAIN}")
            check(c and c["verified"], "jude holds Delta Chat's key verified")

            print("Encrypted mail both ways")
            jude.run("send", "--to", f"dchat@{DOMAIN}", "--subject", "from e3mail", "--body", "hello from jude")
            got = None
            deadline = time.time() + 90
            while time.time() < deadline and not got:
                for m in acc.get_next_messages():
                    s = m.get_snapshot()
                    if "hello from jude" in (s.text or ""):
                        got = s
                time.sleep(1)
            check(got is not None, "Delta Chat received jude's mail")
            check(got.show_padlock, "it was encrypted")
            chat = acc.get_contact_by_addr(f"jude@{DOMAIN}").create_chat()
            chat.send_text("hello from Delta Chat")
            def from_dc():
                for m in jude.run("list", "--tag", "inbox"):
                    shown = jude.run("show", str(m["id"]))
                    if "hello from Delta Chat" in json.dumps(shown):
                        return shown
                return None
            until(lambda: from_dc() is not None, "jude received Delta Chat's mail")
            shown = from_dc()
            check(shown.get("encrypted"), "it was encrypted")
            check(shown.get("verified"), "and signed by the verified key")
        finally:
            stop.set()
            syncer.join()
            acc.stop_io()
    shutil.rmtree(root, ignore_errors=True)
    print("interop pass: all ok")


if __name__ == "__main__":
    main()
