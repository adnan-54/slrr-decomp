# 2.2.1 system script sources (`bin/2.2.1 sources/system/Scripts`, not in git)

69 `.java` files (8,180 lines) for the engine-side `java.*` packages: `io`, `lang`, `render` (+`osd`, `osd.dialog`), `sound`, `util`, `util.resource`.
The installed 2.3.1 game ships only `.class` for these, so this is the only readable source for them.

## Use
- They declare the `native` methods with parameter names and comments, so they document the interface behind `notes/06-native-methods.md` / `analysis/natives.tsv`.
- The non-native bodies are the reference for the rebuilt runtime library (Object, String, Vector, Thread, Osd gadgets, Config...).
- Conformance suite: compile each with the original game compiler (`JVM::compileSource`) vs. ours, and diff against the shipped `.class`.

## Cross-check against the 2.3.1 exe (name-level, from `tools/natives.py` vs. `native` declarations)
244 native names declared in 2.2.1 vs. 278 registered in 2.3.1. Most classes match exactly. Differences are version drift:
- **New in 2.3.1**: `Math.{atan2,exp,ln,log10,pow}`, `Sound` music API (`playTrack`, `setMode`, ...), `Steam.*`, `NetworkEngine.httpRequest[Async]`, `Replay.{readFile,writeFile}`, `Painter.*`, `Debug.{errorEX,giveWarning,writeFile}`, `Object.{runMethod,runDLL,hashCode,errorVM}`, `GroundRef` route/traffic/pedestrian queries, `ParticleSystem.*` and `Animation.*` (split out of `RenderRef`), `Vehicle.getSpeedSquare`, `GameLogic.kismajomCheck`.
- **Removed / moved**: `Controller.user_*`, `PhysicsRef.{getAngVel,getVel,setStatic}`, `System.net{Host,Join,Leave}`, `Camera.{activate,deactivate}`, `GameType.enterAsyncMode_Script`, `Input.{getPos,getPickedPos}` (now `MouseCursor`), `RenderRef` particle/animation methods (now `ParticleSystem`/`Animation`).
- 2.3.1 sources for the new natives must be inferred from the exe; the declarations for the unchanged ~85% can be taken from here.
