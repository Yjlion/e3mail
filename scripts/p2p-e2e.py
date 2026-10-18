#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Two devices of one account, live: e3mail-cli against server/compose,
paired with a code and syncing over Iroh between two processes on this
machine (relay off, fixed ports).

    cd server/compose && docker compose up -d --build
    scripts/p2p-e2e.py --cli build/dev/src/cli/e3mail-cli

    --relay http://127.0.0.1:3340   the same through server/compose's relay
                                    (docker compose --profile relay up -d),
                                    with no direct paths at all
    --fallback                      the devices never run at once: changes
                                    travel by mail through the server

It empties alice's and bob's mailboxes on the test server first.
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

CONTAINER = "e3mail-test-mail"
SERVER = "127.0.0.1"
ENV = dict(os.environ, E3MAIL_NO_KEYCHAIN="1", QT_LOGGING_RULES="e3.*.info=false")


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        for log in LOGS:
            print(log)
        sys.exit(1)


class Device:
    def __init__(self, cli, root, name):
        self.cli = cli
        self.dir = os.path.join(root, name)

    def cmd(self, *args):
        return [self.cli, "--data-dir", self.dir, *args]

    def run(self, *args, raw=False):
        out = subprocess.run(self.cmd(*args), env=ENV, capture_output=True, text=True)
        if out.returncode != 0:
            raise SystemExit(f"FAIL {self.dir} {' '.join(args)}: {out.stderr.strip()} {out.stdout.strip()}")
        return out.stdout if raw else json.loads(out.stdout)

    def find(self, tag, subject):
        return next((m for m in self.run("list", "--tag", tag) if m["subject"] == subject), None)


def on_server(user, grep=None):
    """Messages in a user's Maildir; with `grep`, only those holding it."""
    find = f"find /var/mail/e3.test/{user}/Maildir/new /var/mail/e3.test/{user}/Maildir/cur -type f 2>/dev/null"
    if grep:
        find += f" | xargs -r grep -l '{grep}'"
    out = subprocess.run(["docker", "exec", CONTAINER, "sh", "-c", f"{find} | wc -l"], capture_output=True, text=True)
    return int(out.stdout.strip() or 0)


def read_json(stream):
    """One JSON object from a process still running: e3mail-cli prints each
    one indented, ending with a lone "}"."""
    lines = []
    for line in stream:
        lines.append(line)
        if line.rstrip() == "}":
            break
    return json.loads("".join(lines))


LOGS = []


def run_both(a, b, seconds):
    """Both devices running at once, as two apps would; their sync logs are
    printed if a check fails. Returns what each printed at the end (the
    paths to the devices it was connected to)."""
    env = dict(ENV, QT_LOGGING_RULES="e3.*.info=false;e3.sync.info=true;e3.engine.info=true",
               QT_FORCE_STDERR_LOGGING="1")
    procs = [subprocess.Popen(d.cmd("run", str(seconds)), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True) for d in (a, b)]
    results = []
    for d, p in zip((a, b), procs):
        out, err = p.communicate(timeout=seconds + 60)
        LOGS.append(f"--- {os.path.basename(d.dir)}, run {len(LOGS) // 2 + 1}\n{err}")
        results.append(json.loads(out) if out.strip() else {})
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="build/dev/src/cli/e3mail-cli")
    ap.add_argument("--relay", help="a relay's URL; the devices then reach each other only through it")
    ap.add_argument("--fallback", action="store_true", help="never run both devices at once")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    for user in ("alice", "bob"):
        subprocess.run(["docker", "exec", CONTAINER, "sh", "-c",
                        f"rm -rf /var/mail/e3.test/{user}/Maildir/new/* /var/mail/e3.test/{user}/Maildir/cur/*"],
                       check=True)
    root = tempfile.mkdtemp(prefix="e3mail-p2p-")
    try:
        cert = json.loads(subprocess.run([args.cli, "cert", "--in-host", SERVER, "--in-port", "1993",
                                          "--in-security", "ssl"], capture_output=True, text=True,
                                         check=True).stdout)["sha256"]
        a, b, bob = Device(args.cli, root, "A"), Device(args.cli, root, "B"), Device(args.cli, root, "bob")
        a.run("add", "--addr", "alice@e3.test", "--password", "pw", "--protocol", "imap", "--in-host", SERVER,
              "--in-port", "1993", "--in-security", "ssl", "--smtp-host", SERVER, "--smtp-port", "1465",
              "--smtp-security", "ssl", "--in-cert", cert, "--smtp-cert", cert)
        bob.run("add", "--addr", "bob@e3.test", "--password", "pw", "--protocol", "imap", "--in-host", SERVER,
                "--in-port", "1993", "--in-security", "ssl", "--smtp-host", SERVER, "--smtp-port", "1465",
                "--smtp-security", "ssl", "--insecure")
        if args.relay:
            a.run("config", "p2p_relay", args.relay)
            a.run("config", "p2p_relay_only", "1")
        else:
            a.run("config", "p2p_relay", "off")
            a.run("config", "p2p_port", "41001")
        a.run("config", "device_name", "Desktop")
        a.run("sync")
        bob.run("sync")

        print("1. pairing with a code" + (" through the relay" if args.relay else ""))
        offer = subprocess.Popen(a.cmd("pair-offer", "--yes"), env=ENV, stdout=subprocess.PIPE, text=True)
        code = read_json(offer.stdout)["code"]
        check(code.startswith("E3PAIR1:"), "the existing device prints a code")
        if args.relay:
            payload = json.loads(base64.urlsafe_b64decode(code[8:] + "=" * (-len(code[8:]) % 4)))
            addrs = json.loads(payload["a"])["addrs"]
            check(addrs and all("Relay" in x for x in addrs), "the code names only the relay, no address")
        joined = b.run("pair-join", code, "--device-name", "Laptop")
        offered = read_json(offer.stdout)
        offer.wait(timeout=30)
        check(joined["paired"] and offered["paired"], "the new device joined, and the existing one agreed")
        if args.relay:
            check(b.run("config", "p2p_relay")["p2p_relay"] == args.relay, "the new device took the relay setting")
            b.run("config", "p2p_relay_only", "1")
        else:
            b.run("config", "p2p_port", "41002")
        devs_a, devs_b = a.run("devices"), b.run("devices")
        check(devs_a["devices"][0]["name"] == "Laptop" and devs_b["devices"][0]["name"] == "Desktop",
              "each knows the other by name")
        check(devs_a["device"] != devs_b["device"], "with a device id of its own")

        print("2. server deletion waits for both devices")
        bob.run("send", "--to", "alice@e3.test", "--subject", "two devices", "--body", "hello both")
        bob.run("sync")
        a.run("sync")
        check(a.find("unverified", "two devices") is not None, "A has the message")
        check(on_server("alice") == 1, "the server keeps it: B has not got it yet")

        if args.fallback:
            fallback(a, b)
        else:
            print("3. the devices sync over Iroh" + (" through the relay" if args.relay else ""))
            ends = run_both(a, b, 20)
            m = b.find("unverified", "two devices")
            check(m is not None, "B has it")
            if args.relay:
                paths = [p for e in ends for p in e.get("paths", {}).values()]
                check(paths and all(p == "relay" for p in paths), f"connected through the relay ({paths})")
            a.run("accept", "bob@e3.test")
            a.run("archive", str(a.find("inbox", "two devices")["id"]))
            run_both(a, b, 15)
            check(b.find("archive", "two devices") is not None, "A's accept and archive reached B")
            a.run("sync")
            check(on_server("alice") == 0, "now both have it, the server copy is deleted")
        print("all passed")
    finally:
        if args.keep:
            print("kept", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


def fallback(a, b):
    """A and B are never running at once; what one does reaches the other as
    a hidden mail to the account's own address."""
    print("3. the mailbox fallback, the devices never online together")
    a.run("accept", "bob@e3.test")
    a.run("archive", str(a.find("inbox", "two devices")["id"]))
    check(a.run("fallback-now")["queued"], "A mails its changes to itself")
    check(a.run("fallback-now")["queued"] is False, "and not twice")
    b.run("sync")
    check(b.find("archive", "two devices") is not None, "B got the message, and A's accept and archive by mail")
    check([m["subject"] for m in b.run("list", "--tag", "all")] == ["two devices"], "the sync mail is hidden on B")
    check(b.run("fallback-now")["queued"], "B answers with what it holds")
    a.run("sync")
    check(on_server("alice", "two devices") == 0, "A learned B has it: the server copy is deleted")

    print("4. ten rounds, one device at a time")
    msg = str(a.find("archive", "two devices")["id"])
    for i in range(10):
        # A change on A, alternately trashing the message and restoring it.
        a.run("trash" if i % 2 == 0 else "restore", msg)
        a.run("fallback-now")
        b.run("sync")
        where = "trash" if i % 2 == 0 else "archive"
        check(b.find(where, "two devices") is not None, f"round {i + 1}: B follows A to {where}")
        b.run("fallback-now")
        a.run("sync")
    left = on_server("alice", "X-E3-Sync")
    print(f"  info  sync mails left on the server after ten rounds: {left}")
    check(left <= 2, "the sync mails do not pile up")


if __name__ == "__main__":
    main()
