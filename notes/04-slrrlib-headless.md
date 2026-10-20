# SlrrLib: headless build and CLI

`bin/tools/SlrrLib` (community library for RPK/SCX/CFG/TRC/SPL/class files) was converted to a WPF project
(`net5.0-windows`, `UseWPF`, project reference to a missing `SLStudio.Core`), so it could not build off Windows.

## What was changed (original files untouched; ignored `bin/` folder, so not in git)
- `bin/tools/SlrrLib/SlrrLib.Core.csproj` — new, `net10.0`, compiles only `Model/**` and `Compat/**`.
- `bin/tools/SlrrLib/Compat/Media3D.cs` — small stand-ins for `System.Windows.Media.Media3D.{Vector3D, Point3D, Rect3D}` (same namespace/members), because `Model/` uses exactly these
  (TRC walk geometry, bounding boxes in `DynamicRpk`, `HighLevel/CarScaler`). Includes the mutating `Rect3D.Union` that `DynamicRpk` calls.
- Not included: `Geom/**` and `View/**` (the 3D viewer, XAML dialogs, `System.Windows.Media` brushes/materials) — they genuinely need WPF and are irrelevant for file-format work.
- Build: `dotnet build SlrrLib.Core.csproj -c Release` (0 errors, 0 warnings; SDK at `/opt/dotnet`, run with `--artifacts-path` outside the source tree to keep it clean).
  The first build also hit a .NET 10 clash (`Rect3D.Union` resolved to `AsyncEnumerable.Union` when the method was missing) — fixed by the shim method.

## CLI (`tools/SlrrCli`, in git)
`slrrcli dump <rpk>` | `rpk2rdb2 <rpk> <out>` | `rdb22rpk <rdb2> <out.rpk>` | `roundtrip <rpk>`.
Project references `$(SlrrLibDir)/SlrrLib.Core.csproj` (default `../../bin/tools/SlrrLib`).

## Two different "rdb" formats
- **Game/ImageCode `.rdb`** (what ships next to `.rpk`): `<FILE 0000000N.res >` / `<FILE 0000000N.rsd >` blocks with tab-separated `typeof/superid/typeid/alias/isparentcompatible/rpropcount/rprop` lines (see `01-rpk-rdb-format.md`).
- **SlrrLib "RDB2"** (`RpkExchange.SaveRpkToRDB2` writes `<file>.rpk.RDB2`): `<RPK>`, `<EXTERNAL_REFS>`, `<RES>` with `typeid/superid/additionaltype/alias/isparentcompatible/typeofentry`,
  `<FLOAT_BOUNDS>`, `<RSD>` … i.e. a lossless text dump of the binary structure, designed for round-tripping back to RPK. Not interchangeable with the game's `.rdb`.

## Round-trip results (SlrrCli, all 14 top-level `.rpk` in the install)
- `rpk -> RDB2 -> rpk` is **byte-identical for 8 of 14** (incl. `system.rpk`, 1836 B).
- The other 6 (`sl`, `racers`, `parts`, `frontend`, `objects`, `EngineSoundPack`) differ in bytes, but **RDB2 is stable** for all of them (original → RDB2 equals rebuilt → RDB2).
  Cause seen in `sl.rpk`: the 64-byte external-reference name field holds stale bytes after the NUL (e.g. `cars.rpk\0ge.rpk` — leftovers of a longer earlier string), which the rebuild zero-fills.
  `objects.rpk` (-1,090 B) and `EngineSoundPack.rpk` (-9.8 KB) also shrink: unreferenced/padding data in the RSD area is not preserved. So the structure is fully captured; only dead bytes are lost.
