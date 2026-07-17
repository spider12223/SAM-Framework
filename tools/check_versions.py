#!/usr/bin/env python3
"""
Fail if the version string has drifted between the places that carry it.

This exists because it kept happening. The Workshop description sat at v0.7.0 for five
releases while the exe went to 1.2.2, and then drifted again one release later — because
a release touches sam_logger.hpp / installer.py / the .spec / framework_version.json and
nothing ever looks at the prose. Nothing kept them honest, so they weren't.

Run before cutting a release:
    python tools/check_versions.py
Exit code 0 = every file agrees with sam_logger.hpp. Non-zero = they don't; it says which.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(rel):
    p = ROOT / rel
    return (p, p.read_text(encoding="utf-8", errors="replace") if p.exists() else None)


def main():
    # sam_logger.hpp is the source of truth — it's what the exe stamps into its banner.
    p, text = read("framework/sam_logger.hpp")
    m = re.search(r'#define SAM_FRAMEWORK_VERSION "([0-9.]+)"', text or "")
    if not m:
        print("FATAL: no SAM_FRAMEWORK_VERSION in framework/sam_logger.hpp")
        return 2
    version = m.group(1)
    print(f"version (framework/sam_logger.hpp): {version}\n")

    problems = []

    def check(rel, pattern, what):
        p, text = read(rel)
        if text is None:
            problems.append(f"{rel}: MISSING")
            return
        found = re.search(pattern, text)
        if not found:
            problems.append(f"{rel}: no {what} found")
        elif found.group(1) != version:
            problems.append(f"{rel}: {what} says {found.group(1)}, expected {version}")
        else:
            print(f"  ok  {rel:<52} {what} = {found.group(1)}")

    check("installer/installer.py", r'APP_VERSION = "([0-9.]+)"', "APP_VERSION")
    check("workshop_upload/framework_version.json", r'"version":\s*"([0-9.]+)"', "version")
    # The prose ones — the two that actually rotted.
    check("workshop_upload/workshop_description.txt", r'\[b\]Version ([0-9.]+)\[/b\]', "version line")
    check("installer/README.md", r'SAM_Framework_Installer_v([0-9.]+)\.spec', "build command")

    # The .spec named for this version has to exist, or the installer build fails.
    spec = ROOT / f"installer/SAM_Framework_Installer_v{version}.spec"
    if spec.exists():
        print(f"  ok  {spec.relative_to(ROOT).as_posix():<52} exists")
    else:
        problems.append(f"installer/SAM_Framework_Installer_v{version}.spec: MISSING")

    # Release notes are the thing a human reads first; a missing one means the release
    # ships with no explanation of what changed.
    notes = ROOT / f"RELEASE_NOTES_v{version}.md"
    if notes.exists():
        print(f"  ok  {notes.relative_to(ROOT).as_posix():<52} exists")
    else:
        problems.append(f"RELEASE_NOTES_v{version}.md: MISSING")

    if problems:
        print("\nDRIFT:")
        for x in problems:
            print(f"  !!  {x}")
        print(f"\n{len(problems)} file(s) disagree with sam_logger.hpp.")
        return 1
    print("\nAll version references agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
