"""Read-only checks for accidental private/generated files in a source snapshot."""
import argparse
from pathlib import Path
import re

DENIED_SUFFIXES = {".dll", ".exe", ".lib", ".obj", ".pdb", ".map", ".bin", ".pt",
                   ".onnx", ".plan", ".log", ".dmp", ".mdmp", ".zip", ".7z", ".whl", ".pyc"}
DENIED_DIRS = {"build", "runtime", "models", "diagnostics", "screenshots", "captures",
               "profiles", "user-data", "prototypes", ".claude", ".codex", "__pycache__"}
ART = {"icon/icon.png", "src/ui/app.ico", "src/ui/web/app-icon.png", "src/ui/web/app-icon@2x.png",
       "docs/images/dxl-interface.png"}
MEDIA = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".ico", ".mp4", ".ppm"}
PATTERNS = {
    "personal Windows home": re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+(?!Public\b|Default\b)[^\\/\s\"']+", re.I),
    "machine name": re.compile(r"\b(?:DESKTOP|LAPTOP)-[A-Z0-9]{4,}\b", re.I),
    "private IPv4": re.compile(r"\b(?:192\.168|10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b"),
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "GitHub credential": re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,})\b"),
}
TEXT = {".h", ".cpp", ".c", ".hlsl", ".js", ".html", ".css", ".ps1", ".cmd",
        ".py", ".md", ".txt", ".rc", ".asm", ".def", ".json"}

def audit(root):
    problems = []
    count = 0
    for p in root.rglob("*"):
        rel = p.relative_to(root)
        if ".git" in rel.parts:
            continue  # Git history is never copied by export_source.py.
        if not p.is_file():
            continue
        count += 1
        if (p.is_symlink() or p.suffix.lower() in DENIED_SUFFIXES or DENIED_DIRS.intersection(rel.parts)
                or (p.suffix.lower() in MEDIA and rel.as_posix() not in ART)):
            problems.append(f"{rel}: non-source/development asset")
            continue
        if p.name in {"settings.json", "launcher.json", "window.json"}:
            problems.append(f"{rel}: personal settings")
        if p.suffix.lower() not in TEXT:
            continue
        text = p.read_text(encoding="utf-8-sig")
        for label, pattern in PATTERNS.items():
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                problems.append(f"{rel}:{line}: {label} [value redacted]")
    return count, problems

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    count, problems = audit(args.root.resolve())
    for problem in problems:
        print(problem)
    print(f"{'FAIL' if problems else 'PASS'} source audit: {count} files, {len(problems)} findings. Git history is not audited/exported.")
    return 1 if problems else 0

if __name__ == "__main__":
    raise SystemExit(main())
