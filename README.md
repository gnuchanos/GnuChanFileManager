# GnuChan File Manager

GnuChan File Manager is a small Nemo-like GTK 3 desktop file manager for
Linux. It provides directory browsing, sortable name/type/size columns,
default-application launching, back/forward/up and address navigation,
case-insensitive filtering, a persistent hidden-files toggle, and confirmed
copy, move, rename, Trash, and permanent-delete actions. A left sidebar provides
a lazy-loaded directory tree and desktop-reported mounted or removable
volumes, with mount, unmount, and eject actions where the device supports them.
File actions are grouped in a right-click menu.

## Requirements

- GTK 3 development files
- GLib/GIO development files
- `pkg-config`, a C11 compiler, and GNU Make

On Debian/Ubuntu, install the build prerequisites with:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev libglib2.0-dev
```

## Build and test

Run these commands from the project directory (including in WSL):

```sh
make
make test
```

The focused file-operations tests create and clean `.test-work` within the
project directory. They cover safe child names, clipboard URI-list parsing,
copy/move/rename/delete, and refusing to copy a directory into itself.

## Install

```sh
sudo make install
```

The executable is installed to `/usr/local/bin` and the desktop entry is
installed to `/usr/local/share/applications`. Override `PREFIX` or use
`DESTDIR` for package staging, for example `make install PREFIX=/usr`.
Run `sudo make uninstall` to remove those installed files.

## Configuration and safety

The app reads and writes
`$XDG_CONFIG_HOME/GnuChanFileManager/config.conf` (normally
`~/.config/GnuChanFileManager/config.conf`). Configuration uses plain
`[Section]` and `key=value` syntax and is never executed as code. The bundled
`config.conf` is an install-time template. Only single safe child names are
accepted by rename and recursive file operations; file paths are represented
as `GFile` objects. Opening files uses GIO's default-application API rather
than a shell. Existing destination files are not overwritten. Trash and
permanent deletion require confirmation. Copy and cut/paste exchange standard
file URI clipboard data; transfers use GIO and do not invoke a shell. Mounting,
unmounting, and ejecting depend on the desktop's GIO volume-monitor backend
and the current user's device permissions.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / cut / paste selected files |
| `F2` | Rename selected item |
| `Delete` / `Shift+Delete` | Move to Trash / permanently delete |
| `Ctrl+H` | Toggle hidden files |
| `Ctrl+L` | Focus the location field |
| `Alt+Left` / `Alt+Right` / `Alt+Up` | Back / forward / parent directory |

## Version

See `version.txt`.
