---
description: Download and install CK Utilities on Linux or macOS, through a package, an archive, or Homebrew, and upgrade it later.
---

# Installation

CK Utilities runs on macOS and Linux. Download the file for your operating
system from the
[release page](https://github.com/cklukas/ckUtilities/releases), or install
it with Homebrew.

Every installation provides the same seven commands: `ck-utilities`,
`ck-json-view`, `ck-find`, `ck-du`, `ck-config`, `ck-edit`, and `ck-chat`.

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

There is no Windows installer, WinGet manifest, or binary download yet. Use a
macOS or Linux installation for now.

## Next steps

- Open the [launcher](tools/ck-utilities.md) and pick a tool.
- To use [Chat](tools/ck-chat.md), point it at a local GGUF model: copy
  `configs/ckai.example.toml` from the source to `~/.config/cktools/ckai.toml`
  and set its model path.
- To build CK Utilities yourself, see [Building from source](building.md).
