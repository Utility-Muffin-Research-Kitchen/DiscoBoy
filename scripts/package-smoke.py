#!/usr/bin/env python3
import argparse
import hashlib
import json
import pathlib
import stat
import zipfile


def fail(message: str) -> None:
    raise SystemExit(f"package-smoke: {message}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate the Disco Boy MLP1 package")
    parser.add_argument("--package", required=True)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    package = pathlib.Path(args.package)
    archive = pathlib.Path(args.archive)
    manifest_path = package / "pak.json"
    binary_path = package / "bin" / "discoboy"
    launch_path = package / "launch.sh"

    if not package.is_dir() or package.name != "DiscoBoy.pak":
        fail("expected one DiscoBoy.pak package directory")
    if not manifest_path.is_file() or not binary_path.is_file() or not launch_path.is_file():
        fail("package is missing pak.json, launch.sh, or bin/discoboy")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("pak_version") != args.version:
        fail(f"manifest version {manifest.get('pak_version')!r} != {args.version!r}")
    if manifest.get("platform") != "mlp1":
        fail("manifest platform must be mlp1")
    if binary_path.read_bytes()[:4] != b"\x7fELF":
        fail("discoboy is not an ELF binary")
    if not archive.is_file():
        fail("archive is missing")

    installed_size = 0
    with zipfile.ZipFile(archive) as source:
        names = source.namelist()
        if len(names) != len(set(names)):
            fail("archive contains duplicate paths")
        expected_files = {
            f"{package.name}/{path.relative_to(package).as_posix()}"
            for path in package.rglob("*")
            if path.is_file()
        }
        if set(names) != expected_files:
            fail("archive file set does not match the package")
        for info in source.infolist():
            path = pathlib.PurePosixPath(info.filename)
            if path.is_absolute() or ".." in path.parts or path.parts[0] != package.name:
                fail(f"unsafe archive path: {info.filename}")
            mode = info.external_attr >> 16
            if not stat.S_ISREG(mode):
                fail(f"non-regular archive member: {info.filename}")
            wanted = 0o755 if (
                info.filename.endswith("/launch.sh")
                or info.filename.endswith("/bin/discoboy")
            ) else 0o644
            if stat.S_IMODE(mode) != wanted:
                fail(f"wrong mode for {info.filename}: {oct(stat.S_IMODE(mode))}")
            installed_size += info.file_size
        archive_manifest = json.loads(source.read(f"{package.name}/pak.json"))
        if archive_manifest != manifest:
            fail("archive manifest differs from package manifest")

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(
        f"package-smoke: PASS version={args.version} "
        f"size={archive.stat().st_size} installed={installed_size} sha256={digest}"
    )


if __name__ == "__main__":
    main()
