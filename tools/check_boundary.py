#!/usr/bin/env python3
"""Boundary lint: the slicer's source and public docs must not name private products.

The slicer talks to other programs only over documented, product-neutral interfaces
(WEB_APP_BRIDGE.md, SLICE_SERVICE_PROTOCOL.md, ACCOUNT_PROVIDER_PROTOCOL.md; see BOUNDARY.md).
This check keeps it that way. It fails (exit 1, one `file:line` per hit) when one of its rules
(RULES below, plus the private product names in tools/boundary-private-rules.txt where that file
exists) matches:

  * in `src/**` C/C++ sources -- strings, identifiers and comments alike;
  * in the public Markdown: `*.md` at the repository root and `docs/**/*.md`.

Paths listed in `.public-exclude` are skipped (they never reach the public tree), and so is
anything the dev-only markers strip (the dev-edition preprocessor switch and line blocks) when
`tools/export_public.py` is present to do the stripping. Justified hits (an upstream "Stealth
Mode", a third-party library) go in `tools/boundary-allowlist.txt` as `path-glob:regex` lines.

    python tools/check_boundary.py               # the working tree
    python tools/check_boundary.py --summary     # counts by rule only

`tools/export_public.py` runs the same scan over the exported tree, so an export with a hit fails.
Python 3.8+, standard library only.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ALLOWLIST = "tools/boundary-allowlist.txt"
C_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".m", ".mm"}

PRIVATE_RULES = "tools/boundary-private-rules.txt"

# Rule categories that apply to src/** only (the public Markdown may use them).
CODE_ONLY = {"Agent (name)"}

# (category, compiled regex). The names of private products themselves are not in this public
# file: they are rules in tools/boundary-private-rules.txt, which lives only in the private
# development repository (it is in .public-exclude) and is added to these when present. The rules
# here catch the shapes every such integration leaves behind.
#
# Case matters: "Agent" as a product name is capitalised; upstream code uses lower-case "agent"
# for printer connections and the vendor network plug-in (NetworkAgent), which are not ours.
#
# The Unlayered Agent launched publicly on 2026-10-24 (slicer 0.1.8), so its NAME may appear in the
# public Markdown (CHANGELOG, README, the bridge docs). The slicer's code stays generic: the name is
# still a hit in src/ (CODE_ONLY below), and Agent-specific identifiers are a hit everywhere.
RULES = [
    ("Agent", re.compile(r"\bAgent[A-Z_]\w*\b|\bagent_(?:app|sidebar|panel|view|edition)\w*"
                         r"|\bAGENT_(?:APP|EDITION|SIDEBAR)\w*|\bm_agent_(?:view|panel|sidebar|sash)\w*"
                         r"|\bWebAppSlot::agent\b|\bagent_app\b")),
    ("Agent (name)", re.compile(r"\bAgent\b|(?i:\bunlayered[-_ ]?agent\b)")),
    # The boundary contract: entitlement is the service's job, never the slicer's.
    ("client entitlement", re.compile(r"\bProGate\b|\bpro_confirmed\b|\bensure_pro\b|\bpro_known\b|\bWAB_PRO_REQUIRED\b")),
]


def parse_rules(text: str, source: str = PRIVATE_RULES) -> list[tuple[str, re.Pattern]]:
    """`Category: regex` per line; # comments."""
    out = []
    for n, raw in enumerate(text.split(chr(10)), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cat, sep, rx = line.partition(":")
        if not sep or not rx.strip():
            raise SystemExit(f"{source}:{n}: want 'Category: regex', got {raw!r}")
        out.append((cat.strip(), re.compile(rx.strip())))
    return out


def load_rules(root: Path) -> list[tuple[str, re.Pattern]]:
    p = root / PRIVATE_RULES
    return parse_rules(p.read_text(encoding="utf-8")) + RULES if p.exists() else list(RULES)


def in_scope(path: str) -> bool:
    if path.startswith("src/"):
        return Path(path).suffix.lower() in C_EXTS
    if path.lower().endswith(".md"):
        return "/" not in path or path.startswith("docs/")
    return False


# ------------------------------------------------------------------------------------------------
# .public-exclude and the dev-only strip (shared with export_public.py when it is present)
# ------------------------------------------------------------------------------------------------

def load_excludes(root: Path) -> list[str]:
    p = root / ".public-exclude"
    if not p.exists():
        return []
    return [ln.strip() for ln in p.read_text(encoding="utf-8").splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")]


def excluded(path: str, patterns: list[str]) -> bool:
    for pat in patterns:
        if pat.endswith("/"):
            if path.startswith(pat) or ("/" + pat) in ("/" + path):
                return True
        elif "/" in pat:
            if fnmatch.fnmatchcase(path, pat):
                return True
        elif fnmatch.fnmatchcase(path.rsplit("/", 1)[-1], pat):
            return True
    return False


def dev_strip():
    """export_public.transform (dev repository only), else the identity: the public tree has
    nothing left to strip."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        from export_public import transform  # type: ignore
        return transform
    except Exception:
        return lambda path, data: data
    finally:
        sys.path.pop(0)


def parse_allowlist(text: str, source: str = ALLOWLIST) -> list[tuple[str, re.Pattern]]:
    out = []
    for n, raw in enumerate(text.split(chr(10)), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        glob, sep, rx = line.partition(":")
        if not sep or not rx:
            raise SystemExit(f"{source}:{n}: want 'path-glob:regex', got {raw!r}")
        out.append((glob.strip(), re.compile(rx.strip())))
    return out


def load_allowlist(root: Path) -> list[tuple[str, re.Pattern]]:
    p = root / ALLOWLIST
    return parse_allowlist(p.read_text(encoding="utf-8")) if p.exists() else []


# ------------------------------------------------------------------------------------------------
# scanning
# ------------------------------------------------------------------------------------------------

def scan_entries(entries, allow=None, root: Path = REPO, rules=None) -> list[tuple[str, int, str, str]]:
    """entries: iterable of (posix path, bytes) as they will be published. Returns
    [(path, line, category, text)] for every hit not covered by the allow-list."""
    if allow is None:
        allow = load_allowlist(root)
    if rules is None:
        rules = load_rules(root)
    hits = []
    for path, data in entries:
        if not in_scope(path) or data is None or b"\0" in data[:8192]:
            continue
        text = data.decode("utf-8", errors="replace")
        for n, line in enumerate(text.split(chr(10)), 1):  # not splitlines(): it also splits on form feeds
            for category, rx in rules:
                if category in CODE_ONLY and not path.startswith("src/"):
                    continue
                if not rx.search(line):
                    continue
                if any(fnmatch.fnmatchcase(path, g) and a.search(line) for g, a in allow):
                    continue
                hits.append((path, n, category, line.strip()[:160]))
                break
    return hits


def format_hits(hits) -> list[str]:
    return [f"{p}:{n}: [{c}] {t}" for p, n, c, t in hits]


def worktree_entries(root: Path):
    proc = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], stdout=subprocess.PIPE)
    if proc.returncode != 0:
        raise SystemExit(f"{root} is not a git working tree")
    excludes = load_excludes(root)
    transform = dev_strip()
    for raw in proc.stdout.split(b"\0"):
        if not raw:
            continue
        path = raw.decode("utf-8")
        if not in_scope(path) or excluded(path, excludes):
            continue
        fp = root / path
        if not fp.is_file():
            continue
        data = fp.read_bytes().replace(b"\r\n", b"\n")
        try:
            data = transform(path, data)
        except Exception as e:  # a malformed dev marker is export_public's error to report
            print(f"warning: {path}: dev-only strip failed ({e}); scanned as is", file=sys.stderr)
        yield path, data


def summary(hits) -> str:
    order = list(dict.fromkeys(c for _, _, c, _ in hits))
    counts: dict[str, int] = {}
    files: dict[str, set] = {}
    for p, _, c, _ in hits:
        counts[c] = counts.get(c, 0) + 1
        files.setdefault(c, set()).add(p)
    lines = [f"  {c:<20} {counts[c]:>5} hit(s) in {len(files[c])} file(s)" for c in order]
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(REPO), help="repository root (default: this checkout)")
    ap.add_argument("--summary", action="store_true", help="print only the counts by rule")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    hits = scan_entries(worktree_entries(root), load_allowlist(root), root, load_rules(root))
    if not hits:
        print("boundary check: clean")
        return
    if not args.summary:
        for h in format_hits(hits):
            print(h)
    print(f"\nBOUNDARY CHECK FAILED -- {len(hits)} hit(s):\n{summary(hits)}", file=sys.stderr)
    raise SystemExit(1)


if __name__ == "__main__":
    main()
