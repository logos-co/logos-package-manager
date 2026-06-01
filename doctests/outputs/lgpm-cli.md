# Managing Local Packages with lgpm

[`lgpm`](https://github.com/logos-co/logos-package-manager) is the **local**
Logos package manager. It installs `.lgx` packages into a set of directories,
scans what is installed, and answers dependency questions — all offline, with no
network access (fetching packages is the job of
[`lgpd`](https://github.com/logos-co/logos-package-downloader)).

We build the `lgpm` CLI, hand-craft two `.lgx` packages (a
core module and a UI plugin that depends on it), install them, and explore them
with `list`, `info`, `deps`, and `dependents`.

**What you'll build:** Two installed Logos packages and a working tour of every `lgpm` subcommand.

**What you'll learn:**

- What an `.lgx` package is on disk, and how to build one by hand
- How to install a local `.lgx` package with `lgpm install --file`
- How to list, inspect (`info`), and query the dependency graph (`deps` / `dependents`)
- How `--json` turns every command into machine-readable output

## Prerequisites

- **Nix** with flakes enabled — used to build the `lgpm` CLI.
- A Linux or macOS machine (the package variants below cover both).

---

## Step 1: Build the lgpm CLI

`lgpm` ships as a C++ CLI. Build it straight from the flake and link the
result as `./lgpm` so the binary lands at `./lgpm/bin/lgpm`.

> Developing against a local checkout? Replace the GitHub reference with
> `.`, e.g. `nix build '.#cli' -o lgpm`.

### 1.1 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

The `-o lgpm` flag names the result symlink, so the executable is at
`./lgpm/bin/lgpm`.

### 1.2 Confirm it runs

```bash
./lgpm/bin/lgpm --version
```

---

## Step 2: Detect the platform variant

An `.lgx` package carries one or more **platform variants**. A dev build of
`lgpm` installs the variant matching this machine with a `-dev` suffix (for
example `linux-x86_64-dev` or `darwin-arm64-dev`). Detect it once and stash
it in a file so the packaging steps can reuse it.

### 2.1 Write the variant name to ./variant

```bash
case "$(uname -s) $(uname -m)" in
  "Linux x86_64")   echo linux-x86_64-dev  > variant ;;
  "Linux aarch64")  echo linux-arm64-dev   > variant ;;
  "Darwin arm64")   echo darwin-arm64-dev  > variant ;;
  "Darwin x86_64")  echo darwin-x86_64-dev > variant ;;
  *) echo "unsupported platform: $(uname -sm)" >&2; exit 1 ;;
esac
echo "variant: $(cat variant)"

```

---

## Step 3: Create the core module package

An `.lgx` file is just a **gzipped tar archive** with a `manifest.json` at
its root and the payload under `variants/<platform>/`. `lgpm` reads the
manifest to learn the package's name, version, type, dependencies, and entry
point — so a hand-built tarball is a perfectly valid package. (In a real
project [`nix-bundle-lgx`](https://github.com/logos-co/nix-bundle-lgx)
generates these from a module's `metadata.json`; here we write the manifest
by hand to keep the focus on `lgpm`.)

We start with a tiny **core module** called `greeter`. The payload is a stub —
`lgpm` installs and indexes packages by their manifest; it does not load the
binary.

### 3.1 Write greeter's manifest

The manifest lives at the root of the package tree:

```json
{
  "manifestVersion": "0.2.0",
  "name": "greeter",
  "version": "1.0.0",
  "description": "Says hello",
  "author": "Logos",
  "type": "core",
  "category": "demo",
  "icon": "",
  "dependencies": [],
  "main": { "PLACEHOLDER_VARIANT": "greeter.dylib" }
}
```

The `main` key maps each platform variant to its entry-point file. The
next step substitutes this machine's variant name for the placeholder.

### 3.2 Stamp the variant into the manifest and pack the .lgx

```bash
VARIANT="$(cat variant)"

# Point main[<variant>] at the real variant name
sed -i "s/PLACEHOLDER_VARIANT/$VARIANT/" build/greeter/manifest.json

# Payload goes under variants/<platform>/ (e.g. variants/linux-x86_64-dev/)
mkdir -p "build/greeter/variants/$VARIANT"
echo 'stub library' > "build/greeter/variants/$VARIANT/greeter.so"  # .dylib on macOS

# An .lgx is a gzipped tar of the manifest + the variants/ tree
tar -C build/greeter -czf greeter.lgx manifest.json variants
```

Confirm the archive layout:

```bash
tar -tzf greeter.lgx
```

```
manifest.json
variants/
variants/<platform>-dev/
variants/<platform>-dev/greeter.so
```

---

## Step 4: Create the UI plugin package

Now a **UI plugin** named `greeter_ui` whose manifest declares a dependency
on `greeter`. The `dependencies` array is what powers `deps` / `dependents`.

### 4.1 Write greeter_ui's manifest

```json
{
  "manifestVersion": "0.2.0",
  "name": "greeter_ui",
  "version": "1.0.0",
  "description": "A face for greeter",
  "author": "Logos",
  "type": "ui",
  "category": "demo",
  "icon": "",
  "dependencies": ["greeter"],
  "main": { "PLACEHOLDER_VARIANT": "greeter_ui.dylib" }
}
```

### 4.2 Stamp the variant and pack it

```bash
VARIANT="$(cat variant)"
sed -i "s/PLACEHOLDER_VARIANT/$VARIANT/" build/greeter_ui/manifest.json
mkdir -p "build/greeter_ui/variants/$VARIANT"
echo 'stub library' > "build/greeter_ui/variants/$VARIANT/greeter_ui.so"
tar -C build/greeter_ui -czf greeter_ui.lgx manifest.json variants
```

---

## Step 5: Install the packages

`lgpm` installs into the directories you give it — `--modules-dir` and
`--ui-plugins-dir` — and later scans both. Pass both on every command so it
sees the whole set. Our hand-built packages are unsigned, so we add
`--allow-unsigned`.

### 5.1 Install the core module

```bash
./lgpm/bin/lgpm --modules-dir ./modules --ui-plugins-dir ./plugins --allow-unsigned install --file ./greeter.lgx
```

### 5.2 Install the UI plugin

```bash
./lgpm/bin/lgpm --modules-dir ./modules --ui-plugins-dir ./plugins --allow-unsigned install --file ./greeter_ui.lgx
```

---

## Step 6: List what's installed

`list` scans every configured directory and prints a table of installed
packages. Pass both directories so it sees the module and the plugin.

### 6.1 List packages

```bash
./lgpm/bin/lgpm --modules-dir ./modules --ui-plugins-dir ./plugins list
```

Output (abridged):

```
Found 2 installed module(s):

NAME                           VERSION         TYPE       CATEGORY
----------------------------------------------------------------------
greeter                        1.0.0           core       demo
greeter_ui                     1.0.0           ui         demo
```

---

## Step 7: Inspect a package with info

`info <name>` prints the full record for a single installed package. Add
`--json` for the machine-readable form — handy for scripting.

### 7.1 Human-readable info

```bash
./lgpm/bin/lgpm --modules-dir ./modules --ui-plugins-dir ./plugins info greeter
```

### 7.2 JSON info

```bash
./lgpm/bin/lgpm --modules-dir ./modules --ui-plugins-dir ./plugins info greeter_ui --json
```

---

## Step 8: Query the dependency graph

`deps` lists what a package depends on; `dependents` lists what depends on
it. These read the `dependencies` arrays across every installed package, so
both directories must be configured.

### 8.1 What does greeter_ui depend on?

```bash
./lgpm/bin/lgpm --modules-dir ./modules --ui-plugins-dir ./plugins deps greeter_ui
```

### 8.2 What depends on greeter?

```bash
./lgpm/bin/lgpm --modules-dir ./modules --ui-plugins-dir ./plugins dependents greeter
```

Both commands accept `-r` / `--recursive` to walk the graph transitively,
and `--json` to emit a JSON array instead of one name per line.

---

## Recap

You built `lgpm`, hand-crafted two `.lgx` archives (a gzipped tar of a
`manifest.json` plus a `variants/` tree), installed them, and toured every
`lgpm` subcommand:

| Command | What it does |
|---|---|
| `install --file <pkg>` | Install one `.lgx` (use `--dir` for a whole folder) |
| `list` | Table of everything installed |
| `info <name>` | Full record for one package (`--json` for machine output) |
| `deps <name>` | What a package depends on (`-r` for transitive) |
| `dependents <name>` | What depends on a package (`-r` for transitive) |
