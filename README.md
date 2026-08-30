# CK Utilities

![ck-chat conversation interface](images/Screenshot%202025-10-25%20at%2023.34.31.png)
![ck-chat model loading](images/Screenshot%202025-10-25%20at%2023.34.40.png)
![ck-chat result](images/Screenshot%202025-10-25%20at%2023.36.05.png)

CK Utilities is a collection of focused terminal applications for everyday
work: finding files, inspecting JSON, understanding disk usage, editing
Markdown, managing settings, and working with local AI models.

Use `ck-utilities` as a launcher, or start any tool directly:
`ck-json-view`, `ck-find`, `ck-du`, `ck-config`, `ck-edit`, and `ck-chat`.

## Free software; no user support or warranty

CK Utilities is provided free of charge, as-is. The maintainer does not offer
user support, maintenance, updates, guarantees, or warranties. Use it at your
own discretion; the legal terms are in [LICENSE](LICENSE).

## Tools

| Tool | Main features |
| --- | --- |
| `ck-utilities` | Launch the installed tools; access shared help and application controls. |
| `ck-json-view` | Browse JSON as an expandable tree; search, reload, and copy values. |
| `ck-find` | Build guided file searches with filters, previews, cancellation, and deliberate result actions. |
| `ck-du` | Scan folders in the background and browse directory sizes and file lists. |
| `ck-config` | Edit Disk Usage and Chat settings; manage keyboard shortcuts. |
| `ck-edit` | Edit Markdown with search, undo/redo, formatting, links, footnotes, and tables. |
| `ck-chat` | Chat with local GGUF models; manage prompts and models, cancel responses, and export conversations. |

CK Utilities runs on macOS and Linux. A Windows version is not available yet.

## Install

Choose your operating system and download the matching file from the release
page.

### Linux

Download the `.deb` or `.rpm` asset attached to the matching GitHub release,
then install it with your package manager:

```sh
# Debian, Ubuntu, and derivatives
sudo apt install ./ck-utilities_*.deb

# Fedora, RHEL, openSUSE, and other RPM-family distributions
sudo dnf install ./ck-utilities-*.rpm
```

The package installs `ck-utilities`, `ck-json-view`, `ck-find`, `ck-du`,
`ck-config`, `ck-edit`, and `ck-chat` on `PATH`. Confirm the install with:

```sh
ck-utilities --help
```

### macOS

Download the archive named
`ck-utilities-<version>-macos.tar.gz`. Extract it wherever you keep local
applications, then add its `bin` directory to your shell path:

```sh
tar -xzf ck-utilities-<version>-macos.tar.gz
export PATH="$PWD/ck-utilities-<version>-macos/bin:$PATH"
ck-utilities --help
```

Add the `export` line to your shell profile if you want it to persist.

Homebrew installation is not available yet; use the archive above.

### Windows

There is no Windows installer, WinGet manifest, or binary download yet. Use a
macOS or Linux download for now.

## Tool guides

- [CK Utilities launcher](docs/tools/ck-utilities.md)
- [JSON View](docs/tools/json-view.md)
- [Find](docs/tools/ck-find.md)
- [Disk Usage](docs/tools/ck-du.md)
- [Config](docs/tools/ck-config.md)
- [Edit](docs/tools/ck-edit.md)
- [Chat](docs/tools/ck-chat.md)

## Use ck-chat with a local model

`ck-chat` uses models stored on your computer. Create a personal configuration
file, then set its model path to a local GGUF model:

```sh
mkdir -p ~/.config/cktools
cp configs/ckai.example.toml ~/.config/cktools/ckai.toml
```

## Build from source

This is optional. You need a C++20 compiler, CMake 3.25 or later, Ninja, and
an installed ckVision SDK.

```bash
git clone https://github.com/cklukas/ckUtilities.git
cd ckUtilities
cmake -S . -B build/native \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk
cmake --build build/native
./build/native/bin/ck-find --help
```

For build, packaging, and test details, see [COMPILE.md](COMPILE.md).
