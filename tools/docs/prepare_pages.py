#!/usr/bin/env python3
"""Stage tracked Koi Markdown and static docs assets as the MkDocs source tree.

The source tree is deliberately assembled from Git's index instead of the
working tree.  That makes the Pages corpus reproducible and keeps ignored
build products, local notes, networks, books, and tablebases out of the site.
"""

from __future__ import annotations

import argparse
import posixpath
import re
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath
from urllib.parse import quote


REPOSITORY_URL = "https://github.com/nhatquang1701/Koi"
BRANCH = "main"
SCRIPT_PATH = Path(__file__).resolve()
REPOSITORY_ROOT = SCRIPT_PATH.parents[2]
SOURCE_DIRECTORY = REPOSITORY_ROOT / "build" / "pages-src"
EXCLUDED_PREFIXES = (
    "third_party/",
    "build/",
    ".superpowers/",
    ".github/",
    "tests/data/",
    "artifacts/",
)
MARKDOWN_LINK = re.compile(r"(?<!!)\[([^\]]+)\]\(([^)]+)\)")


def tracked_paths(pathspec: str | None = None) -> list[PurePosixPath]:
    """Return repository paths reported by Git's index."""
    command = ["git", "ls-files", "-z"]
    if pathspec is not None:
        command.extend(["--", pathspec])
    result = subprocess.run(
        command,
        cwd=REPOSITORY_ROOT,
        check=True,
        capture_output=True,
    )
    paths = [
        PurePosixPath(item.decode("utf-8"))
        for item in result.stdout.split(b"\0")
        if item
    ]
    return sorted(paths)


def tracked_markdown_paths() -> list[PurePosixPath]:
    """Return committed Markdown paths that belong to Koi's documentation."""
    return [
        path
        for path in tracked_paths("*.md")
        if not path.as_posix().startswith(EXCLUDED_PREFIXES)
    ]


def tracked_asset_paths() -> list[PurePosixPath]:
    """Return committed static asset paths under docs/assets/."""
    return [
        path
        for path in tracked_paths("docs/assets")
        if not path.as_posix().endswith(".md")
    ]


def is_external(target: str) -> bool:
    return target.startswith(("#", "/", "http://", "https://", "mailto:", "data:"))


def split_destination(destination: str) -> tuple[str, str]:
    """Split a Markdown destination from an optional anchor or quoted title."""
    destination = destination.strip()
    if destination.startswith("<") and destination.endswith(">"):
        destination = destination[1:-1]
    target, separator, suffix = destination.partition(" ")
    if "#" in target:
        path, anchor = target.split("#", 1)
        return path, f"#{anchor}" + (separator + suffix if separator else "")
    return target, separator + suffix if separator else ""


def github_url(path: PurePosixPath, is_directory: bool, suffix: str) -> str:
    kind = "tree" if is_directory else "blob"
    quoted_path = quote(path.as_posix())
    return f"{REPOSITORY_URL}/{kind}/{BRANCH}/{quoted_path}{suffix}"


def output_path(source_path: PurePosixPath) -> PurePosixPath:
    """Avoid MkDocs' special index treatment for repository README files."""
    if source_path == PurePosixPath("docs/index.md"):
        return PurePosixPath("index.md")
    if source_path == PurePosixPath("README.md"):
        return PurePosixPath("repository.md")
    if source_path == PurePosixPath("docs/README.md"):
        return PurePosixPath("docs/overview.md")
    return source_path


def rewrite_links(
    markdown: str,
    source_path: PurePosixPath,
    destination_path: PurePosixPath,
    source_to_output: dict[PurePosixPath, PurePosixPath],
    repository_paths: set[PurePosixPath],
) -> str:
    """Keep corpus links relative; turn omitted tracked files into GitHub links."""

    def replace(match: re.Match[str]) -> str:
        label, destination = match.groups()
        target, suffix = split_destination(destination)
        if not target or is_external(target):
            return match.group(0)
        resolved = PurePosixPath(source_path.parent, target)
        normalized = PurePosixPath(posixpath.normpath(resolved.as_posix()))
        if ".." in normalized.parts:
            return match.group(0)
        if normalized in source_to_output:
            mapped_target = source_to_output[normalized]
            if mapped_target == normalized and destination_path == source_path:
                return match.group(0)
            relative_target = posixpath.relpath(
                mapped_target.as_posix(), destination_path.parent.as_posix() or "."
            )
            return f"[{label}]({relative_target}{suffix})"
        disk_path = REPOSITORY_ROOT.joinpath(*normalized.parts)
        if normalized in repository_paths:
            return f"[{label}]({github_url(normalized, disk_path.is_dir(), suffix)})"
        if disk_path.exists():
            return label
        return match.group(0)

    return MARKDOWN_LINK.sub(replace, markdown)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--output",
        type=Path,
        default=SOURCE_DIRECTORY,
        help="directory to stage tracked Markdown and static docs assets (default: build/pages-src)",
    )
    args = parser.parse_args(argv)
    output_directory = args.output.resolve()

    paths = tracked_markdown_paths()
    if not paths:
        raise RuntimeError("No tracked Markdown files were found.")

    source_to_output = {path: output_path(path) for path in paths}
    repository_paths = set(tracked_paths())
    if PurePosixPath("docs/index.md") not in source_to_output:
        raise RuntimeError("docs/index.md must be tracked before building the Pages site.")

    if output_directory.exists():
        shutil.rmtree(output_directory)
    output_directory.mkdir(parents=True)

    for path in paths:
        source = REPOSITORY_ROOT.joinpath(*path.parts)
        staged_path = source_to_output[path]
        destination = output_directory.joinpath(*staged_path.parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        markdown = source.read_text(encoding="utf-8")
        destination.write_text(
            rewrite_links(markdown, path, staged_path, source_to_output, repository_paths),
            encoding="utf-8",
        )

    asset_paths = tracked_asset_paths()
    for asset_path in asset_paths:
        source = REPOSITORY_ROOT.joinpath(*asset_path.parts)
        relative_asset = asset_path.relative_to("docs")
        destination = output_directory.joinpath(*relative_asset.parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    try:
        display_output = output_directory.relative_to(REPOSITORY_ROOT)
    except ValueError:
        display_output = output_directory
    print(
        f"Staged {len(paths)} tracked Markdown files and {len(asset_paths)} static assets "
        f"in {display_output}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"prepare_pages.py: {error}", file=sys.stderr)
        raise SystemExit(1)
