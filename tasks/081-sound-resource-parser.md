---
id: 081
status: merged
branch: card/081
depends: []
---
# audio: sound resource parser (res_sound .rsd text) + WAV reader + PCM decode

## Goal
First piece of the process-wide SFX service (notes/14 §2.6). Port the parsing half of the
exe's sound resource load: the `.rsd` key/value text of a type-6 (`res_sound`) record
(`Resource_Sound_Data__parse` 0x525d90 with its line reader `Str__readLine` 0x5281b0 and the
`CompatibleSound__ctor` 0x525b00 defaults), the RIFF/WAVE reader `Wav__parse` 0x5462a0, and a
decode of the WAV PCM into the software mixer's sample format (interleaved stereo int16).
Plus a corpus check over every shipped `.rpk` in the install. Nothing is wired to the
natives yet (cards 082-084).

Files (≤3 src files): `src/audio/sound.h` (new), `src/audio/sound.cpp` (new),
`src/tools/resman.cpp`. Build glue: add `audio/sound.cpp` to `LIB_SRC` and one `check`
line in `src/Makefile`.

## Exe evidence (Claude-traced, decompile + asm checked; notes/14 §2.6)

### 0x5281b0 Str__readLine(src, dst)
    n = 0;
    do { c = src[n]; dst[n] = c; n++; } while (c != '\n' && c != '\0');
    if (c == '\n') dst[n] = '\0';
    return n;                     // bytes consumed, '\n' included and kept in dst

### 0x525d90 Resource_Sound_Data__parse(data, size)   (ECX = data block, RI+0xb0)
    if (data == 0) return 0.0;
    volume = 1.0f; mindist = 5.0f /*0x40a00000*/; maxdist = 1000.0f /*0x447a0000*/;
    instances = 4; flags = 0; src[0] = 0;     // char src[256], key[64] (key never reset)
    p = data;
    do {
        p += Str__readLine(p, line /*global 0x7a7798*/);
        if (line[0] != 0) {
            sscanf(line, "%s", key);                       // fmt at 0x5a9020 = "%s"
            if      (!_stricmp(key, "sourcefile")) sscanf(line, "%*s %s", src);
            else if (!_stricmp(key, "volume"))     sscanf(line, "%*s %f", &volume);
            else if (!_stricmp(key, "mindist"))    sscanf(line, "%*s %f", &mindist);
            else if (!_stricmp(key, "maxdist"))    sscanf(line, "%*s %f", &maxdist);
            else if (!_stricmp(key, "instances"))  sscanf(line, "%*s %d", &instances);
            else if (!_stricmp(key, "flags"))      sscanf(line, "%*s %d", &flags);
        }
    } while (p - data < size);
    if (src[0] == 0) return 0.0;
    ... create CompatibleSound(src) and store volume/mindist/maxdist/instances/flags
        at +0x6c/+0x70/+0x74/+0x64/+0x68 (card 083) ...
    return 1.0;
Notes: `%d` stops at the '.', so "instances 2.000" → 2 and "0.500" → 0; a blank "\r\n"
line makes `sscanf("%s")` fail, `key` keeps the previous key and the second sscanf fails
too (value unchanged). Keys are case-insensitive (`_stricmp`, src has
`str_ieq` in natives.cpp; write a local ASCII helper in sound.cpp, do not include natives).

### 0x5462a0 Wav__parse(p, &bytes, &wfx, &pcm)   (u32 words; no bounds checks in the exe)
    if (p == 0 || p[0] != 'RIFF' /*0x46464952*/ || p[2] != 'WAVE' /*0x45564157*/) return -1;
    if (p[3] != 'fmt ' /*0x20746d66*/) return -2;            // fmt must start at byte 12
    *wfx = (byte*)p + 20;                                      // WAVEFORMATEX as stored
    q = (byte*)p + 20 + p[4];                                  // skip the fmt body
    while (*(u32*)q != 'data' /*0x61746164*/) q += 8 + ((u32*)q)[1];
    *bytes = ((u32*)q)[1]; *pcm = q + 8; return 0;
The buffer is then created from `wfx` as-is (DirectSound plays the stored format at its own
rate) and filled by `DSBuffer__fill` 0x5461e0, which fails when `pcm == 0` or `bytes == 0`.

## Changes

1. `src/audio/sound.h`:
   ```cpp
   // Sound resources (res_sound, record type 6): .rsd text and WAV data (notes/14 §2.6).
   #pragma once
   #include <stddef.h>
   #include <stdint.h>
   #include <string>
   #include <vector>
   namespace audio {
   // Resource_Sound_Data__parse 0x525d90 fields; defaults are CompatibleSound__ctor 0x525b00.
   struct SoundParams {
       std::string sourcefile;     // "%*s %s"
       float volume = 1.0f;        // CompatibleSound +0x6c
       float mindist = 5.0f;       // +0x70
       float maxdist = 1000.0f;    // +0x74
       int32_t instances = 4;      // +0x64
       int32_t flags = 0;          // +0x68 (bit0 set = 2D bus)
   };
   // 0x525d90 key loop. False when no sourcefile was read (exe returns 0.0).
   bool parse_sound_params(const char *data, size_t size, SoundParams *out);
   // Wav__parse 0x5462a0 result.
   struct WavView {
       uint16_t format_tag = 0, channels = 0, bits = 0;
       uint32_t rate = 0;
       const uint8_t *pcm = nullptr;
       uint32_t bytes = 0;
   };
   // 0: ok, -1: not RIFF/WAVE, -2: first chunk is not "fmt ".
   // INVENTED: -3 when a chunk header or the data range leaves the buffer (exe reads on).
   int parse_wav(const uint8_t *data, size_t size, WavView *out);
   // INVENTED representation of the DirectSound buffer for SoftwareMixer::set_sample:
   // interleaved stereo int16 at the file rate. PCM tag 1 only; 8-bit is unsigned
   // ((x - 128) << 8), 16-bit as is; mono is duplicated to both channels. Other formats
   // (one shipped 32-bit file) return false, like a failed CreateSoundBuffer (-2 bus).
   bool decode_pcm(const WavView &w, std::vector<int16_t> *stereo);
   }  // namespace audio
   ```
2. `src/audio/sound.cpp`: implement the three functions.
   - `parse_sound_params`: copy each line exactly like 0x5281b0 into a NUL-terminated
     `std::string` (stop after '\n' or at a NUL byte; INVENTED: also stop at `size`, the exe
     reads past the record end until '\n'/NUL). Loop `while (consumed < size)` (do-while: a
     size of 0 still reads one line, but return false at once when `data == nullptr`). Keep
     `char key[64] = ""` outside the loop; use `sscanf(line, "%63s", key)` and the value
     formats `"%*s %255s"`, `"%*s %f"`, `"%*s %d"` (INVENTED widths only guard the buffers).
     Only overwrite `out->sourcefile` from a successful `%s` read into a `char src[256]`
     initialised to "" before the loop; return `src[0] != 0`.
   - `parse_wav`: the 0x5462a0 steps with little-endian reads; read tag/channels/rate/bits
     from `wfx` (+0 u16 tag, +2 u16 channels, +4 u32 rate, +14 u16 bits). Bounds: need 20
     bytes for the header, `20 + fmt_size <= size`, each chunk header `q + 8 <= end`, and
     `pcm + bytes <= end` (else -3, INVENTED).
   - `decode_pcm`: false when `format_tag != 1`, `channels` not 1/2, `bits` not 8/16,
     `bytes == 0` or `pcm == nullptr` (0x5461e0). Frames = `bytes / (channels * bits / 8)`.
3. `src/tools/resman.cpp`: new mode `resman --sounds [GAME_DIR]` (default "bin/the game"),
   listed in `usage()`. Walk GAME_DIR recursively with `fs::list_dir` (skip "." and ".."; a
   name is a directory when `fs::list_dir` on it succeeds). For each `*.rpk` (case-insensitive)
   with a sibling `.rdb` (same stem, try `.rdb` and `.RDB`), open a fresh `ResourceManager`
   with that pair; count open failures and skip them. For every record with `type == 6` and
   `data_size > 0`:
   - `parse_sound_params(rpk.data(rec), rec.data_size)`;
   - cross-check with the shipped `.rdb`: find the `RdbResource` with `id == rec.id` and
     `has_rsd`; parse its `rsd` text the same way; `Checker`-fail if sourcefile, volume,
     mindist, maxdist, instances or flags differ (two shipped copies of the same data);
   - when a sourcefile was read, `mgr.resolve_path(sourcefile)`; count missing files; for an
     existing file `fs::read_file` + `parse_wav` (must return 0: fail otherwise) +
     `decode_pcm` (count successes and rejections by bits).
   Print totals: packages walked/opened, type-6 records with data, without sourcefile,
   missing files, WAV ok, decode ok, rejected. Return non-zero on any check failure.
   Expected on the install (measured with a Python pass over all 157 rpks, paired and
   unpaired): every existing referenced WAV passes `Wav__parse` (623 files: 617 x 16-bit,
   5 x 8-bit, 1 x 32-bit); the paired-only subset will be smaller. Do not hard-code counts;
   assert only "0 parse_wav failures" and "0 rpk/rdb mismatches".
   Add to `src/Makefile` `check:` after the resman selfcheck:
   `cd .. && src/$(BUILD)/resman --sounds "bin/the game"`.

## Out of scope
Bus allocation (082), the service/natives (083), frame hook and SfxItem owner (084).

## UNRESOLVED
- The exe resolves `sourcefile` through `File__readAll` 0x565610 → 0x545840 (file open,
  '/' → '\\'), i.e. relative to the working directory; src uses `ResourceManager::resolve_path`
  (package dir and ancestors), the convention all other src file refs use.
