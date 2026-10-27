#!/usr/bin/env python3
"""Require milestone source defaults to match the requested native version."""

import argparse
from pathlib import Path
import re


def validate(root: Path, version: str) -> None:
    expected = version.removeprefix("v").split("-", 1)[0]
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", expected):
        raise ValueError("Expected a native SemVer version")
    patterns = {
        "CMakeLists.txt": r"project\(fileflow LANGUAGES CXX VERSION ([0-9]+\.[0-9]+\.[0-9]+)\)",
        "app/build.gradle.kts": r'val releaseVersionName = providers\.gradleProperty\("fileflowVersionName"\)\.orElse\("([0-9]+\.[0-9]+\.[0-9]+)"\)\.get\(\)',
    }
    for name, pattern in patterns.items():
        source = (root / name).read_text(encoding="utf-8")
        match = re.search(pattern, source)
        if not match or match.group(1) != expected:
            actual = match.group(1) if match else "missing"
            raise ValueError(f"{name} version is {actual}; expected {expected}")
        if name.endswith(".kts") and not re.search(r"\bversionName\s*=\s*releaseVersionName\b", source):
            raise ValueError("Android versionName must use releaseVersionName")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    args = parser.parse_args()
    validate(Path(__file__).resolve().parents[1], args.version)
