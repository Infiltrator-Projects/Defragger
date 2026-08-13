#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enforce the project-wide first-party GPL-3.0-or-later SPDX policy."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IDENTIFIER = "SPDX-License-Identifier: GPL-3.0-or-later"
EXCLUDED_TOP_LEVEL = {"LICENSE", "LICENSES", ".git", ".pytest_cache"}
EXCLUDED_PREFIXES = ("build", "native-verify", "release")

HASH_SUFFIXES = {".py", ".sh", ".ini", ".desktop", ".yml", ".yaml"}
SLASH_SUFFIXES = {".c", ".h"}
HTML_SUFFIXES = {".md"}
XML_SUFFIXES = {".svg"}
SIDECAR_REQUIRED = {
    Path("VERSION"),
    Path("pyrightconfig.json"),
    Path("tests/fixtures/affs-ffs-fragmented.adf.gz"),
    Path("tests/fixtures/affs-ofs-fragmented.adf.gz"),
}


def iter_first_party_files() -> list[Path]:
    files: list[Path] = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if rel.parts and rel.parts[0] in EXCLUDED_TOP_LEVEL:
            continue
        if rel.parts[:2] == ("shared", "infiltratr-common"):
            continue
        if any(part.startswith(EXCLUDED_PREFIXES) for part in rel.parts):
            continue
        if path.suffix in {".deb", ".run", ".zip", ".pyc"}:
            continue
        if "__pycache__" in rel.parts:
            continue
        files.append(path)
    return files


def expected_prefix(path: Path) -> str | None:
    rel = path.relative_to(ROOT)
    name = path.name
    if name == "CMakeLists.txt" or path.suffix == ".cmake":
        return "# " + IDENTIFIER
    if path.suffix in SLASH_SUFFIXES or name.endswith(".h.in"):
        return "// " + IDENTIFIER
    if (
        path.suffix in HASH_SUFFIXES
        or name.endswith(".py.in")
        or name.endswith(".sh.in")
        or rel == Path("packaging/linux-defragger")
    ):
        return "# " + IDENTIFIER
    if path.suffix in HTML_SUFFIXES:
        return "<!-- " + IDENTIFIER + " -->"
    if path.suffix in XML_SUFFIXES:
        return "<!-- " + IDENTIFIER + " -->"
    if path.suffix == ".license":
        return IDENTIFIER
    return None


def test_spdx_coverage() -> None:
    failures: list[str] = []
    for path in iter_first_party_files():
        rel = path.relative_to(ROOT)
        if rel in SIDECAR_REQUIRED:
            sidecar = Path(str(path) + ".license")
            if not sidecar.is_file() or sidecar.read_text(encoding="utf-8").strip() != IDENTIFIER:
                failures.append(f"{rel}: missing exact .license sidecar")
            continue

        prefix = expected_prefix(path)
        if prefix is None:
            # The canonical project licence text is excluded above. Any new
            # otherwise-unclassified first-party file must declare its licence
            # through a sidecar rather than silently escaping the policy.
            sidecar = Path(str(path) + ".license")
            if not sidecar.is_file() or sidecar.read_text(encoding="utf-8").strip() != IDENTIFIER:
                failures.append(f"{rel}: unclassified file has no GPL-3.0-or-later sidecar")
            continue

        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            failures.append(f"{rel}: binary/unreadable file lacks declared sidecar policy")
            continue
        first_lines = lines[:8]
        if prefix not in first_lines:
            failures.append(f"{rel}: expected {prefix!r} in first 8 lines")

    assert not failures, "SPDX licensing failures:\n" + "\n".join(failures)


def test_no_first_party_gpl2_spdx_headers() -> None:
    offenders: list[str] = []
    for path in iter_first_party_files():
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        if ("SPDX-License-Identifier: " + "GPL-2") in text:
            offenders.append(str(path.relative_to(ROOT)))
    assert not offenders, f"first-party GPL-2 SPDX headers found: {offenders}"


def test_project_licence_text_is_present() -> None:
    licence = ROOT / "LICENSES" / "GPL-3.0-or-later.txt"
    text = licence.read_text(encoding="utf-8")
    assert "GNU GENERAL PUBLIC LICENSE" in text
    assert "Version 3, 29 June 2007" in text


def main() -> None:
    test_spdx_coverage()
    test_no_first_party_gpl2_spdx_headers()
    test_project_licence_text_is_present()
    print("GPL-3.0-or-later SPDX licensing gate passed")


if __name__ == "__main__":
    main()
