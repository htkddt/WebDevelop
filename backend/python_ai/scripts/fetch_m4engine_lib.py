#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Download an engine package (``.tar.gz`` or ``.zip``) from GitHub (or any HTTPS URL) and copy
``libm4engine.dylib`` / ``libm4engine.so`` into ``python_ai/lib/``.

Why this exists: pip cannot install the shared library; many engine repos are **private** (API returns 404
without a token), so you must either paste a **release asset URL** or set ``M4_GITHUB_TOKEN`` + repo/tag.

Usage
-----
  export M4_ENGINE_DOWNLOAD_URL='https://github.com/OWNER/REPO/releases/download/vX.Y/m4engine-darwin-arm64.tar.gz'
  python3 scripts/fetch_m4engine_lib.py

  python3 scripts/fetch_m4engine_lib.py --url 'https://...'

Optional (private repo — pick asset by tag):
  export M4_GITHUB_TOKEN=ghp_...
  export M4_ENGINE_GITHUB_REPO=ngoky/lib_c
  export M4_ENGINE_GITHUB_TAG=v1.0.0-beta.1
  python3 scripts/fetch_m4engine_lib.py

Requires: stdlib only (no pip deps).
"""
import argparse
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
LIB_DIR = ROOT / "lib"

LIB_DYLIB = "libm4engine.dylib"
LIB_SO = "libm4engine.so"


def _want_name() -> str:
    return LIB_DYLIB if sys.platform == "darwin" else LIB_SO


def _gh_cli_token() -> str:
    """Try to get a token from ``gh auth token`` (GitHub CLI)."""
    import subprocess
    try:
        result = subprocess.run(
            ["gh", "auth", "token"], capture_output=True, text=True, timeout=5,
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return ""


_cached_token: Optional[str] = None


def _resolve_token() -> str:
    """Resolve GitHub token: M4_GITHUB_TOKEN env var → gh CLI → empty. Cached after first call."""
    global _cached_token
    if _cached_token is not None:
        return _cached_token
    tok = (os.environ.get("M4_GITHUB_TOKEN") or "").strip()
    if not tok:
        tok = _gh_cli_token()
        if tok:
            print("Using token from gh CLI (gh auth token)", file=sys.stderr)
    _cached_token = tok
    return tok


def _request(url: str) -> urllib.request.Request:
    req = urllib.request.Request(url, headers={"User-Agent": "m4-fetch-m4engine-lib/1"})
    tok = _resolve_token()
    if tok and "github.com" in url:
        req.add_header("Authorization", f"Bearer {tok}")
        if "/releases/assets/" in url:
            req.add_header("Accept", "application/octet-stream")
    return req


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(_request(url), timeout=300) as resp:
        data = resp.read()
    dest.write_bytes(data)


def _find_member(names: list, want: str) -> Optional[str]:
    w = want.lower()
    best = None
    for n in names:
        if n.rstrip("/").split("/")[-1].lower() == w:
            if best is None or len(n) < len(best):
                best = n
    return best


def _is_tar_gz(path: Path) -> bool:
    n = path.name.lower()
    return n.endswith(".tar.gz") or n.endswith(".tgz")


def _tar_extract_member(tf: tarfile.TarFile, member: str, dest: Path) -> None:
    try:
        tf.extract(member, dest, filter="data")
    except TypeError:
        tf.extract(member, dest)


def _extract_and_copy(archive: Path, want: str) -> Path:
    ext = archive.suffix.lower()
    tmp_extract = Path(tempfile.mkdtemp(prefix="m4engine_extract_"))
    try:
        if _is_tar_gz(archive):
            with tarfile.open(archive, "r:*") as tf:
                members = tf.getnames()
                m = _find_member(members, want)
                if not m:
                    raise FileNotFoundError(
                        f"No {want} in archive; members sample: {members[:20]}"
                    )
                _tar_extract_member(tf, m, tmp_extract)
                src = tmp_extract / m
        elif ext == ".zip":
            with zipfile.ZipFile(archive, "r") as zf:
                members = zf.namelist()
                m = _find_member(members, want)
                if not m:
                    raise FileNotFoundError(
                        f"No {want} in archive; members sample: {members[:20]}"
                    )
                zf.extract(m, tmp_extract)
                src = tmp_extract / m
        else:
            raise SystemExit(f"Unsupported archive type: {archive.name} (use .tar.gz or .zip)")

        LIB_DIR.mkdir(parents=True, exist_ok=True)
        dst = LIB_DIR / want
        shutil.copy2(src, dst)
        return dst
    finally:
        shutil.rmtree(tmp_extract, ignore_errors=True)


DEFAULT_GITHUB_REPO = "ngoky/lib_c"


def _resolve_latest_tag(repo: str) -> str:
    """Query GitHub API for the latest release tag (including pre-releases), sorted by published_at descending."""
    api = f"https://api.github.com/repos/{repo}/releases?per_page=30"
    try:
        with urllib.request.urlopen(_request(api), timeout=60) as resp:
            releases = json.load(resp)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:500]
        raise SystemExit(
            f"GitHub API HTTP {e.code} for {api}\n"
            f"  Private repo? Set M4_GITHUB_TOKEN. Body: {body}"
        ) from e
    if not releases:
        raise SystemExit(f"No releases found for {repo}")
    # Sort by published_at descending — GitHub API may not return in chronological order
    releases.sort(key=lambda r: r.get("published_at") or r.get("created_at") or "", reverse=True)
    tag = releases[0].get("tag_name", "")
    if not tag:
        raise SystemExit(f"Latest release for {repo} has no tag_name")
    print(f"Resolved latest release: {tag}", file=sys.stderr)
    return tag


def _pick_platform_asset(assets: list, want: str) -> str:
    """Score release assets by platform match, return best download URL.

    For private repos ``browser_download_url`` returns 404 even with a token.
    Use the API asset URL (``/releases/assets/{id}``) with
    ``Accept: application/octet-stream`` instead — ``_request()`` already adds
    that header when the URL contains ``/releases/assets/``.
    """
    plat_hints = (
        ("darwin", "arm64", "macos", "osx")
        if sys.platform == "darwin"
        else ("linux", "amd64", "x86_64")
    )
    scored: List[Tuple[int, dict]] = []
    for a in assets:
        name = (a.get("name") or "").lower()
        if not (name.endswith(".tar.gz") or name.endswith(".zip")):
            continue
        score = 0
        for i, h in enumerate(plat_hints):
            if h in name:
                score += 10 - i
        if want in name:
            score += 5
        scored.append((score, a))
    scored.sort(key=lambda x: -x[0])
    if not scored or scored[0][0] <= 0:
        names = [a.get("name") for a in assets]
        raise SystemExit(
            f"No matching .tar.gz/.zip for this platform ({want}). Asset names:\n  " + "\n  ".join(names)
        )
    best = scored[0][1]
    # Prefer API asset URL for private repo compatibility; fall back to browser URL.
    return str(best.get("url") or best["browser_download_url"])


def _resolve_url_from_github_release(cli_repo: Optional[str] = None, cli_tag: Optional[str] = None) -> Optional[str]:
    repo = cli_repo or (os.environ.get("M4_ENGINE_GITHUB_REPO") or "").strip() or DEFAULT_GITHUB_REPO
    tag = cli_tag or (os.environ.get("M4_ENGINE_GITHUB_TAG") or "").strip()

    if not tag:
        tag = _resolve_latest_tag(repo)

    api = f"https://api.github.com/repos/{repo}/releases/tags/{tag}"
    try:
        with urllib.request.urlopen(_request(api), timeout=60) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:500]
        raise SystemExit(
            f"GitHub API HTTP {e.code} for {api}\n"
            f"  Private repo? Set M4_GITHUB_TOKEN. Body: {body}"
        ) from e
    assets = data.get("assets") or []
    if not assets:
        raise SystemExit(f"No release assets for {repo}@{tag}")
    return _pick_platform_asset(assets, _want_name())


def _extract_version_from_archive(archive: Path) -> Optional[str]:
    """Read VERSION file from the tarball/zip if present."""
    try:
        if _is_tar_gz(archive):
            with tarfile.open(archive, "r:*") as tf:
                m = _find_member(tf.getnames(), "VERSION")
                if m:
                    f = tf.extractfile(m)
                    if f:
                        return f.read().decode("utf-8", errors="replace").strip()
        elif archive.suffix.lower() == ".zip":
            with zipfile.ZipFile(archive, "r") as zf:
                m = _find_member(zf.namelist(), "VERSION")
                if m:
                    return zf.read(m).decode("utf-8", errors="replace").strip()
    except Exception:
        pass
    return None


def _pip_install_as_package(lib_path: Path, version: str) -> None:
    """Copy the binary into m4engine_binary_dist/native/, update version, pip install."""
    dist_dir = ROOT / "m4engine_binary_dist"
    if not dist_dir.is_dir():
        return
    native_dir = dist_dir / "m4engine_binary" / "native"
    native_dir.mkdir(parents=True, exist_ok=True)
    dst = native_dir / lib_path.name
    shutil.copy2(lib_path, dst)

    # Update version in pyproject.toml
    pyproject = dist_dir / "pyproject.toml"
    if pyproject.is_file():
        text = pyproject.read_text()
        import re
        text = re.sub(r'version\s*=\s*"[^"]*"', f'version = "{version}"', text, count=1)
        pyproject.write_text(text)

    # Update __version__ in __init__.py
    init_py = dist_dir / "m4engine_binary" / "__init__.py"
    if init_py.is_file():
        text = init_py.read_text()
        import re
        text = re.sub(r'__version__\s*=\s*"[^"]*"', f'__version__ = "{version}"', text, count=1)
        init_py.write_text(text)

    # pip install into current env (or venv if active)
    import subprocess
    pip_cmd = [sys.executable, "-m", "pip", "install", "--quiet", str(dist_dir)]
    print(f"Installing m4engine-binary {version} via pip...", file=sys.stderr)
    try:
        subprocess.run(pip_cmd, check=True, timeout=60)
        print(f"pip install OK: m4engine-binary=={version}", file=sys.stderr)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"pip install failed (non-fatal): {e}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description="Fetch libm4engine into python_ai/lib/")
    ap.add_argument("--url", help="HTTPS URL to .tar.gz or .zip containing libm4engine")
    ap.add_argument("--repo", help=f"GitHub owner/repo (default: {DEFAULT_GITHUB_REPO})")
    ap.add_argument("--tag", help="Release tag (e.g. v1.0.0-beta.1). Omit to auto-resolve latest release.")
    ap.add_argument("--no-pip", action="store_true", help="Skip pip install of m4engine-binary package")
    args = ap.parse_args()
    url = (args.url or os.environ.get("M4_ENGINE_DOWNLOAD_URL") or "").strip()
    if not url:
        url = _resolve_url_from_github_release(cli_repo=args.repo, cli_tag=args.tag) or ""
    if not url:
        print(
            "No download URL. Do one of:\n"
            f"  1) python3 scripts/fetch_m4engine_lib.py                    (auto: latest from {DEFAULT_GITHUB_REPO})\n"
            "  2) python3 scripts/fetch_m4engine_lib.py --tag v1.0.0-beta.1 (specific tag)\n"
            "  3) python3 scripts/fetch_m4engine_lib.py --url 'https://...' (direct URL)\n"
            "\n"
            f"Private repo? Set M4_GITHUB_TOKEN. Default repo: {DEFAULT_GITHUB_REPO}",
            file=sys.stderr,
        )
        raise SystemExit(1)

    want = _want_name()
    base = url.split("?")[0].lower()
    suf = ".zip" if base.endswith(".zip") else ".tar.gz"
    with tempfile.TemporaryDirectory(prefix="m4engine_dl_") as td:
        td_path = Path(td)
        archive_path = td_path / f"package{suf}"
        print(f"Downloading: {url[:120]}...", file=sys.stderr)
        try:
            _download(url, archive_path)
        except urllib.error.HTTPError as e:
            raise SystemExit(
                f"Download failed HTTP {e.code}. Private asset? Set M4_GITHUB_TOKEN or use a browser URL."
            ) from e
        version = _extract_version_from_archive(archive_path) or "0.0.0"
        dst = _extract_and_copy(archive_path, want)
    print(f"Installed: {dst} (version: {version})")

    if not args.no_pip:
        _pip_install_as_package(dst, version)


if __name__ == "__main__":
    main()
