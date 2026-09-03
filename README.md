# drop — Distill Linux Binary Package Manager

`drop` is the native, ultra-minimal precompiled binary package manager for **Distill Linux** (`x86_64-musl`).

## Features
- **Zero bloat**: ~39 KB stripped static-friendly ELF binary targeting pure `musl libc`.
- **Streaming verification**: On-the-fly SHA-256 verification and streaming `.drop` (gzipped ustar) decompression.
- **Strict manifest security**: Enforces `.PORT` metadata as the first header in the archive with illegal path traversal protection (`../`).
- **Integrity auditing**: `drop check` audits all installed files against recorded SHA-256 hashes in the local package database (`/var/db/drop/ports/<name>/.PORT`).
- **Clean builds**: Compiles cleanly with `samurai` (`samu`), BSD `bmake`, and POSIX `make` using `clang` with zero warnings.

## Usage
```sh
# Synchronize repository catalog
drop update

# Install a package from repository or local file
drop in <package>
drop in path/to/pkg-1.0.drop

# Audit installed package integrity against SHA-256 hashes
drop check <package>
drop check

# List installed packages
drop ls

# Query package metadata
drop info <package>

# Uninstall package
drop rm <package>
```

## Building
```sh
# Using samurai (ninja)
samu

# Or using make
make CC=clang
```
