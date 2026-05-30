# Installing a Real Module with lgpm

The [`lgpm-cli`](lgpm-cli.md) doc builds packages by hand to focus on the CLI.
This one is the real thing end-to-end: it clones an actual Logos module
([`logos-accounts-module`](https://github.com/logos-co/logos-accounts-module)),
builds its `.lgx` package straight from the module's own flake, and installs it
with `lgpm`.

**What you'll build:** The `accounts_module` core module, packaged as `.lgx` and installed via lgpm.

**What you'll learn:**

- How a module's flake exposes a ready-to-install `.lgx` via the `#lgx` output
- How to install a freshly-built `.lgx` with `lgpm install --file`
- How to confirm the install with `list` and `info`

## Prerequisites

- **Nix** with flakes enabled — builds both `lgpm` and the module's `.lgx`.
- **git** — to clone the module repository.
- A Linux or macOS machine.

---

## Step 1: Build the lgpm CLI

Build `lgpm` from this repository's flake and link it as `./lgpm`.

> Developing against a local checkout? Use `nix build '.#cli' -o lgpm`.

### 1.1 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

---

## Step 2: Clone the module

Clone [`logos-accounts-module`](https://github.com/logos-co/logos-accounts-module) —
a real `core` module. We clone over HTTPS so the step works in CI; over SSH
the URL is `git@github.com:logos-co/logos-accounts-module.git`.

### 2.1 git clone

```bash
# HTTPS (used here):
git clone --depth 1 https://github.com/logos-co/logos-accounts-module.git

# or over SSH:
# git clone git@github.com:logos-co/logos-accounts-module.git
```

---

## Step 3: Build the module's .lgx

Every module built with
[`logos-module-builder`](https://github.com/logos-co/logos-module-builder)
exposes an `#lgx` output that packages the compiled plugin into an
installable `.lgx`. Build it and link the result as `./accounts-lgx`. (This
compiles the module and its SDK dependencies through Nix, so the first build
is slow.)

### 3.1 nix build .#lgx

```bash
# From inside the clone this is simply: nix build '.#lgx'
nix build 'path:./logos-accounts-module#lgx' -o accounts-lgx
```

The `.lgx` package is now under `./accounts-lgx/`:

```bash
ls accounts-lgx/*.lgx
```

---

## Step 4: Install it with lgpm

Install the freshly-built package into a local `./modules` directory.
`accounts_module` is a `core` module, so it goes to `--modules-dir`. The
package is unsigned (a local dev build), so we pass `--allow-unsigned`.

### 4.1 Install the .lgx

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file accounts-lgx/*.lgx
```

---

## Step 5: Confirm the install

Scan the directory and inspect the installed module.

### 5.1 List installed packages

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

### 5.2 Show its details

```bash
./lgpm/bin/lgpm --modules-dir ./modules info accounts_module
```

That's the whole loop: a real module, packaged by its own flake and
installed with `lgpm` — no hand-written manifests. Add `--json` to either
command for machine-readable output.
