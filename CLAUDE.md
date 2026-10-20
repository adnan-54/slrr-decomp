Target: bin/StreetLegal_Redline.exe (SLRR 2.3.1, 32-bit PE). Use the rea MCP tools with provider ghidra.
Write findings to notes/. Never commit anything in bin/.
Sample data: bin/system.rpk + bin/system.rdb (game resource package + its database). Use them to confirm the file formats the exe loader parses.
Sample script: bin/Init.class + bin/Init.java (game/Init, script entry point, compiled form + its source). Use the pair to decode the .class bytecode format the exe VM loads (magic "TUFA").
Sample cfg: bin/FL_door.cfg (car part config from cars/racers/Axis_data/scripts: render/mesh/body/slot/attach). Use it to find and verify the exe cfg parser.
Sample script: bin/Axis_FL_door.class + .java (smallest pair, part script for bin/FL_door.cfg). Start .class format decoding here.
