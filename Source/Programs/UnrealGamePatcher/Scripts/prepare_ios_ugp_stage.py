#!/usr/bin/env python3
"""Prepare unsigned iOS content for UGP and a separate stage with no cooked data."""

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def prepare(stage: Path, unreal_pak: Path, output: Path) -> None:
    cooked = stage / "cookeddata"
    pak_dirs = list(cooked.glob("*/content/paks"))
    if len(pak_dirs) != 1:
        raise ValueError(f"Expected one project Pak directory in {cooked}")
    paks = pak_dirs[0]
    base_pak = paks / "pakchunk0-ios.pak"
    if not base_pak.is_file() or base_pak.with_suffix(".sig").exists():
        raise ValueError("An unsigned pakchunk0-ios.pak is required")

    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError(f"Output directory is not empty: {output}")

    with tempfile.TemporaryDirectory(prefix=".prepare-ios-ugp-", dir=output) as work_dir:
        work = Path(work_dir)
        extracted = work / "base"
        extracted.mkdir()
        subprocess.run([unreal_pak, base_pak, "-Extract", extracted], check=True)
        entries = {
            path.relative_to(extracted).as_posix(): path
            for path in extracted.rglob("*") if path.is_file()
        }
        if not entries:
            raise ValueError(f"No files extracted from {base_pak}")
        base_names = {name.casefold() for name in entries}
        for path in cooked.rglob("*"):
            if not path.is_file() or paks in path.parents:
                continue
            name = path.relative_to(cooked).as_posix()
            if name.casefold() in base_names:
                raise ValueError(f"Loose staged file overlaps the base Pak: {name}")
            entries[name] = path

        content = work / "Content"
        shutil.copytree(paks, content)
        (content / base_pak.name).unlink()
        response = work / "pak-response.txt"
        response.write_text(
            "".join(f'"{source}" "../../../{name}" -compress\n' for name, source in sorted(entries.items())),
            encoding="utf-8",
        )
        repacked = content / base_pak.name
        subprocess.run([unreal_pak, repacked, f"-Create={response}", "-compressionformats=Oodle"], check=True)
        subprocess.run([unreal_pak, repacked, "-Verify"], check=True)

        shell = work / "Stage"
        shell.mkdir()
        shutil.copy2(stage / "uecommandline.txt", shell)
        if (stage / "mute.caf").is_file():
            shutil.copy2(stage / "mute.caf", shell)
        content.rename(output / "Content")
        shell.rename(output / "Stage")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--unreal-pak", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.stage.resolve(), args.unreal_pak.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
