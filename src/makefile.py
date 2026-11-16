#!/usr/bin/env python3
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
BUILD = ROOT / "build"
APP = "GnuChanFileManager"

X11_PACKAGES = ["x11", "xft"]
GIO_PACKAGES = ["gio-2.0", "gdk-pixbuf-2.0"]


def run(cmd, **kwargs):
    if isinstance(cmd, str):
        printable = cmd
        cmd = shlex.split(cmd)
    else:
        printable = " ".join(shlex.quote(c) for c in cmd)
    print("+", printable)
    subprocess.run(cmd, check=True, **kwargs)


def pkg_config(args, packages):
    pkg = os.environ.get("PKG_CONFIG", "pkg-config")
    out = subprocess.check_output([pkg] + args + packages, text=True)
    return shlex.split(out.strip())


def read_version():
    return (ROOT / "version.txt").read_text(encoding="utf-8").strip()


def ensure_build_dir():
    BUILD.mkdir(parents=True, exist_ok=True)


def cflags_base():
    cc = os.environ.get("CC", "cc")
    base = shlex.split(os.environ.get("CFLAGS", "-O2 -g"))
    warnings = ["-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    return cc, base + warnings


def generate_icons():
    script = SRC / "gen_icons.py"
    if script.exists():
        run([sys.executable, str(script)], cwd=ROOT)


def compile_objects():
    ensure_build_dir()
    generate_icons()
    cc, flags = cflags_base()
    version = read_version()
    x11_gio = pkg_config(["--cflags"], X11_PACKAGES + GIO_PACKAGES)
    gio = pkg_config(["--cflags"], GIO_PACKAGES)

    run([cc] + flags + ["-std=c11", "-D_GNU_SOURCE", f'-DAPP_VERSION="{version}"']
        + x11_gio + ["-Isrc", "-c", str(SRC / "main.c"), "-o", str(BUILD / "main.o")],
        cwd=ROOT)
    run([cc] + flags + ["-std=c11"]
        + gio + ["-Isrc", "-c", str(SRC / "gfm_file_ops.c"), "-o", str(BUILD / "gfm_file_ops.o")],
        cwd=ROOT)


def link_app():
    cc, flags = cflags_base()
    libs = pkg_config(["--libs"], X11_PACKAGES + GIO_PACKAGES)
    run([cc] + flags + [str(BUILD / "main.o"), str(BUILD / "gfm_file_ops.o")]
        + libs + ["-lpthread", "-o", str(ROOT / APP)],
        cwd=ROOT)


def install_file(src, dst, mode):
    try:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        os.chmod(dst, mode)
        print(f"Installed {src} -> {dst}")
    except PermissionError:
        print(f"Permission denied for {dst}, trying with sudo...")
        run(["sudo", "install", "-Dm" + format(mode, "o"), str(src), str(dst)])


def install_paths():
    prefix = Path(os.environ.get("PREFIX", "/usr/local"))
    destdir = Path(os.environ.get("DESTDIR", "/"))
    return destdir / prefix.relative_to(prefix.anchor)


def install_app():
    app_path = ROOT / APP
    staged_prefix = install_paths()
    install_file(app_path, staged_prefix / "bin" / APP, 0o755)
    install_file(ROOT / "gnuchanfilemanager.desktop",
                 staged_prefix / "share" / "applications" / "gnuchanfilemanager.desktop", 0o644)
    install_file(SRC / "config" / "config.conf",
                 staged_prefix / "share" / "gnuchanfilemanager" / "config.conf", 0o644)
    install_file(ROOT / "version.txt",
                 staged_prefix / "share" / "gnuchanfilemanager" / "version.txt", 0o644)
    icons_dir = SRC / "icons" / "ui"
    if icons_dir.exists():
        dest_icons = staged_prefix / "share" / "gnuchanfilemanager" / "icons" / "ui"
        try:
            dest_icons.mkdir(parents=True, exist_ok=True)
            for p in icons_dir.glob("*.png"):
                shutil.copy2(p, dest_icons / p.name)
                os.chmod(dest_icons / p.name, 0o644)
            print(f"Installed icons -> {dest_icons}")
        except PermissionError:
            print(f"Permission denied for {dest_icons}, trying with sudo...")
            run(["sudo", "mkdir", "-p", str(dest_icons)])
            for p in icons_dir.glob("*.png"):
                run(["sudo", "install", "-Dm644", str(p), str(dest_icons / p.name)])


def main():
    try:
        compile_objects()
        link_app()
        print(f"Built {ROOT / APP}")
        install_app()
        print("Install complete.")
    except subprocess.CalledProcessError as e:
        print(f"Command failed with exit code {e.returncode}", file=sys.stderr)
        return e.returncode or 1
    except PermissionError as e:
        print(f"Permission denied: {e}", file=sys.stderr)
        print(
            "Kurulum için yetki gerekiyor. sudo başarısız olduysa 'sudo python3 src/makefile.py' ile çalıştırın "
            "veya PREFIX ile kullanıcı dizinine kurun (örn: PREFIX=$HOME/.local python3 src/makefile.py).",
            file=sys.stderr,
        )
        return 1
    except FileNotFoundError as e:
        print(f"Missing required tool/file: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

