"""Export allowlisted source without old Git history or binary dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
sys.dont_write_bytecode = True
from audit_source import audit

ROOT_FILES = {".gitignore", "README.md", "README_ZH.md", "LICENSE", "CHANGELOG.md", "DEPENDENCIES.md",
              "THIRD_PARTY_NOTICES.md", "LICENSE_STATUS.md", "NRFG_OPTICAL_FLOW_NOTICES.txt", "SOURCE_CODE.md"}
ROOT_DIRS = {"src", "scripts", "tests", "docs", "licenses", "lut", "post-processing"}
VENDORS = {"imgui", "minhook", "fidelityfx", "pix", "tensorrt", "cuda-stub", "reshade"}
TEXT_SUFFIXES = {".h", ".hpp", ".cpp", ".c", ".hlsl", ".fx", ".fxh", ".js", ".html", ".css", ".ps1", ".psm1", ".cmd",
                 ".py", ".md", ".txt", ".rc", ".asm", ".def", ".json"}
ART = {"icon/icon.png", "src/ui/app.ico", "src/ui/web/app-icon.png", "src/ui/web/app-icon@2x.png", "lut/Neutral-16.png"}
GUIDES = {"docs/DXL-Guide-ZH.docx", "docs/DXL-Guide-EN.docx", "docs/images/dxl-interface.png"}
DEVELOPMENT_NOTES = {"docs/FRAME_GENERATION_FEASIBILITY.md", "docs/GRAPHICS_COMPAT_TESTS.md",
                     "docs/RDR2_PRESENT_RECURSION.md", "docs/STARTUP_DIAGNOSTICS.md",
                     "docs/VULKAN_DLSS_INVESTIGATION.md", "docs/PRESENTATION_INITIALIZATION.md"}

def selected(rel):
    parts = rel.parts
    if rel.as_posix() in DEVELOPMENT_NOTES:
        return False
    if len(parts) == 1:
        return rel.name in ROOT_FILES
    if rel.as_posix() in ART or rel.as_posix() in GUIDES:
        return True
    if rel.suffix.lower() not in TEXT_SUFFIXES:
        return False
    return parts[0] in ROOT_DIRS or (parts[0] == "third_party" and len(parts) > 2 and parts[1] in VENDORS)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    dest = args.out.resolve()
    if dest == root or root in dest.parents or dest in root.parents:
        raise SystemExit("Export must be separate from source.")
    if dest.exists() and any(dest.iterdir()):
        raise SystemExit("Export destination must be new or empty.")
    count, problems = audit(root)
    if problems:
        raise SystemExit("\n".join(problems))
    dest.mkdir(parents=True, exist_ok=True)
    records = []
    for p in sorted(root.rglob("*")):
        if not p.is_file() or ".git" in p.relative_to(root).parts:
            continue
        rel = p.relative_to(root)
        if not selected(rel):
            continue
        to = dest / rel
        to.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, to)
        records.append({"path":rel.as_posix(),"sha256":hashlib.sha256(to.read_bytes()).hexdigest()})
    (dest/"SOURCE_MANIFEST.json").write_text(json.dumps({"version":"0.6","git_history_included":False,
        "runtime_or_model_binaries_included":False,"files":records},indent=2),encoding="utf-8")
    count, problems = audit(dest)
    if problems:
        raise SystemExit("\n".join(problems))
    print(f"PASS clean source snapshot: {count} files; no old Git history, runtime DLLs or models.")

if __name__ == "__main__":
    main()
