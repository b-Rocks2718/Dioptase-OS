#!/usr/bin/env python3
"""Extract an ext2 image into a host directory using debugfs.

Purpose:
  Turn the emulator's SD1 output image into a normal host directory tree so OS
  tests can inspect filesystem side effects without mounting loop devices.

Inputs:
  argv[1]: path to the ext2 image.
  argv[2]: destination directory on the host.

Outputs:
  Replaces the destination directory with regular files, directories, and
  symlinks from the image root.

Important assumptions:
  The script relies on `debugfs ls -p`, `dump`, and `stat`.
  Host paths passed to debugfs must not contain whitespace; the Dioptase repo
  layout satisfies that constraint.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEBUGFS_BANNER_PREFIX = "debugfs "
DEBUGFS_ERROR_PREFIXES = ("debugfs:", "ls:", "dump:", "stat:")
FAST_LINK_RE = re.compile(r'^Fast link dest: "(.*)"$')


def run_debugfs(image: Path, command: str, cwd: Path | None = None) -> list[str]:
    """Run one debugfs command and return non-banner output lines."""
    proc = subprocess.run(
        ["debugfs", "-R", command, str(image)],
        check=False,
        capture_output=True,
        cwd=cwd,
        text=True,
    )
    lines: list[str] = []
    for stream in (proc.stdout, proc.stderr):
        for line in stream.splitlines():
            if line.startswith(DEBUGFS_BANNER_PREFIX):
                continue
            if line:
                lines.append(line)

    if proc.returncode != 0 or any(
        line.startswith(DEBUGFS_ERROR_PREFIXES) for line in lines
    ):
        details = "\n".join(lines).strip()
        if not details:
            details = "no error details from debugfs"
        raise RuntimeError(f"debugfs command failed: {command}\n{details}")

    return lines


def join_guest_path(parent: str, name: str) -> str:
    if parent == "/":
        return f"/{name}"
    return f"{parent.rstrip('/')}/{name}"


def parse_ls_entry(line: str) -> tuple[str, str, str]:
    parts = line.split("/")
    if len(parts) < 7 or parts[0] != "":
        raise RuntimeError(f"unexpected debugfs ls output: {line}")
    inode = parts[1]
    mode = parts[2]
    name = parts[5]
    return inode, mode, name


def read_symlink_target(image: Path, guest_path: str) -> str:
    """Read a symlink target from the ext2 image."""
    stat_lines = run_debugfs(image, f"stat {guest_path}")
    for line in stat_lines:
        match = FAST_LINK_RE.match(line)
        if match:
            return match.group(1)

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_root = Path(temp_dir)
        temp_path = temp_root / "symlink-target"
        run_debugfs(image, f"dump {guest_path} symlink-target", cwd=temp_root)
        return temp_path.read_text(encoding="utf-8", errors="surrogateescape")


def extract_tree(image: Path, guest_dir: str, host_dir: Path, output_root: Path) -> None:
    """Recursively copy supported inode types into the host directory."""
    host_dir.mkdir(parents=True, exist_ok=True)
    listing = run_debugfs(image, f"ls -p {guest_dir}")
    for line in listing:
        inode, mode, name = parse_ls_entry(line)
        if inode == "0" or mode == "000000" or name in ("", ".", ".."):
            continue

        guest_path = join_guest_path(guest_dir, name)
        host_path = host_dir / name
        file_mode = int(mode, 8) & 0o777

        if mode.startswith("04"):
            extract_tree(image, guest_path, host_path, output_root)
            os.chmod(host_path, file_mode)
            continue

        if mode.startswith("10"):
            host_path.parent.mkdir(parents=True, exist_ok=True)
            relative_host_path = host_path.relative_to(output_root).as_posix()
            run_debugfs(image, f"dump {guest_path} {relative_host_path}", cwd=output_root)
            os.chmod(host_path, file_mode)
            continue

        if mode.startswith("12"):
            host_path.parent.mkdir(parents=True, exist_ok=True)
            target = read_symlink_target(image, guest_path)
            if host_path.exists() or host_path.is_symlink():
                host_path.unlink()
            os.symlink(target, host_path)
            continue

        raise RuntimeError(
            f"unsupported inode type for {guest_path}: mode {mode}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract an ext2 image into a host directory via debugfs.",
    )
    parser.add_argument("image", type=Path, help="ext2 image to extract")
    parser.add_argument("output_dir", type=Path, help="host directory to replace")
    args = parser.parse_args()

    image = args.image.resolve()
    output_dir = args.output_dir.resolve()

    if not image.is_file():
        raise RuntimeError(f"ext2 image does not exist: {image}")

    for path in (image, output_dir):
        if any(ch.isspace() for ch in str(path)):
            raise RuntimeError(
                f"paths with whitespace are not supported by debugfs extraction: {path}"
            )

    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    try:
        extract_tree(image, "/", output_dir, output_dir)
    except Exception:
        if output_dir.exists():
            shutil.rmtree(output_dir)
        raise
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as err:
        print(f"extract_ext2_dir.py: {err}", file=sys.stderr)
        raise SystemExit(1)
