# Toolchain decision: MinGW-w64 i686 (GCC 14.2), msvcrt, ASI DLL

Investigated 2026-10-20 on the Debian trixie container. All proofs are in
`/tmp/slrr-tc/` (ephemeral). Nothing under `src/` or `bin/` was changed.

## Decision

- Build everything 32-bit with Debian's **mingw-w64 i686 GCC 14.2.0**
  (`i686-w64-mingw32-gcc/g++`, win32 thread model, msvcrt CRT).
- Hybrid phase ships as a plain **`.asi` DLL** (`-shared`), with
  `-static-libgcc -static-libstdc++` so no extra runtime DLLs are needed in the
  game directory.
- Hook through raw addresses/RVAs with explicit `__cdecl` / `__stdcall` /
  `__thiscall` function-pointer types. Do not expect MSVC C++ ABI
  compatibility (mangling, RTTI, exceptions); see risks.
- Do **not** install Wine now: `wine32` is not installable in the current apt
  configuration (no i386 foreign architecture), and the task only needs
  structural verification. Runtime smoke tests stay a Windows-machine step (or
  a later `dpkg --add-architecture i386` + wine32 setup).
- Do not use clang-cl/lld-link or MSVC-under-Wine for this phase (see
  Alternatives).

## Installed packages (exact)

```
apt-get install -y --no-install-recommends gcc-mingw-w64-i686 g++-mingw-w64-i686
```

Installed (dpkg):

```
binutils-mingw-w64-i686            2.44-3+12+b1
gcc-mingw-w64-i686                 14.2.0-17+27
g++-mingw-w64-i686                 14.2.0-17+27
gcc-mingw-w64-i686-win32           14.2.0-19+27+b1
gcc-mingw-w64-i686-posix           14.2.0-19+27+b1
g++-mingw-w64-i686-win32           14.2.0-19+27+b1
g++-mingw-w64-i686-posix           14.2.0-19+27+b1
...-win32-runtime / ...-posix-runtime, mingw-w64-common, mingw-w64-i686-dev  12.0.0-5
```

`i686-w64-mingw32-gcc` resolves to the **win32** variant:

```
Target: i686-w64-mingw32
Thread model: win32
```

The posix variants are also installed; call `i686-w64-mingw32-g++-posix`
explicitly only if the posix thread model is ever needed (it adds
`libwinpthread-1.dll` unless fully static; win32 does not).

`apt-cache policy` at decision time: `mingw-w64` 12.0.0-5, `gcc-mingw-w64-i686`
14.2.0-17+27, `wine`/`wine64` 10.0~repack-6, **`wine32` has no candidate**,
`clang`/`lld` 1:19.0-63.

## Proof: 32-bit Windows target

### Console EXE

```
$ i686-w64-mingw32-gcc -O2 -o hello.exe hello.c
$ file hello.exe
hello.exe: PE32 executable for MS Windows 4.00 (console), Intel i386, 16 sections

$ objdump -f hello.exe
hello.exe:     file format pei-i386
architecture: i386, flags 0x0000013b:
HAS_RELOC, EXEC_P, HAS_DEBUG, HAS_SYMS, HAS_LOCALS, D_PAGED
start address 0x004013f0

$ objdump -p hello.exe | grep -E 'Magic|ImageBase|Subsystem|DLL Name'
Magic                   010b    (PE32)
ImageBase               00400000
MajorSubsystemVersion   4
Subsystem               00000003        (Windows CUI)
        DLL Name: KERNEL32.dll
        DLL Name: msvcrt.dll
```

### DLL with DllMain + exports

Source used `__declspec(dllexport) int __stdcall Add(int,int)` and
`__declspec(dllexport) const char* __cdecl Version(void)`, plus a `DllMain`.

```
$ i686-w64-mingw32-gcc -O2 -shared -o asi_test.dll asi_test.c \
      -Wl,--out-implib,libasi_test.a -static-libgcc
$ file asi_test.dll
asi_test.dll: PE32 executable for MS Windows 4.00 (DLL), Intel i386, 17 sections

$ objdump -p asi_test.dll | grep -E \
    'Magic|AddressOfEntryPoint|ImageBase|DllCharacteristics|DYNAMIC_BASE|NX_COMPAT|Base Relocation|DLL Name'
Magic                   010b    (PE32)
AddressOfEntryPoint     000013b0
ImageBase               647c0000
DllCharacteristics      00000140
                        DYNAMIC_BASE
                        NX_COMPAT
Entry 5 0000a000 000001d8 Base Relocation Directory [.reloc]
        DLL Name: KERNEL32.dll
        DLL Name: msvcrt.dll

$ objdump -p asi_test.dll | sed -n '/Ordinal\/Name Pointer/,/^$/p'
[Ordinal/Name Pointer] Table -- Ordinal Base 1
    [   0] +base[   1]  0000 Add@8
    [   1] +base[   2]  0001 Version
```

So: i386 DLL, CRT startup as entry point, hand-written `DllMain`, real export
table, and a `.reloc` section (required: the ASI loader can load it at an
arbitrary base). The default `DllCharacteristics` already includes
`DYNAMIC_BASE` and `NX_COMPAT`.

### Export naming and minimal variants

- `__stdcall` exports get the `@N` decoration (`Add@8`); `__cdecl` (`Version`)
  is undecorated. `-Wl,--kill-at` makes stdcall exports plain (`Add`).
- A `.def` file gives exact names and is the cleanest way to export a plugin
  entry point:

```
$ cat init.def
EXPORTS
  InitializeASI
$ i686-w64-mingw32-gcc -O2 -shared -o init.asi init.c init.def -static-libgcc
$ objdump -p init.asi | sed -n '/Ordinal\/Name Pointer/,/^$/p'
    [   0] +base[   1]  0000 InitializeASI
```

- A no-CRT DLL also links (`-shared -nostdlib -lkernel32
  -Wl,--entry,_DllMain@12`; note the COFF symbol is `_DllMain@12`), but that
  test build had **no relocation directory**, so it must load at its preferred
  base and is not suitable for an ASI. Use the normal CRT-linked build.

### ABI checks (compile-time)

```
$ i686-w64-mingw32-gcc -c abi.c     # _Static_asserts passed
struct { char c; int i; }                  -> sizeof 8, offsetof(i) == 4
#pragma pack(push,1) same struct           -> sizeof 5
sizeof(long)==4, sizeof(void*)==4          -> ILP32
```

Macros: `__i386__`, `_WIN32`, `__MSVCRT__`, `__SIZEOF_POINTER__=4`,
`__SIZEOF_WCHAR_T__=2` (`wchar_t` = unsigned short), `__SIZEOF_LONG_DOUBLE__=12`,
char signed (no `__CHAR_UNSIGNED__`). GCC target options show
`-mms-bitfields [enabled]` (MSVC-compatible bitfield layout) and
`-mrtd [disabled]` (default cdecl).

`__thiscall` and `__attribute__((naked))` both compile on this target:

```
int __thiscall thiscall_add(void *self, int x);      // ECX = this, callee pops
__attribute__((naked)) void naked_hook(void) { ... } // trampolines
```

## Import libraries and headers in the sysroot

`/usr/i686-w64-mingw32/lib/` contains, among others:

```
libd3d9.a              libdinput8.a       libdsound.a
libd3dx9.a ... libd3dx9_32.a ... libd3dx9_43.a   (including the exact
                                                  d3dx9_32 the game ships)
libuser32.a            libkernel32.a      libdxguid.a
libmsvcrt.a / libmsvcrt-os.a              libucrt.a / libucrtbase.a
```

Headers in `/usr/i686-w64-mingw32/include/`: `d3d9.h`, `d3d9types.h`,
`d3dx9*.h`, `dsound.h`, `dinput.h` (dinputd.h). A C++ translation unit
including `windows.h`, `d3d9.h`, `dsound.h` and `dinput.h` compiles cleanly
with `i686-w64-mingw32-g++`.

Link test (all four subsystems + Win32):

```
$ i686-w64-mingw32-gcc -O2 -o linkdeps.exe linkdeps.c \
      -ld3d9 -ldsound -ldinput8 -luser32 -lkernel32 -ldxguid
$ objdump -p linkdeps.exe | grep 'DLL Name'
        DLL Name: d3d9.dll
        DLL Name: DINPUT8.dll
        DLL Name: d3dx9_32.dll     # from a separate -ld3dx9_32 test
        DLL Name: dsound.dll
        DLL Name: KERNEL32.dll
        DLL Name: msvcrt.dll
        DLL Name: USER32.dll
```

Notes:

- `DirectInput8Create` is `_DirectInput8Create@20`; the IIDs are **not** in
  `libdinput8.a` — link `-ldxguid` as well.
- `-ld3dx9_32` resolves to import name `d3dx9_32.dll`, exactly the DLL shipped
  in `bin/the game/`. Other D3DX versions have their own import libs.
- Anything lacking an import lib can be reached with `GetProcAddress` on the
  already-loaded module; the game already imports `d3d9`, `d3dx9_32`, `DINPUT8`,
  `DSOUND`, `KERNEL32`, `USER32`, `GDI32`, `ole32`, `OLEAUT32`, `WINMM`, `PSAPI`,
  `steam_api` (see `objdump -p` on the exe).

## ASI-hook path: feasible, with direct evidence from this install

### The game already loads an ASI loader

`bin/the game/dinput8.dll` is the shipped proxy that the exe imports
(`DLL Name: DINPUT8.dll`), and it is **Ultimate ASI Loader (Win32)**:

- PDB path string:
  `D:\a\Ultimate-ASI-Loader\Ultimate-ASI-Loader\bin\Win32\Release\dinput8.pdb`
- wide strings `ASI Loader`, `*.asi`, and the ThirteenAG project URL;
- exported marker `IsUltimateASILoader` (ordinal 544);
- the string `InitializeASI` is present in the loader but is **not** one of its
  own exports, i.e. the loader looks it up on each plugin with
  `GetProcAddress` (optional plugin entry point).

Because the exe imports `DINPUT8.dll`, Windows resolves the local proxy first
(application-directory search order), so `.asi` files in the game root are
loaded into the game process at startup.

### Plugin shape that works

- `bin/the game/ModernSlrrPatch.asi` is a shipped example: **no exports at
  all**, imports only `WINMM.dll`, `USER32.dll`, `KERNEL32.dll`, and uses
  `VirtualProtect`. That is the DllMain + pattern-patch ASI pattern; an export
  is optional. Our `asi_test.dll`/`init.asi` prove the same shape builds here,
  with the optional `InitializeASI` export if we want an explicit init entry.
- ASI loader only calls `LoadLibrary`; any runtime interface between our DLL and
  other code must be our own exported C functions (`GetProcAddress`), with a
  `.def`/`--kill-at` to keep plain names.

### CRT linkage and UCRT vs msvcrt

- Debian mingw defaults to **msvcrt** (`__MSVCRT__`); our DLLs import
  `msvcrt.dll`, a system DLL present on every supported Windows. `libucrt.a`
  exists, but UCRT mode is a different CRT (ucrtbase.dll, Win10+); it is the
  wrong choice for an XP-era game and adds a dependency for no benefit.
- The game exe imports **no** `msvcrt.dll`, `ucrtbase.dll` or `vcruntime*.dll`,
  and has no delay-import directory: its MSVC CRT is statically linked.
  Consequences for a hybrid DLL:
  - there are two heaps/CRT states (game static MSVC CRT vs our msvcrt);
    never `free()`/`delete` memory across the boundary, and never hand an STL
    or CRT-owned object to the game (or vice versa). Use the game's allocator
    when the game owns the object; `HeapAlloc`/our own allocator otherwise;
  - C++ exceptions must not unwind across a hook boundary — catch inside our
    DLL;
  - C++ static initialization runs in our `DllMain`, before game init; keep it
    side-effect-free and do not touch game globals there.
- C++ runtime DLLs: a default `g++ -shared` DLL imports
  `libgcc_s_dw2-1.dll` and `libstdc++-6.dll`; the game dir ships neither.
  Build with `-static-libgcc -static-libstdc++`; the resulting DLL imports only
  `KERNEL32.dll` and `msvcrt.dll`. If the posix thread model is used, also
  force-static (or `-static`), otherwise `libwinpthread-1.dll` appears. The
  win32 model's `std::thread` links without it (verified).
- Cleanest ASI shape: `gcc/g++ -shared -static-libgcc -static-libstdc++`; no
  additional DLLs to deploy.

### ASLR / relocation

The original exe has `DllCharacteristics 0x8140` = `DYNAMIC_BASE | NX_COMPAT |
TERMINAL_SERVER_AWARE` and a `.reloc` directory, so with Windows ASLR the
runtime base will not reliably be `0x400000`. Hooks must compute
`GetModuleHandleA(NULL)` + RVA (or scan patterns), not use absolute Ghidra
addresses. Our DLLs carry `.reloc` and `DYNAMIC_BASE`, so the loader can place
them anywhere.

## ABI risks specific to GCC vs the MSVC-built game

1. **C++ ABI**: GCC uses the Itanium C++ ABI; the game uses MSVC. Name
   mangling, vtable/RTTI layout and exception metadata differ. Do not link
   against game C++ symbols and do not exchange C++ types across the boundary.
   Reimplement classes with matching layouts and call through raw addresses;
   keep hook interfaces `extern "C"`/POD.
2. **CRT/heap**: as above — static MSVC CRT in the game vs msvcrt in the DLL.
3. **long double**: GCC is 12 bytes (80-bit x87); MSVC is 8 bytes (same as
   double). If a reverse-engineered struct ever contains `long double`, the
   layout differs; use `double` or verify against Ghidra.
   `wchar_t` (2 bytes), plain `char` (signed), `int`/`long`/pointers (4) and
   default struct alignment (8, MSVC `/Zp8`) match; `-mms-bitfields` is on by
   default, so bitfields match MSVC rules.
4. **Calling conventions**: MSVC member functions are `__thiscall` (this in
   ECX, callee pops, hidden struct-return pointer where applicable). GCC's
   `__thiscall` matches on i686, but every hook signature must be checked
   against the disassembly, especially struct returns and variadic functions.
   Use `__attribute__((naked))`, not `__declspec(naked)` (unsupported).
5. **Struct packing**: default pack 8 and `#pragma pack(1)` behave as expected,
   but every RDB-derived layout still deserves `static_assert(offsetof...)`
   against the Ghidra offsets (`tools/` already generates these tables).
6. **Aliasing/optimization**: reverse-engineered code often type-puns through
   casts; consider `-fno-strict-aliasing` for the reimplemented engine to avoid
   GCC miscompiles. This is a code-generation choice, not an ABI one.
7. **No runtime proof here**: no Wine (`wine32` has no candidate in the current
   apt configuration; `wine64` alone cannot run a 32-bit exe, so it was not
   installed). Only file/objdump structure is proven. First ASI load and D3D9
   smoke test must run on Windows (or after enabling i386 multiarch and
   installing wine32).

## Alternatives considered

| Option | Verdict |
|---|---|
| **mingw-w64 i686 (chosen)** | Native Linux cross build, free import libs/headers for every subsystem the game uses, produces valid PE32 i386, ASI DLL proven. |
| clang-cl + lld-link + Windows SDK | Impossible on Linux without the Windows SDK headers/import libs; MSVC ABI compatibility does not help since internal game symbols are unknown anyway. |
| clang `--target=i686-w64-windows-gnu` | Could reuse this same mingw sysroot (clang/lld 1:19 are installable), but adds no capability over GCC now. Revisit only for better diagnostics/sanitizers. |
| MSVC under Wine | Needs 32-bit Wine (not currently installable: no i386 foreign arch), a licensed MSVC toolchain download, and a slow build loop. Only advantage is exact MSVC CRT/ABI, which the hybrid design does not require. |
| Full static mingw (`-static`) | Works if desired (verified: C++/threads then import only `KERNEL32`+`msvcrt`); `-static-libgcc -static-libstdc++` is enough for the ASI. |

## Recommended build commands

ASI / hybrid DLL:

```
i686-w64-mingw32-g++ -std=c++20 -O2 -fno-strict-aliasing -Wall -Wextra \
  -shared -o MyMod.asi mod.cpp hooks.cpp \
  -static-libgcc -static-libstdc++ \
  -ld3d9 -ldsound -ldinput8 -ldxguid -luser32 -lkernel32
```

Optional exports (plain names): add `-Wl,--kill-at`, or pass a `.def` with
`EXPORTS InitializeASI`. Do not add `-Wl,--out-implib` unless another module
links against us.

Full-exe replacement later (same compiler), matching the original PE shape:

```
i686-w64-mingw32-g++ -O2 ... -o StreetLegal_Redline.exe ... \
  -Wl,--subsystem,windows -Wl,--major-subsystem-version,5 \
  -Wl,--minor-subsystem-version,1 -Wl,--image-base,0x400000
```

Verification for every artifact:

```
file MyMod.asi
objdump -f MyMod.asi                    # file format pei-i386
objdump -p MyMod.asi | grep -E 'Magic|ImageBase|Subsystem|DllCharacteristics|Base Relocation|DLL Name'
objdump -p MyMod.asi | sed -n '/Ordinal\/Name Pointer/,/^$/p'
```

## Test sources (in /tmp/slrr-tc, not in the repo)

`hello.c`, `asi_test.c`, `cpp_test.cpp`, `linkdeps.c`, `abi.c`, `cc_test.cpp`,
`thread_test.cpp`, `d3dx2.c`, `init.c` + `init.def`, plus built `.exe`/`.dll`
and the `libasi_test.a` import library.
