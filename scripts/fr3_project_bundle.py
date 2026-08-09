#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_LINE = re.compile(r"([0-9a-f]{64})  ([\x21-\x7e]+)")
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


@dataclass(frozen=True)
class GameProfile:
    game: str
    display_name: str
    config_version: str
    manifest_path: PurePosixPath
    config_path: PurePosixPath
    allowed_prefixes: tuple[str, ...]
    allowed_exact_paths: frozenset[str]
    additional_resource_paths: frozenset[str] = frozenset()


PROFILES = {
    "jak1": GameProfile(
        game="jak1",
        display_name="Jak 1",
        config_version="ntsc_v1",
        manifest_path=PurePosixPath("decompiler/config/jak1/fr3-project-resources.sha256"),
        config_path=PurePosixPath("decompiler/config/jak1/jak1_config.jsonc"),
        allowed_prefixes=(
            "decompiler/config/jak1/",
            "game/assets/jak1/patches/",
        ),
        allowed_exact_paths=frozenset({"goal_src/jak1/build/all_objs.json"}),
    ),
    "jak2": GameProfile(
        game="jak2",
        display_name="Jak II",
        config_version="ntsc_v1",
        manifest_path=PurePosixPath("decompiler/config/jak2/fr3-project-resources.sha256"),
        config_path=PurePosixPath("decompiler/config/jak2/jak2_config.jsonc"),
        allowed_prefixes=("decompiler/config/jak2/",),
        allowed_exact_paths=frozenset(
            {
                "game/assets/fonts/jak2_jak3_korean_db.json",
                "game/assets/jak2/game_subtitle.gp",
                "game/assets/jak2/game_text.gp",
                "game/assets/jak2/subtitle/subtitle_lines_en-US.json",
                "game/assets/jak2/subtitle/subtitle_meta_en-US.json",
                "game/assets/jak2/text/game_custom_text_de-DE.json",
                "game/assets/jak2/text/game_custom_text_en-GB.json",
                "game/assets/jak2/text/game_custom_text_en-US.json",
                "game/assets/jak2/text/game_custom_text_es-ES.json",
                "game/assets/jak2/text/game_custom_text_fr-FR.json",
                "game/assets/jak2/text/game_custom_text_it-IT.json",
                "game/assets/jak2/text/game_custom_text_ja-JP.json",
                "game/assets/jak2/text/game_custom_text_ko-KR.json",
            }
        ),
        additional_resource_paths=frozenset(
            {
                "game/assets/fonts/jak2_jak3_korean_db.json",
                "game/assets/jak2/game_subtitle.gp",
                "game/assets/jak2/game_text.gp",
                "game/assets/jak2/subtitle/subtitle_lines_en-US.json",
                "game/assets/jak2/subtitle/subtitle_meta_en-US.json",
                "game/assets/jak2/text/game_custom_text_de-DE.json",
                "game/assets/jak2/text/game_custom_text_en-GB.json",
                "game/assets/jak2/text/game_custom_text_en-US.json",
                "game/assets/jak2/text/game_custom_text_es-ES.json",
                "game/assets/jak2/text/game_custom_text_fr-FR.json",
                "game/assets/jak2/text/game_custom_text_it-IT.json",
                "game/assets/jak2/text/game_custom_text_ja-JP.json",
                "game/assets/jak2/text/game_custom_text_ko-KR.json",
            }
        ),
    ),
}


class BundleError(Exception):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative_path(raw_path: str, label: str) -> PurePosixPath:
    relative_path = PurePosixPath(raw_path)
    parts = relative_path.parts
    if relative_path.is_absolute() or not parts or any(part in {"", ".", ".."} for part in parts):
        raise BundleError(f"unsafe {label}: {raw_path}")
    if "\\" in raw_path or any(part in FORBIDDEN_COMPONENTS for part in parts):
        raise BundleError(f"forbidden {label}: {raw_path}")
    return relative_path


def read_manifest(
    profile: GameProfile, manifest_root: Path = REPOSITORY_ROOT
) -> list[tuple[str, PurePosixPath]]:
    manifest_path = manifest_root.joinpath(*profile.manifest_path.parts)
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise BundleError("resource manifest is missing, linked, or not a regular file")
    entries: list[tuple[str, PurePosixPath]] = []
    for line_number, raw_line in enumerate(
        manifest_path.read_text(encoding="ascii").splitlines(), 1
    ):
        if not raw_line or raw_line.startswith("#"):
            continue
        match = MANIFEST_LINE.fullmatch(raw_line)
        if not match:
            raise BundleError(f"invalid manifest line {line_number}")
        expected_hash, raw_path = match.groups()
        relative_path = safe_relative_path(raw_path, "manifest path")
        if relative_path.suffix.lower() in FORBIDDEN_SUFFIXES:
            raise BundleError(f"retail or generated file type is forbidden: {raw_path}")
        if raw_path not in profile.allowed_exact_paths and not raw_path.startswith(
            profile.allowed_prefixes
        ):
            raise BundleError(f"path is outside the FR3 project-resource allowlist: {raw_path}")
        entries.append((expected_hash, relative_path))

    paths = [path.as_posix() for _, path in entries]
    if not entries or paths != sorted(paths) or len(paths) != len(set(paths)):
        raise BundleError("manifest paths must be nonempty, unique, and sorted")
    return entries


def strip_jsonc(text: str) -> str:
    output: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(text):
        character = text[index]
        if in_string:
            output.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            index += 1
            continue
        if character == '"':
            in_string = True
            output.append(character)
            index += 1
            continue
        if character == "/" and index + 1 < len(text) and text[index + 1] == "/":
            index += 2
            while index < len(text) and text[index] not in "\r\n":
                index += 1
            continue
        if character == "/" and index + 1 < len(text) and text[index + 1] == "*":
            end = text.find("*/", index + 2)
            if end < 0:
                raise BundleError("unterminated block comment in decompiler configuration")
            index = end + 2
            continue
        output.append(character)
        index += 1

    without_comments = "".join(output)
    output = []
    index = 0
    in_string = False
    escaped = False
    while index < len(without_comments):
        character = without_comments[index]
        if in_string:
            output.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            index += 1
            continue
        if character == '"':
            in_string = True
            output.append(character)
            index += 1
            continue
        if character == ",":
            lookahead = index + 1
            while lookahead < len(without_comments) and without_comments[lookahead].isspace():
                lookahead += 1
            if lookahead < len(without_comments) and without_comments[lookahead] in "}]":
                index += 1
                continue
        output.append(character)
        index += 1
    return "".join(output)


def read_effective_config(root: Path, profile: GameProfile) -> dict:
    config_file = ensure_regular_source(root, profile.config_path)
    try:
        config = json.loads(strip_jsonc(config_file.read_text(encoding="utf-8")))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise BundleError(f"invalid {profile.display_name} decompiler configuration") from error
    if not isinstance(config, dict):
        raise BundleError(f"invalid {profile.display_name} decompiler configuration root")
    overrides = config.get("version_overrides")
    if not isinstance(overrides, dict) or not isinstance(overrides.get(profile.config_version), dict):
        raise BundleError(
            f"the {profile.display_name} decompiler configuration lacks {profile.config_version}"
        )
    effective = dict(config)
    effective.update(overrides[profile.config_version])
    return effective


def referenced_config_paths(config: dict) -> set[PurePosixPath]:
    required_keys = {
        "all_types_file",
        "anonymous_function_types_file",
        "art_info_file",
        "hacks_file",
        "import_deps_file",
        "inputs_file",
        "label_types_file",
        "process_stack_size_file",
        "stack_structures_file",
        "type_casts_file",
    }
    optional_keys = {
        "anonymous_function_types_merge_file",
        "art_group_dump_file",
        "hacks_merge_file",
        "joint_node_dump_file",
        "label_types_merge_file",
        "obj_file_name_map_file",
        "part_group_table_dump_file",
        "stack_structures_merge_file",
        "tex_dump_file",
        "type_casts_merge_file",
    }
    if not config.get("ignore_var_name_casts", False):
        required_keys.add("var_names_file")

    paths: set[PurePosixPath] = set()
    for key in required_keys:
        value = config.get(key)
        if not isinstance(value, str):
            raise BundleError(f"decompiler configuration has no string {key}")
        paths.add(safe_relative_path(value, f"decompiler path for {key}"))
    for key in optional_keys:
        if key not in config:
            continue
        value = config[key]
        if not isinstance(value, str):
            raise BundleError(f"decompiler configuration has a non-string {key}")
        paths.add(safe_relative_path(value, f"decompiler path for {key}"))

    object_patches = config.get("object_patches")
    if not isinstance(object_patches, dict):
        raise BundleError("decompiler configuration has invalid object_patches")
    for patch in object_patches.values():
        if not isinstance(patch, dict) or not isinstance(patch.get("out"), str):
            raise BundleError("decompiler configuration has an invalid object patch")
        paths.add(safe_relative_path(patch["out"], "object patch path"))
    return paths


def conventional_texture_merge_paths(root: Path, profile: GameProfile) -> set[PurePosixPath]:
    relative_root = PurePosixPath("game") / "assets" / profile.game / "texture_merges"
    texture_root = root.joinpath(*relative_root.parts)
    if not os.path.lexists(texture_root):
        return set()
    if texture_root.is_symlink() or not texture_root.is_dir():
        raise BundleError(f"{profile.display_name} texture_merges is linked or not a directory")
    paths: set[PurePosixPath] = set()
    for directory, directory_names, file_names in os.walk(texture_root, followlinks=False):
        directory_path = Path(directory)
        for name in directory_names:
            child = directory_path / name
            if child.is_symlink():
                raise BundleError("texture_merges contains a symbolic link")
        for name in file_names:
            child = directory_path / name
            if child.is_symlink() or not child.is_file():
                raise BundleError("texture_merges contains an unsupported file")
            paths.add(PurePosixPath(child.relative_to(root).as_posix()))
    return paths


def validate_resource_closure(
    root: Path, profile: GameProfile, entries: list[tuple[str, PurePosixPath]]
) -> None:
    expected = {path for _, path in entries}
    derived = {profile.config_path}
    derived.update(referenced_config_paths(read_effective_config(root, profile)))
    derived.update(conventional_texture_merge_paths(root, profile))
    derived.update(PurePosixPath(path) for path in profile.additional_resource_paths)
    if derived != expected:
        missing = sorted(path.as_posix() for path in derived - expected)
        extra = sorted(path.as_posix() for path in expected - derived)
        raise BundleError(
            f"manifest differs from the {profile.display_name} import resource closure; "
            f"missing={missing}, extra={extra}"
        )


def ensure_no_symlink_components(path: Path, label: str, require_final: bool) -> None:
    if not path.is_absolute():
        raise BundleError(f"{label} must be absolute")
    current = Path(path.anchor)
    for index, part in enumerate(path.parts[1:]):
        current /= part
        try:
            status = os.lstat(current)
        except FileNotFoundError:
            if require_final or index != len(path.parts[1:]) - 1:
                raise BundleError(f"{label} is missing: {current}")
            return
        if stat.S_ISLNK(status.st_mode):
            raise BundleError(f"{label} contains a symbolic link: {current}")


def ensure_repository_root(source_root: Path) -> None:
    ensure_no_symlink_components(source_root, "source root", True)
    result = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "--show-toplevel"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode or Path(result.stdout.strip()).resolve() != source_root.resolve():
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


def ensure_regular_source(root: Path, relative_path: PurePosixPath) -> Path:
    current = root
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


def validate_files(
    root: Path, entries: list[tuple[str, PurePosixPath]], exact: bool
) -> tuple[int, str]:
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


def copy_checked_file(
    source_root: Path,
    destination_root: Path,
    expected_hash: str,
    relative_path: PurePosixPath,
) -> None:
    source = ensure_regular_source(source_root, relative_path)
    destination = destination_root.joinpath(*relative_path.parts)
    destination.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(source, flags)
    try:
        status = os.fstat(descriptor)
        if not stat.S_ISREG(status.st_mode) or status.st_size > MAX_FILE_BYTES:
            raise BundleError(f"resource changed or exceeds its size limit: {relative_path}")
        digest = hashlib.sha256()
        with os.fdopen(os.dup(descriptor), "rb") as opened:
            while chunk := opened.read(1024 * 1024):
                digest.update(chunk)
        if digest.hexdigest() != expected_hash:
            raise BundleError(f"resource changed while staging: {relative_path}")
        os.lseek(descriptor, 0, os.SEEK_SET)
        destination_descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            while chunk := os.read(descriptor, 1024 * 1024):
                remaining = memoryview(chunk)
                while remaining:
                    written = os.write(destination_descriptor, remaining)
                    if written <= 0:
                        raise BundleError(f"could not copy resource: {relative_path}")
                    remaining = remaining[written:]
        finally:
            os.close(destination_descriptor)
    finally:
        os.close(descriptor)
    destination.chmod(0o644)
    os.utime(destination, (NORMALIZED_MTIME, NORMALIZED_MTIME), follow_symlinks=False)


def build_bundle(
    source_root: Path,
    output_root: Path,
    profile: GameProfile,
    entries: list[tuple[str, PurePosixPath]],
) -> None:
    ensure_repository_root(source_root)
    ensure_tracked(source_root, entries)
    validate_resource_closure(source_root, profile, entries)
    validate_files(source_root, entries, exact=False)
    if os.path.lexists(output_root):
        raise BundleError("output path already exists")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    ensure_no_symlink_components(output_root.parent, "output parent", True)
    temporary_root = Path(tempfile.mkdtemp(prefix=f".{output_root.name}.", dir=output_root.parent))
    try:
        for expected_hash, relative_path in entries:
            copy_checked_file(source_root, temporary_root, expected_hash, relative_path)
        validate_resource_closure(temporary_root, profile, entries)
        validate_files(temporary_root, entries, exact=True)
        temporary_root.rename(output_root)
    except Exception:
        shutil.rmtree(temporary_root, ignore_errors=True)
        raise


def run_for_game(game: str, arguments: list[str] | None = None) -> int:
    profile = PROFILES[game]
    parser = argparse.ArgumentParser(
        description=f"Build or verify the minimal public {profile.display_name} import project-resource bundle."
    )
    parser.add_argument(
        "output", type=Path, help="new bundle directory, or existing bundle with --verify-only"
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=REPOSITORY_ROOT,
        help="OpenGOAL checkout root used as the resource source",
    )
    parser.add_argument("--verify-only", action="store_true", help="verify an existing bundle")
    parsed = parser.parse_args(arguments)

    try:
        entries = read_manifest(profile)
        output_root = Path(os.path.abspath(parsed.output.expanduser()))
        if parsed.verify_only:
            ensure_no_symlink_components(output_root, "bundle root", True)
            if not output_root.is_dir():
                raise BundleError("bundle root is missing or not a directory")
        else:
            source_root = Path(os.path.abspath(parsed.source_root.expanduser()))
            build_bundle(source_root, output_root, profile, entries)
        validate_resource_closure(output_root, profile, entries)
        total_bytes, digest = validate_files(output_root, entries, exact=True)
        print(f"verified {len(entries)} files, {total_bytes} bytes, bundle sha256 {digest}")
        return 0
    except (BundleError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
