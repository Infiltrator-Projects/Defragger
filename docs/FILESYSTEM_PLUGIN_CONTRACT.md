<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Filesystem plugin contract

Linux Defragger has one filesystem plugin layer: `gui/filesystems/<id>/`.

Each discoverable filesystem package exposes `BACKEND` from `plugin.py`. The
registry in `gui/backends/registry.py` discovers those packages automatically,
validates IDs and aliases, and is the sole authority for filesystem dispatch.
There is no second native filesystem registry or native plugin ABI.

## Backend interface

A read-only plugin implements:

```python
def probe(path: str) -> bool: ...
def map(path: str, cells: int) -> dict: ...
```

Write-capable plugins additionally declare operations and their private worker:

```python
operation("defrag", "fat-native")
operation("growth-defrag", "fat-native")
operation("recover", "fat-native")
```

The operation engine resolves the declared worker without knowing filesystem
format details.

## Native helpers

A filesystem package may contain native C helpers under its own directory, for
example `gui/filesystems/fat/native/`. Those helpers are implementation details
of that plugin, not independently registered filesystems.

Filesystem-neutral native services remain in `src/core/` and may be shared by
such helpers. They provide raw I/O, device safety, staging, stop handling and
runtime support; they do not probe, register or dispatch filesystem formats.

FAT, XFS, Amiga OFS/FFS, HFS+/HFSX, EXT, NTFS and exFAT keep their mutation engines in native C below their owning GUI packages. Their Python `plugin.py` files are thin adapters only; parsing, planning, mutation, staged verification and recovery are native implementation details. Classic HFS owns a first-party direct read-only C analyser under `gui/filesystems/hfs/native/`; no third-party HFS source is bundled. The architecture is one GUI plugin hierarchy with C filesystem engines beneath it, not parallel Python and C implementations of the same filesystem logic.

## Worker invocation

All write workers receive:

```text
WORKER OPERATION TARGET --write --confirm TARGET --journal PATH [options]
```

Common options include `--live-map-cells`, `--growth-percent 10`, `--ram-buffer`,
`--workers` and transaction sizing where supported. Growth Defrag must reject a
reserve other than 10%.

## Worker event protocol

Workers publish typed protocol records such as:

```text
@@PHASE {"message":"Preparing canonical layout"}
@@LIVE_RANGES {"ranges":[[0,4096,4096]],"sequence":1}
@@RESULT {"operation":"growth-defrag","status":"not-needed","message":""}
```

`gui/core/protocol.py` validates these records independently of GTK. A
successful mutation emits exactly one semantic result with operation `defrag`,
`growth-defrag` or `recover` and status `completed`, `not-needed` or `stopped`.
Human-readable log text is not an API.

## Required architecture invariant

Filesystem-specific parsing, planning, writing and verification must live below
`gui/filesystems/<format>/`. The source tree must not contain a parallel
`src/filesystems/` hierarchy or a native filesystem registry. Architecture
tests enforce this invariant.

## Source licensing

Every first-party filesystem plugin, native worker and associated test must carry the project SPDX identifier `GPL-3.0-or-later`. Native C/C headers use `// SPDX-License-Identifier: GPL-3.0-or-later`; Python glue uses `# SPDX-License-Identifier: GPL-3.0-or-later`. A new plugin that fails the SPDX licensing gate is not releasable.
