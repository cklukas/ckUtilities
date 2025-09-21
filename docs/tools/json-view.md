# json-view — Interactive JSON browser

## SYNOPSIS

```
ckjsonview [path]
```

## DESCRIPTION

`ckjsonview` embeds the Turbo Vision frontend from the standalone
`json-view` project so that CkTools gains an interactive JSON browser on
Day 1.  The viewer presents parsed JSON data as a tree where each node
is decorated with friendly icons, Unicode box-drawing characters, and
previews for common types.  Search results are highlighted inline, and
clipboard export uses OSC 52 sequences when supported by the terminal.

If a file path is provided on the command line it will be opened on
startup.  Otherwise `ckjsonview` prompts for a file via the Turbo Vision
file picker.

## OPTIONS

`ckjsonview` accepts the same flags as the upstream project:

* `--help` – display usage information.
* `--version` – show the embedded json-view version string.

## EXAMPLES

Open a JSON document from the current directory:

```
ckjsonview example.json
```

Launch the viewer and choose a file interactively:

```
ckjsonview
```

## EXIT STATUS

`ckjsonview` exits with status 0 on success and non-zero on failure.

## SEE ALSO

`jq(1)`, `json_pp(1)`
