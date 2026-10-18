#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Encrypted backups against real servers (ADR 0020): server/compose's
WebDAV, S3 and SFTP servers, and its mail server for the account.

    cd server/compose && docker compose --profile backup up -d
    scripts/backup-e2e.py [--cli build/dev/src/cli/e3mail-cli] [--tests build/dev/tests]

For each destination: the live part of tst_backup (put, get, list,
remove, then a whole backup and restore), then e3mail-cli as a person would
use it: an account, a note to self, backup-setup, backup-now, backup-list,
restore on a new data directory, and the note read there.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ENV = dict(os.environ, E3MAIL_NO_KEYCHAIN="1", QT_LOGGING_RULES="e3.*.info=false")
SERVER = "127.0.0.1"


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        sys.exit(1)


def cli(cli_path, data, *args, ok=(0,)):
    out = subprocess.run([cli_path, "--data-dir", data, *args], env=ENV, capture_output=True, text=True)
    if out.returncode not in ok:
        raise SystemExit(f"FAIL {' '.join(args[:1])}: {out.stderr.strip()} {out.stdout.strip()}")
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip().startswith(("{", "[")) else out.stdout)


def destinations(cli_path, root):
    """The three, with the SFTP server's host key learnt as a person would:
    shown by backup-setup, then pinned."""
    webdav = ({"type": "webdav", "url": f"http://{SERVER}:18080/e3backup/", "user": "e3"}, "pw")
    s3 = ({"type": "s3", "endpoint": f"http://{SERVER}:17070", "region": "us-east-1", "bucket": "e3backup",
           "prefix": "e3test", "accessKey": "e3backup", "pathStyle": True}, "e3backup-secret")
    sftp = {"type": "sftp", "host": SERVER, "port": 12222, "user": "e3", "path": "/backup"}
    probe = os.path.join(root, "probe")
    cli(cli_path, probe, "add", "--addr", "probe@e3.test", "--password", "x", "--in-host", SERVER,
        "--smtp-host", SERVER)
    code, shown = cli(cli_path, probe, "backup-setup", "--dest", json.dumps(sftp), "--dest-secret", "pw",
                      "--passphrase", "x", ok=(3,))
    check(code == 3 and shown["host_key"].startswith("SHA256:"), f"the SFTP host key is shown first: {shown['host_key']}")
    sftp["hostKey"] = shown["host_key"]
    return [("webdav",) + webdav, ("s3",) + s3, ("sftp", sftp, "pw")]


def clean():
    for container, path in (("e3mail-test-webdav", "/data/e3backup"), ("e3mail-test-s3", "/data/e3backup"),
                            ("e3mail-test-sftp", "/home/e3/backup/*")):
        subprocess.run(["docker", "exec", container, "sh", "-c", f"rm -rf {path}"], check=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="build/dev/src/cli/e3mail-cli")
    ap.add_argument("--tests", default="build/dev/tests")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    root = tempfile.mkdtemp(prefix="e3mail-backup-")
    try:
        clean()
        subprocess.run(["docker", "exec", "e3mail-test-mail", "sh", "-c",
                        "rm -rf /var/mail/e3.test/alice/Maildir/new/* /var/mail/e3.test/alice/Maildir/cur/*"], check=True)
        dests = destinations(args.cli, root)

        print("1. tst_backup against each destination")
        listed = os.path.join(root, "destinations.json")
        with open(listed, "w") as f:
            json.dump([{"config": c, "secret": s} for _, c, s in dests], f)
        out = subprocess.run([os.path.join(args.tests, "tst_backup"), "liveDestinations"],
                             env=dict(ENV, E3MAIL_TEST_DESTINATIONS=listed, QT_QPA_PLATFORM="offscreen"),
                             capture_output=True, text=True)
        if out.returncode:
            print(out.stdout[-3000:], out.stderr[-2000:])
        check(out.returncode == 0, "put, get, list, remove, backup and restore on " + ", ".join(n for n, _, _ in dests))
        clean()

        print("2. e3mail-cli, as a person would")
        cert = json.loads(subprocess.run([args.cli, "cert", "--in-host", SERVER, "--in-port", "1993", "--in-security",
                                          "ssl"], capture_output=True, text=True, check=True).stdout)["sha256"]
        for name, config, secret in dests:
            a = os.path.join(root, f"{name}-A")
            cli(args.cli, a, "add", "--addr", "alice@e3.test", "--password", "pw", "--protocol", "imap", "--in-host",
                SERVER, "--in-port", "1993", "--in-security", "ssl", "--smtp-host", SERVER, "--smtp-port", "1465",
                "--smtp-security", "ssl", "--in-cert", cert, "--smtp-cert", cert)
            cli(args.cli, a, "send", "--to", "alice@e3.test", "--subject", f"note via {name}", "--body", "the plan")
            cli(args.cli, a, "sync")
            cli(args.cli, a, "sync")
            cli(args.cli, a, "backup-setup", "--dest", json.dumps(config), "--dest-secret", secret,
                "--passphrase", "a long passphrase")
            _, first = cli(args.cli, a, "backup-now")
            check(first["ok"] and first["uploaded"] > 0, f"{name}: backed up ({first['uploaded']} objects, {first['bytes']} bytes)")
            _, again = cli(args.cli, a, "backup-now")
            check(again["ok"] and again["reused"] > 0 and again["uploaded"] < first["uploaded"],
                  f"{name}: the second time only what changed ({again['uploaded']} up, {again['reused']} already there)")
            _, listing = cli(args.cli, a, "backup-list")
            check(len(listing["snapshots"]) >= 1 and listing["snapshots"][0]["this_device"], f"{name}: listed")
            b = os.path.join(root, f"{name}-B")
            code, _ = cli(args.cli, b, "restore", "--dest", json.dumps(config), "--dest-secret", secret,
                          "--passphrase", "wrong", ok=(1,))
            check(code == 1, f"{name}: a wrong passphrase restores nothing")
            _, restored = cli(args.cli, b, "restore", "--dest", json.dumps(config), "--dest-secret", secret,
                              "--passphrase", "a long passphrase", "--device-name", "Phone")
            check(restored["addr"] == "alice@e3.test", f"{name}: restored on a new device")
            _, notes = cli(args.cli, b, "list", "--tag", "all")
            note = next((m for m in notes if m["subject"] == f"note via {name}"), None)
            check(note is not None, f"{name}: the note is there")
            _, shown = cli(args.cli, b, "show", str(note["id"]))
            check("the plan" in json.dumps(shown), f"{name}: and it decrypts")
        print("all passed")
    finally:
        if args.keep:
            print("kept", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
