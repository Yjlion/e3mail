#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Checks every src/app/translations/*.ts: nothing unfinished or empty, and
each translation keeps its source's placeholders (%1, %n), bold tags, and
leading and trailing line breaks and spaces. Run by ctest as `translations`.

    scripts/check-translations.py [translations-dir]
"""
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET


def marks(s, source=None):
    trailing_space = s.endswith(" ")
    if source is not None and source.endswith(": ") and s.endswith("："):
        trailing_space = True  # a full-width colon carries its own space
    return (sorted(set(re.findall(r"%\d", s))), s.count("<b>"),
            len(s) - len(s.lstrip("\n")), len(s) - len(s.rstrip("\n")), trailing_space)


def check(path):
    errors = []
    plurals_only = path.endswith("_en.ts")
    for ctx in ET.parse(path).getroot().iter("context"):
        name = ctx.find("name").text
        for m in ctx.iter("message"):
            src = m.find("source").text or ""
            tr = m.find("translation")
            where = f"{os.path.basename(path)}: {name}: {src[:50]!r}"
            if tr.get("type") in ("unfinished", "vanished", "obsolete"):
                errors.append(f"{where}: {tr.get('type')}")
                continue
            if m.get("numerus") == "yes":
                forms = [f.text or "" for f in tr.findall("numerusform")]
                if not all(forms):
                    errors.append(f"{where}: an empty plural form")
                if not any("%n" in f for f in forms):
                    errors.append(f"{where}: no plural form shows %n")
                for f in forms:
                    if marks(f)[0] != marks(src)[0]:
                        errors.append(f"{where}: placeholders differ in {f!r}")
            elif not plurals_only:
                t = tr.text or ""
                if not t:
                    errors.append(f"{where}: empty")
                elif marks(t, src) != marks(src):
                    errors.append(f"{where}: {marks(t, src)} != {marks(src)} in {t!r}")
    return errors


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "src", "app", "translations")
    files = sorted(glob.glob(os.path.join(folder, "e3mail_*.ts")))
    if not files:
        print("no translations found in", folder)
        return 1
    errors = [e for f in files for e in check(f)]
    print("\n".join(errors) if errors else f"{len(files)} translation files OK")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
