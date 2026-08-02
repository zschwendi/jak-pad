#!/usr/bin/env python3

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPOSITORY_ROOT / "decompiler/config/jak1/fr3-project-resources.sha256"
MANIFEST_LINE = re.compile(r"([0-9a-f]{64})  ([\x21-\x7e]+)")
ALLOWED_PREFIXES = (
    "decompiler/config/jak1/",
    "game/assets/jak1/patches/",
)
ALLOWED_EXACT_PATHS = {"goal_src/jak1/build/all_objs.json"}
FORBIDDEN_COMPONENTS = {"decompiler_out", "iso_data"}
FORBIDDEN_SUFFIXES = {
    ".bin",
    ".cgo",
    ".dgo",
    ".fr3",
    ".iso",
    ".str",
    ".txt",
    ".vag",
}
MAX_FILE_BYTES = 4 * 1024 * 1024
MAX_BUNDLE_BYTES = 8 * 1024 * 1024
NORMALIZED_MTIME = 946684800


class BundleError(Exception):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def read_manifest() -> list[tuple[str, PurePosixPath]]:
    entries: list[tuple[str, PurePosixPath]] = []
    for line_number, raw_line in enumerate(MANIFEST_PATH.read_text(encoding="ascii").splitlines(), 1):
        if not raw_line or raw_line.startswith("#"):
            continue
        match = MANIFEST_LINE.fullmatch(raw_line)
        if not match:
            raise BundleError(f"invalid manifest line {line_number}")
        expected_hash, raw_path = match.groups()
        relative_path = PurePosixPath(raw_path)
        parts = relative_path.parts
        if relative_path.is_absolute() or not parts or any(part in {"", ".", ".."} for part in parts):
            raise BundleError(f"unsafe manifest path: {raw_path}")
        if "\\" in raw_path or any(part in FORBIDDEN_COMPONENTS for part in parts):
            raise BundleError(f"forbidden manifest path: {raw_path}")
        if relative_path.suffix.lower() in FORBIDDEN_SUFFIXES:
            raise BundleError(f"retail or generated file type is forbidden: {raw_path}")
        if raw_path not in ALLOWED_EXACT_PATHS and not raw_path.startswith(ALLOWED_PREFIXES):
            raise BundleError(f"path is outside the FR3 project-resource allowlist: {raw_path}")
        entries.append((expected_hash, relative_path))

    paths = [path.as_posix() for _, path in entries]
    if not entries or paths != sorted(paths) or len(paths) != len(set(paths)):
        raise BundleError("manifest paths must be nonempty, unique, and sorted")
    return entries


def ensure_repository_root(source_root: Path) -> None:
    result = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "--show-toplevel"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode or Path(result.stdout.strip()).resolve() != source_root:
        raise BundleError("source root is not the root of an OpenGOAL Git checkout")


def ensure_tracked(source_root: Path, entries: list[tuple[str, PurePosixPath]]) -> None:
    paths = [path.as_posix() for _, path in entries]
    result = subprocess.run(
        ["git", "-C", str(source_root), "ls-files", "--error-unmatch", "--", *paths],
        check=False,
        capture_output=True,
        text=True,
    )
    tracked = result.stdout.splitlines()
    if result.returncode or sorted(tracked) != paths:
        raise BundleError("every resource must be explicitly tracked by Git")


def ensure_regular_source(source_root: Path, relative_path: PurePosixPath) -> Path:
    current = source_root
    for part in relative_path.parts:
        current /= part
        if current.is_symlink():
            raise BundleError(f"resource path contains a symbolic link: {relative_path}")
    try:
        mode = current.stat().st_mode
    except FileNotFoundError as error:
        raise BundleError(f"resource is missing: {relative_path}") from error
    if not stat.S_ISREG(mode):
        raise BundleError(f"resource is not a regular file: {relative_path}")
    return current


def validate_files(root: Path, entries: list[tuple[str, PurePosixPath]], exact: bool) -> tuple[int, str]:
    expected_paths = {path.as_posix() for _, path in entries}
    if exact:
        actual_paths = {
            path.relative_to(root).as_posix()
            for path in root.rglob("*")
            if path.is_file() or path.is_symlink()
        }
        if actual_paths != expected_paths:
            missing = sorted(expected_paths - actual_paths)
            extra = sorted(actual_paths - expected_paths)
            raise BundleError(f"bundle contents differ from manifest; missing={missing}, extra={extra}")

    total_bytes = 0
    bundle_digest = hashlib.sha256()
    for expected_hash, relative_path in entries:
        source = ensure_regular_source(root, relative_path)
        size = source.stat().st_size
        if size > MAX_FILE_BYTES or total_bytes > MAX_BUNDLE_BYTES - size:
            raise BundleError(f"resource bundle exceeds its size limit at {relative_path}")
        actual_hash = sha256(source)
        if actual_hash != expected_hash:
            raise BundleError(f"resource hash mismatch: {relative_path}")
        total_bytes += size
        bundle_digest.update(relative_path.as_posix().encode("ascii"))
        bundle_digest.update(b"\0")
        bundle_digest.update(bytes.fromhex(actual_hash))
    return total_bytes, bundle_digest.hexdigest()


def build_bundle(source_root: Path, output_root: Path, entries: list[tuple[str, PurePosixPath]]) -> None:
    ensure_repository_root(source_root)
    ensure_tracked(source_root, entries)
    validate_files(source_root, entries, exact=False)
    if output_root.exists() or output_root.is_symlink():
        raise BundleError("output path already exists")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = Path(tempfile.mkdtemp(prefix=f".{output_root.name}.", dir=output_root.parent))
    try:
        for _, relative_path in entries:
            source = source_root.joinpath(*relative_path.parts)
            destination = temporary_root.joinpath(*relative_path.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            destination.chmod(0o644)
            os.utime(destination, (NORMALIZED_MTIME, NORMALIZED_MTIME), follow_symlinks=False)
        validate_files(temporary_root, entries, exact=True)
        temporary_root.rename(output_root)
    except Exception:
        shutil.rmtree(temporary_root, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build or verify the minimal public Jak 1 FR3 project-resource bundle."
    )
    parser.add_argument("output", type=Path, help="new bundle directory, or existing bundle with --verify-only")
    parser.add_argument(
        "--source-root",
        type=Path,
        default=REPOSITORY_ROOT,
        help="OpenGOAL checkout root used as the resource source",
    )
    parser.add_argument("--verify-only", action="store_true", help="verify an existing bundle")
    arguments = parser.parse_args()

    try:
        entries = read_manifest()
        output_root = arguments.output.expanduser().resolve()
        if arguments.verify_only:
            if not output_root.is_dir() or output_root.is_symlink():
                raise BundleError("bundle root is missing, not a directory, or a symbolic link")
        else:
            build_bundle(arguments.source_root.expanduser().resolve(), output_root, entries)
        total_bytes, digest = validate_files(output_root, entries, exact=True)
        print(f"verified {len(entries)} files, {total_bytes} bytes, bundle sha256 {digest}")
        return 0
    except (BundleError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
