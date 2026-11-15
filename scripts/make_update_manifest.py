"""Create the update.json release asset beside the final DXL win64 ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from tempfile import NamedTemporaryFile
from zipfile import ZipFile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("release_notes", type=Path, help="UTF-8 release description")
    args = parser.parse_args()
    name = args.archive.name
    number = r"(?:0|[1-9][0-9]*)"
    match = re.fullmatch(rf"DXL-v({number}(?:\.{number}){{1,3}})-win64\.zip", name)
    if not match:
        parser.error("Expected DXL-v<version>-win64.zip")
    version = match[1]
    tag = "v" + version
    with args.archive.open("rb") as package:
        with ZipFile(package) as archive:
            root = "DXL-" + tag + "/"
            manifest = json.loads(archive.read(root + "PACKAGE_MANIFEST.json").decode("utf-8-sig"))
            if manifest.get("Version") != version:
                parser.error("Package version differs from the ZIP filename")
            if not archive.getinfo(root + "DXL.exe").file_size:
                parser.error("Empty DXL executable")
        package.seek(0)
        digest = hashlib.sha256()
        for block in iter(lambda: package.read(1024 * 1024), b""):
            digest.update(block)
        size = package.tell()

    release = {
        "version": version,
        "tag": tag,
        "name": name,
        "url": f"https://github.com/LCPD15/DXL/releases/download/{tag}/{name}",
        "size": size,
        "sha256": digest.hexdigest(),
        "body": args.release_notes.read_bytes().decode("utf-8-sig"),
    }
    output = args.archive.with_name("update.json")
    tmp = NamedTemporaryFile(dir=output.parent, delete=False)
    temporary = Path(tmp.name)
    try:
        with tmp:
            tmp.write((json.dumps(release, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"{output} ({name}, {size} bytes)")


if __name__ == "__main__":
    main()
