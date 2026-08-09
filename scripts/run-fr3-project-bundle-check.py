#!/usr/bin/env python3

import hashlib
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile

from fr3_project_bundle import BundleError, GameProfile, read_manifest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def run(*arguments: str, expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(arguments, check=False, capture_output=True, text=True)
    if result.returncode != expected:
        raise RuntimeError(
            f"unexpected exit {result.returncode}: {' '.join(arguments)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def assert_rejected(*arguments: str) -> None:
    result = subprocess.run(arguments, check=False, capture_output=True, text=True)
    if result.returncode == 0:
        raise RuntimeError(f"unsafe bundle was accepted: {' '.join(arguments)}")


def main() -> int:
    python = sys.executable
    with tempfile.TemporaryDirectory(
        prefix="opengoal-fr3-project-bundle.", dir="/private/tmp"
    ) as temporary:
        temporary_root = Path(temporary)
        bundles: dict[str, Path] = {}
        for game in ("jak1", "jak2"):
            builder = REPOSITORY_ROOT / "scripts" / f"build-{game}-fr3-project-bundle.py"
            bundle = temporary_root / f"{game}-bundle"
            run(python, str(builder), "--source-root", str(REPOSITORY_ROOT), str(bundle))
            run(python, str(builder), str(bundle), "--verify-only")
            expected_paths = {path.as_posix() for _, path in read_manifest_for_game(game)}
            actual_paths = {
                path.relative_to(bundle).as_posix()
                for path in bundle.rglob("*")
                if path.is_file() or path.is_symlink()
            }
            if actual_paths != expected_paths:
                raise RuntimeError(f"{game} bundle does not match its manifest")
            bundles[game] = bundle

        jak2_builder = REPOSITORY_ROOT / "scripts" / "build-jak2-fr3-project-bundle.py"
        jak2_bundle = bundles["jak2"]
        extra = jak2_bundle / "unexpected.json"
        extra.write_text("{}\n", encoding="ascii")
        assert_rejected(python, str(jak2_builder), str(jak2_bundle), "--verify-only")
        extra.unlink()

        config = jak2_bundle / "decompiler/config/jak2/jak2_config.jsonc"
        original_config = config.read_bytes()
        config.write_bytes(original_config + b"\n")
        assert_rejected(python, str(jak2_builder), str(jak2_bundle), "--verify-only")
        config.write_bytes(original_config)

        inputs = jak2_bundle / "decompiler/config/jak2/ntsc_v1/inputs.jsonc"
        original_inputs = inputs.read_bytes()
        inputs.unlink()
        inputs.symlink_to("hacks.jsonc")
        assert_rejected(python, str(jak2_builder), str(jak2_bundle), "--verify-only")
        inputs.unlink()
        inputs.write_bytes(original_inputs)

        linked_bundle = temporary_root / "linked-jak2-bundle"
        linked_bundle.symlink_to(jak2_bundle, target_is_directory=True)
        assert_rejected(python, str(jak2_builder), str(linked_bundle), "--verify-only")

        fixture_root = temporary_root / "source-fixture"
        shutil.copytree(jak2_bundle, fixture_root)
        run("git", "-C", str(fixture_root), "init", "--quiet")
        run("git", "-C", str(fixture_root), "add", ".")
        untracked = fixture_root / "game/assets/jak2/texture_merges/untracked.json"
        untracked.parent.mkdir(parents=True)
        untracked.write_text("{}\n", encoding="ascii")
        assert_rejected(
            python,
            str(jak2_builder),
            "--source-root",
            str(fixture_root),
            str(temporary_root / "untracked-output"),
        )

        manifest_root = temporary_root / "manifest-fixture"
        manifest_path = manifest_root / "manifest.sha256"
        manifest_path.parent.mkdir(parents=True)
        manifest_path.write_text(
            f"{hashlib.sha256(b'fixture').hexdigest()}  decompiler/config/jak2/retail.iso\n",
            encoding="ascii",
        )
        profile = GameProfile(
            game="jak2",
            display_name="Jak II",
            config_version="ntsc_v1",
            manifest_path=PurePosixPath("manifest.sha256"),
            config_path=PurePosixPath("decompiler/config/jak2/jak2_config.jsonc"),
            allowed_prefixes=("decompiler/config/jak2/",),
            allowed_exact_paths=frozenset(),
        )
        try:
            read_manifest(profile, manifest_root)
        except BundleError as error:
            if "retail or generated file type" not in str(error):
                raise
        else:
            raise RuntimeError("retail extension in a resource manifest was accepted")

    print("FR3 project-resource bundle checks passed.")
    return 0


def read_manifest_for_game(game: str):
    from fr3_project_bundle import PROFILES

    return read_manifest(PROFILES[game])


if __name__ == "__main__":
    raise SystemExit(main())
