#!/usr/bin/env bash
# Verify the environment and print the resume state. Usage: tools/checkpoint.sh
cd "$(dirname "$0")/.." || exit 1
ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [MISSING] %s\n' "$1"; }
echo "== git"; git log --oneline | head -5; git status --short | head -10
echo "== inputs (bin/, not in git)"
[ -f "bin/the game/StreetLegal_Redline.exe" ] && ok "game exe" || bad "bin/the game/StreetLegal_Redline.exe"
[ "$(sha256sum "bin/the game/StreetLegal_Redline.exe" 2>/dev/null | cut -c1-16)" = "f44fb88a4335c657" ] && ok "exe sha256 matches" || bad "exe sha256 (expected f44fb88a4335c657...)"
[ -d "bin/tools/SlrrLib" ] && ok "bin/tools/SlrrLib" || bad "bin/tools/SlrrLib"
[ -d "bin/2.2.1 sources/system/Scripts" ] && ok "2.2.1 sources" || bad "bin/2.2.1 sources"
echo "== toolchain"
for t in python3 gcc git; do command -v $t >/dev/null && ok "$t" || bad "$t"; done
[ -x /opt/dotnet/dotnet ] && ok "dotnet (/opt/dotnet)" || bad "dotnet at /opt/dotnet (see CHECKPOINT.md)"
echo "== analysis data"
for f in analysis/names.json analysis/natives.tsv analysis/procs.tsv analysis/strings.tsv; do [ -f $f ] && ok "$f" || bad "$f"; done
python3 - <<'PY'
import json
print("  names.json entries:", len(json.load(open('analysis/names.json'))))
PY
echo "== notes"; ls notes | sed 's/^/  /'
echo "== src build check"
mkdir -p /tmp/ckpt && printf '#include "engine/math.h"\nint main(){Quat q={0,0,0,1};quat_normalize(&q);return q.w==1.0f?0:1;}\n' > /tmp/ckpt/t.c \
  && gcc -I src -o /tmp/ckpt/t /tmp/ckpt/t.c -lm 2>/dev/null && /tmp/ckpt/t && ok "src/engine/math.h compiles and passes smoke test" || bad "src math smoke test"
echo; echo "Next: read CHECKPOINT.md ('State' and 'Next-steps queue'); notes via notes/INDEX.md, history in notes/LOG.md; open the exe with rea open_binary and re-apply analysis/names.json."
