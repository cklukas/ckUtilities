---
description: Download and install CK Utilities on Linux or macOS, through a package, an archive, or Homebrew, and upgrade it later.
---

# Installation

CK Utilities runs on macOS, Linux, and Windows. Download the file for your
operating system from the
[release page](https://github.com/cklukas/ckUtilities/releases), or install
it with Homebrew.

Every installation provides the same commands: `ck-utilities`,
`ck-json-view`, `ck-find`, `ck-du`, `ck-edit`, `ck-chat`, `ck-todo`,
`ck-sysmon`, and `ck-config`.

## Linux

Download the `.deb` or `.rpm` asset attached to the matching release, then
install it with your package manager:

```sh
# Debian, Ubuntu, and derivatives
sudo apt install ./ck-utilities_*.deb

# Fedora, RHEL, openSUSE, and other RPM-family distributions
sudo dnf install ./ck-utilities-*.rpm
```

The package puts every command on `PATH`. Confirm the install with:

```sh
ck-utilities --help
```

## macOS

### Homebrew

The Homebrew tap builds CK Utilities from the release source, together with
its pinned ckVision SDK:

```sh
brew install cklukas/ckutilities/ck-utilities
```

To upgrade later:

```sh
brew update
brew upgrade cklukas/ckutilities/ck-utilities
```

### Archive

Download the archive named `ck-utilities-<version>-macos.tar.gz`. Extract it
wherever you keep local applications, then add its `bin` directory to your
shell path:

```sh
tar -xzf ck-utilities-<version>-macos.tar.gz
export PATH="$PWD/ck-utilities-<version>-macos/bin:$PATH"
ck-utilities --help
```

Add the `export` line to your shell profile to keep it.

## Windows

Download `ck-utilities-<version>-windows-x64.zip` from the release page and
check it against the `.sha256` file published beside it:

```powershell
(Get-FileHash .\ck-utilities-<version>-windows-x64.zip).Hash
```

Extract the archive wherever you keep local applications and add its `bin`
folder to your user `PATH`; each tool is a single self-contained `.exe` that
needs no runtime libraries:

```powershell
Expand-Archive .\ck-utilities-<version>-windows-x64.zip -DestinationPath $env:LOCALAPPDATA\Programs
# then add %LOCALAPPDATA%\Programs\ck-utilities-<version>-windows-x64\bin to PATH
ck-utilities --help
```

CK Utilities needs Windows 10 version 1903 or later (or Windows 11) and runs
in Windows Terminal or the console host. File names are handled as Unicode
throughout.

## Next steps

- Open the [launcher](tools/ck-utilities.md) and pick a tool.
- To use [Chat](tools/ck-chat.md), point it at a local GGUF model: copy
  `configs/ckai.example.toml` from the source to `~/.config/cktools/ckai.toml`
  (`%APPDATA%\cktools\ckai.toml` on Windows) and set its model path.
- To build CK Utilities yourself, see [Building from source](building.md).
