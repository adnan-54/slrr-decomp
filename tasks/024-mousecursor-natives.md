---
id: 024
status: todo
branch:
depends: []
---
# input: MouseCursor.getPos / getPickedPos (both overloads)

## Goal
Make the four `java.io.MouseCursor` natives non-stubs. The exe reads the
MouseCursor script object's `cursor` field (a GameRef to the RID_CURSOR native
object) and asks it for info block 2 (screen x,y) or 0x3b (picked world x,y,z).
In our tree the render side reports those through `input::cursor_pos` /
`input::cursor_picked_pos` (src/input/cursor.h). **Dependency:** no renderer
installs a `CursorSource` yet, so until one exists the natives return zeros
(INVENTED fallback); the null-field error path is exact.

## Exe evidence (cleaned pseudocode, renamed fields)
`FUN_004ecfc0(obj, "cursor")` = read object field "cursor" (null if unset).
`FUN_0047c640(ref, id, &blk)` = getInfo; it leaves `blk` untouched when the
resource does not answer (the exe then reads uninitialized stack).
`newVector3` = allocate `java.lang.Vector3` without running a constructor (x=y=z=0).

### 0x4df270 MouseCursor.getPos() -> Vector3
    cur = this.cursor;
    if (cur == null) { log("!" "Mighty Mouse ERROR 2"); return null; }
    getInfo(cur, 2, &blk);              // blk.x, blk.y
    v = newVector3(); v.x = blk.x; v.y = blk.y;   // z stays 0
    return v;

### 0x4df3b0 MouseCursor.getPos(Vector3 out) -> Vector3
    cur = this.cursor;
    if (cur == null) { log("!" "Mighty Mouse ERROR 2"); return null; }
    getInfo(cur, 2, &blk);
    out.x = blk.x; out.y = blk.y;       // out.z NOT written
    return out;

### 0x4def10 MouseCursor.getPickedPos() -> Vector3
    cur = this.cursor;
    if (cur == null) {
        log("!" "vm_mouse_getpickedpos: scripted resource " <id of this> " not found in file \""
            <script file> "\" at line " <line>);
        return null;
    }
    getInfo(cur, 0x3b, &blk);           // blk.x, blk.y, blk.z
    v = newVector3(); v.x = blk.x; v.y = blk.y; v.z = blk.z;
    return v;

### 0x4df100 MouseCursor.getPickedPos(Vector3 out) -> Vector3
    same null path as 0x4def10;
    getInfo(cur, 0x3b, &blk);
    out.x = blk.x; out.y = blk.y; out.z = blk.z;
    return out;

## Implementation notes
- Receiver is a[0]; the Vector3 arg (overloads) is a[1].
- Read the field with `vm.get_instance_field(self, "cursor", &v, &e)`; treat a failed read
  or `v.kind != Value::Obj || !v.obj` as the null path.
- Block source: `input::cursor_pos(&x, &y)` / `input::cursor_picked_pos(p)`.
  INVENTED: when they return false (no CursorSource), use zeros (exe would use stack garbage).
- New Vector3: existing `new_point(vm, "java.lang.Vector3", v, false, err)` with v = {x, y, 0}
  (getPos) or {x, y, z} (getPickedPos). Write into `out` with the existing `write_f`
  (getPos: only "x" and "y").
- Logging: print to stderr in the style of `file_log` (`std::fprintf(stderr, ...)`).
  INVENTED: getPickedPos prints only `vm_mouse_getpickedpos: scripted resource not found`
  (no resource id / script file / line are available to natives).
- Return `Value::make_null()` on the null path (not an error).

## Files
- edit: src/script/natives.cpp — add `#include "input/cursor.h"`; four natives with
  `// 0xADDR MouseCursor.name` comments; switch rows
  `getPickedPos ()Ljava.lang.Vector3;`, `getPos ()Ljava.lang.Vector3;`,
  `getPickedPos (Ljava.lang.Vector3;)Ljava.lang.Vector3;`, `getPos (Ljava.lang.Vector3;)Ljava.lang.Vector3;`
  to `<fn>, false`.
- read only: src/input/cursor.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
