#!/usr/bin/env python3
"""Validates app.json manifests.

    python3 tools/validate-manifest.py apps/calculator
    python3 tools/validate-manifest.py --all apps

Errors fail the command. Warnings do not: they flag conventions the player
tolerates but the repository asks for.
"""

from __future__ import annotations

import json
import os
import re
import sys

KNOWN_FIELDS = {
    "id",
    "name",
    "version",
    "entry",
    "icon",
    "description",
    "min_player",
    "tier",
}

SEMVER = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$")
APP_ID = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
TIERS = {"light", "heavy"}


def safe_relative(path: str) -> bool:
    """The player's rule: relative, no backslash, and no '..' anywhere."""
    return bool(path) and not path.startswith("/") and "\\" not in path and ".." not in path


def check_app(app_dir: str) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    dirname = os.path.basename(os.path.normpath(app_dir))
    manifest_path = os.path.join(app_dir, "app.json")
    if not os.path.isfile(manifest_path):
        return [f"{manifest_path}: missing"], warnings

    try:
        with open(manifest_path, encoding="utf-8") as handle:
            manifest = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{manifest_path}: {exc}"], warnings

    if not isinstance(manifest, dict):
        return [f"{manifest_path}: the root must be an object"], warnings

    app_id = manifest.get("id", dirname)
    if not isinstance(app_id, str) or not app_id:
        errors.append(f"{manifest_path}: id must be a non-empty string")
    else:
        if not APP_ID.fullmatch(app_id):
            errors.append(
                f"{manifest_path}: id {app_id!r} must be lowercase with no spaces"
            )
        if app_id != dirname:
            errors.append(
                f"{manifest_path}: id {app_id!r} must match the directory name {dirname!r}"
            )

    name = manifest.get("name", app_id)
    if not isinstance(name, str) or not name:
        errors.append(f"{manifest_path}: name must be a non-empty string")

    version = manifest.get("version", "")
    if not isinstance(version, str):
        errors.append(f"{manifest_path}: version must be a string")
    elif not version:
        warnings.append(f"{manifest_path}: version is recommended")
    elif not SEMVER.fullmatch(version):
        warnings.append(f"{manifest_path}: version {version!r} is not semver")

    entry = manifest.get("entry")
    if not isinstance(entry, str) or not entry:
        errors.append(f"{manifest_path}: entry is required")
    elif not safe_relative(entry):
        errors.append(
            f"{manifest_path}: entry {entry!r} must be relative to the app folder"
        )
    else:
        entry_path = os.path.join(app_dir, entry)
        if os.path.isdir(entry_path):
            errors.append(f"{manifest_path}: entry {entry!r} is a directory")

    icon = manifest.get("icon")
    if icon is not None:
        if not isinstance(icon, str):
            errors.append(f"{manifest_path}: icon must be a string")
        elif icon:
            if not safe_relative(icon):
                errors.append(
                    f"{manifest_path}: icon {icon!r} must be relative to the app folder"
                )
            elif not os.path.isfile(os.path.join(app_dir, icon)):
                errors.append(f"{manifest_path}: icon {icon!r} does not exist")

    description = manifest.get("description", "")
    if not isinstance(description, str):
        errors.append(f"{manifest_path}: description must be a string")
    else:
        if "\n" in description:
            errors.append(f"{manifest_path}: description must be one line")
        if len(description) > 160:
            warnings.append(f"{manifest_path}: description is longer than 160 bytes")
        if description.endswith("."):
            warnings.append(f"{manifest_path}: description should not end with a period")

    min_player = manifest.get("min_player", 0)
    if isinstance(min_player, bool) or not isinstance(min_player, int) or min_player < 0:
        errors.append(f"{manifest_path}: min_player must be a non-negative integer")

    tier = manifest.get("tier", "light")
    if not isinstance(tier, str) or tier not in TIERS:
        errors.append(f"{manifest_path}: tier must be light or heavy")

    for field in sorted(set(manifest) - KNOWN_FIELDS):
        warnings.append(f"{manifest_path}: unknown field {field!r} is ignored by the player")

    return errors, warnings


def app_dirs(args: list[str]) -> list[str]:
    if "--all" in args:
        index = args.index("--all")
        if index + 1 >= len(args):
            print("usage: --all <apps-directory>", file=sys.stderr)
            raise SystemExit(2)
        root = args[index + 1]
        return [
            os.path.join(root, name)
            for name in sorted(os.listdir(root))
            if os.path.isdir(os.path.join(root, name))
        ]
    return args


def main(argv: list[str]) -> int:
    directories = app_dirs(argv[1:])
    if not directories:
        print("usage: validate-manifest.py <app-dir>... | --all <apps-dir>", file=sys.stderr)
        return 2

    failed = False
    for app_dir in directories:
        errors, warnings = check_app(app_dir)
        label = os.path.basename(os.path.normpath(app_dir))
        for warning in warnings:
            print(f"warning: {warning}", file=sys.stderr)
        if errors:
            failed = True
            for error in errors:
                print(f"error: {error}", file=sys.stderr)
            print(f"  {label}: failed")
        else:
            suffix = f" ({len(warnings)} warning{'s' if len(warnings) != 1 else ''})"
            print(f"  {label}: ok{suffix}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
