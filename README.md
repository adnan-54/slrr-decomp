# slrr-re

Reverse-engineering notes for Street Legal Racing Redline 2.3.1 (StreetLegal_Redline.exe, PE32 i386).

Target sha256: f44fb88a4335c657e20701cdb84afb8d1d85f7c3aceb93d30113e6e6e44fefc2

Tooling: [rea](https://github.com/morluto/rea) + Ghidra 12.1.4 (headless).

Binary lives in bin/ (gitignored). Copy your own from the game install.

    rea analyze $PWD/bin/StreetLegal_Redline.exe --provider ghidra --snapshot $PWD/analysis/slrr.json
