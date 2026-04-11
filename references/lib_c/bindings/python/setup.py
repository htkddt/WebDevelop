from setuptools import setup
import subprocess, os, sys, platform, json

def get_platform():
    s = platform.system().lower()
    m = platform.machine().lower()
    if m in ("x86_64", "amd64"): m = "amd64"
    elif m in ("aarch64", "arm64"): m = "arm64"
    return f"{s}-{m}"

def download_lib():
    """Download prebuilt library from GitHub Releases during pip install."""
    dest = os.path.join(os.path.dirname(__file__), "m4engine", "_native")
    if os.path.exists(os.path.join(dest, "lib")):
        return  # already downloaded

    plat = get_platform()
    print(f"[m4engine] downloading prebuilt library for {plat}...")

    try:
        subprocess.check_call([
            "bash", os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "install.sh"),
            "--dest", dest
        ])
    except (subprocess.CalledProcessError, FileNotFoundError):
        # Fallback: try gh directly
        import tempfile
        tmpdir = tempfile.mkdtemp()
        try:
            subprocess.check_call([
                "gh", "release", "download",
                "--repo", "ngoky/lib_c",
                "--pattern", f"*{plat}*",
                "--dir", tmpdir
            ])
            tarball = [f for f in os.listdir(tmpdir) if f.endswith(".tar.gz")][0]
            os.makedirs(dest, exist_ok=True)
            subprocess.check_call(["tar", "xzf", os.path.join(tmpdir, tarball),
                                   "--strip-components=1", "-C", dest])
        finally:
            import shutil
            shutil.rmtree(tmpdir, ignore_errors=True)

download_lib()

setup(
    name="m4engine",
    version="1.0.0",
    description="M4 AI Engine — Python bindings (prebuilt native library)",
    packages=["m4engine"],
    package_data={"m4engine": ["_native/lib/*", "_native/include/*", "_native/VERSION"]},
    python_requires=">=3.7",
)
