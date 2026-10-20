# Native method table (java.* classes implemented in C++)

Extracted statically by `tools/natives.py` from `JVM::registerAllNatives` (0x4d5fb0, called once from 0x47cd20):
302 calls to `JVM::registerNative(class, method, signature, fnptr)` (0x55c690), 292 distinct implementations. Full table: `analysis/natives.tsv`.
These are the **only** ways script code reaches the engine; a rebuild must provide all of them. (The `vm_*` strings seen earlier are error-message prefixes inside these implementations, not registered names.)

## Dispatch (Thread::callMethod 0x561830, observed)
- Method record: `+4` flags (`0x8` static, `0x40` **native**), `+8` name (constant-pool idx), `+0x10` index into the class method-body table `[class+0x18]`, `+0x14` descriptor.
- Non-native: a new Frame (FUN_0054c180) is created, args popped from the caller's operand stack and stored as named `Variable` values in the callee locals (`this` is local 0 for instance methods); thread ip switches to the callee body, caller ip saved in `frame+0x2c`.
- Native: `hash = sum(method-name chars) * 0x49 + sum(class-name chars)`; bucket = `hash & 0x7f` in a 128-bucket table at `[vm+0xc]` (bucket = 12 bytes: key, chain head, next); entry = `NativeInfo` {vftable, vm, `+8` fn, `+0xc` class, `+0x10` method, `+0x14` signature}. Match on class, method and signature, so overloads exist (`System.exit()`, `exit(String)`, `analyze(Object|I|F)`).
- Args are popped (Variable refs copied to plain values), converted to the declared parameter descriptor (`assign` 0x55e020) and `java.lang.Native` objects are unwrapped to their raw payload.
- Call: `fn(&ctx)`, `ctx = {vm, thread, &args{cap,data,count}, nativeInfo}`. Return convention picked by the *return descriptor*: `F` → x87 float, `I`/`L`/`[` → EAX, `V` → void. The result is wrapped in a new `Value` and pushed.
- Errors: "native implementation missing for \"%s.%s\"", "instance method called with no instance".

## Thread::evalName (0x562280)
Walks the `0x1001` (local) / `0x1019` (class name = static access) / `0x101b` (cp field/method ref) chain for `a.b.c`: first element = local, class or `this`; later elements use the previous result (`null.methodcall()`, "field access on primitive variable" errors); a `0x40000000` flag on the resolved id means static.

## Natives per class

### java.game.GameLogic (1)
- `kismajomCheck([Ljava.lang.String;)I` → 0x4debd0

### java.game.Navigator (1)
- `updateNavigator(Ljava.util.resource.GameRef;I)V` → 0x4df470

### java.game.Painter (3)
- `paintPart(Ljava.util.resource.GameRef;I)V` → 0x4e0800
- `doPaint(Ljava.util.resource.GameRef;IIIFFI)V` → 0x4e0560
- `xPaint(Ljava.util.resource.GameRef;)V` → 0x4e07c0

### java.game.Vehicle (1)
- `getSpeedSquare()F` → 0x4e6960

### java.io.File (14)
- `open(I)I` → 0x4d9830
- `close()V` → 0x4d93b0
- `delete(Ljava.lang.String;)I` → 0x4d9520
- `copy(Ljava.lang.String;Ljava.lang.String;)I` → 0x4d94f0
- `move(Ljava.lang.String;Ljava.lang.String;)I` → 0x4d9800
- `exists(Ljava.lang.String;)I` → 0x4d9550
- `write(I)I` → 0x4d9af0
- `write(F)I` → 0x4d9af0
- `write(Ljava.util.resource.ResourceRef;)I` → 0x4d9b50
- `write(Ljava.lang.String;)I` → 0x4d9db0
- `readInt()I` → 0x4d95d0
- `readFloat()F` → 0x4d9580
- `readResID()I` → 0x4d9620
- `readString()Ljava.lang.String;` → 0x4d9740

### java.io.FindFile (3)
- `first(Ljava.lang.String;I)Ljava.lang.String;` → 0x4d9e90
- `next()Ljava.lang.String;` → 0x4d9fb0
- `close()V` → 0x4d9e40

### java.io.Input (11)
- `getAxis(II)F` → 0x4de990
- `getDeviceName(I)Ljava.lang.String;` → 0x4de9c0
- `mapAxis(Ljava.util.resource.GameRef;IIIFFFF)V` → 0x4dea20
- `setAxisSmooth(Ljava.util.resource.GameRef;IFFFF)V` → 0x4deab0
- `activeAxis(I)I` → 0x4de6d0
- `axisName(II)Ljava.lang.String;` → 0x4de700
- `lastKey()I` → 0x4de9f0
- `createHotkey(IILjava.io.Hotkey;Ljava.util.resource.GameRef;Ljava.render.Osd;I)V` → 0x4de880
- `deleteHotkey(Ljava.io.Hotkey;)V` → 0x4de920
- `checkHotkeys(Ljava.util.resource.GameRef;Ljava.render.Osd;)V` → 0x4de750
- `flushHotkeys()V` → 0x4de970

### java.io.MouseCursor (4)
- `getPickedPos()Ljava.lang.Vector3;` → 0x4def10
- `getPos()Ljava.lang.Vector3;` → 0x4df270
- `getPickedPos(Ljava.lang.Vector3;)Ljava.lang.Vector3;` → 0x4df100
- `getPos(Ljava.lang.Vector3;)Ljava.lang.Vector3;` → 0x4df3b0

### java.lang.Debug (3)
- `writeFile(Ljava.lang.String;Ljava.lang.String;)V` → 0x4e30d0
- `errorEX(Ljava.lang.String;)V` → 0x4d9350
- `giveWarning(Ljava.lang.String;)V` → 0x4d9380

### java.lang.Float (1)
- `toString(FLjava.lang.String;)Ljava.lang.String;` → 0x4da0e0

### java.lang.GameType (11)
- `remNotification(Ljava.util.resource.GameRef;I)V` → 0x4db6c0
- `addNotification(Ljava.util.resource.GameRef;IILjava.lang.String;)V` → 0x4da280
- `addNotification(Ljava.util.resource.GameRef;IILjava.lang.String;Ljava.lang.String;)V` → 0x4da140
- `addTimer(FI)V` → 0x4da3c0
- `removeAllTimers()V` → 0x4da930
- `setEventMask(I)V` → 0x4dc780
- `clearEventMask(I)V` → 0x4dc4d0
- `registerCallback(I)V` → 0x4dc740
- `unregisterCallback(I)V` → 0x4dc7c0
- `unregisterCallbacks()V` → 0x4dc800
- `createNativeInstance(Ljava.util.resource.GameRef;ILjava.lang.String;Ljava.lang.String;)V` → 0x4dc510

### java.lang.Integer (2)
- `toString(I)Ljava.lang.String;` → 0x4deb80
- `toHexString(I)Ljava.lang.String;` → 0x4deb30

### java.lang.Math (9)
- `random()F` → 0x4dee50
- `sqrt(F)F` → 0x4deee0
- `randomize()V` → 0x4dee90
- `setrandseed(I)V` → 0x4deeb0
- `ln(F)F` → 0x4decf0
- `log10(F)F` → 0x4decf0
- `exp(F)F` → 0x4decc0
- `pow(FF)F` → 0x4dee10
- `atan2(FF)F` → 0x4dec90

### java.lang.Object (13)
- `wait()V` → 0x4e0440
- `notify()V` → 0x4dfc70
- `notifyAll()V` → 0x4dfc90
- `toString()Ljava.lang.String;` → 0x4e03f0
- `disableGC()V` → 0x4dfb60
- `enableGC()V` → 0x4dfb80
- `hashCode()I` → 0x4dfc50
- `errorVM(Ljava.lang.String;)V` → 0x4dfba0
- `runDLL(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;[Ljava.lang.String;)Ljava.lang.String;` → 0x4dfcb0
- `runMethod(Ljava.lang.String;I)I` → 0x4dff40
- `runMethod(Ljava.lang.String;F)I` → 0x4dfda0
- `runMethod(Ljava.lang.String;Ljava.lang.Object;)I` → 0x4e00d0
- `runMethod(Ljava.lang.String;)I` → 0x4e0260

### java.lang.String (7)
- `finalize()V` → 0x4e4f70
- `token(ILjava.lang.String;)Ljava.lang.String;` → 0x4e5150
- `intValue()I` → 0x4e50d0
- `floatValue()F` → 0x4e4fc0
- `append(I)Ljava.lang.String;` → 0x4e4ed0
- `length()I` → 0x4e5110
- `getASCII()[I` → 0x4e5000

### java.lang.System (23)
- `exit()V` → 0x4e5550
- `exit(Ljava.lang.String;)V` → 0x4e5260
- `log(Ljava.lang.String;)V` → 0x4e5700
- `gc()I` → 0x4e55b0
- `runFinalization()I` → 0x461910
- `analyze(Ljava.lang.Object;)V` → 0x4e5240
- `analyze(I)V` → 0x4e5260
- `analyze(F)V` → 0x4e5260
- `arraycopy(Ljava.lang.Object;ILjava.lang.Object;II)V` → 0x4e5280
- `currentTime()F` → 0x4e5500
- `simTime()F` → 0x4e5810
- `timeWarp(F)F` → 0x4e5880
- `openLib(Ljava.lang.String;)I` → 0x4e0470
- `compileAll(Ljava.lang.String;)I` → 0x4e54c0
- `info(I)Ljava.lang.String;` → 0x4e55d0
- `buildNumber()I` → 0x4e5470
- `setLdPriority(I)V` → 0x4e56b0
- `syncGameTime(F)V` → 0x4e5820
- `setMeasure(F)V` → 0x4e57b0
- `getConfigOptions()V` → 0x4e55c0
- `isLoadingReset()V` → 0x4e56a0
- `isLoading()I` → 0x4e5690
- `memUsage()I` → 0x4e5740

### java.lang.Thread (10)
- `init(Ljava.lang.String;)V` → 0x4e5fb0
- `start()V` → 0x4e6170
- `stop()V` → 0x4e6200
- `isAlive()I` → 0x4e6040
- `getPriority()I` → 0x4e5f70
- `setPriority(I)V` → 0x4e60f0
- `sleep(F)V` → 0x4e6130
- `suspend()V` → 0x4e6250
- `resume()V` → 0x4e6070
- `setDaemon(I)V` → 0x4e60a0

### java.lang.Vector3 (6)
- `rotate(Ljava.lang.Ypr;)Ljava.lang.Vector3;` → 0x4e67e0
- `length()F` → 0x4e64c0
- `distance(Ljava.lang.Vector3;)F` → 0x4e62c0
- `normalize()V` → 0x4e6540
- `rotate(Ljava.lang.Vector3;F)Ljava.lang.Vector3;` → 0x4e6650
- `<init>(Ljava.lang.Ypr;)` → 0x4e6380

### java.lang.Ypr (2)
- `<init>(Ljava.lang.Vector3;)` → 0x4e7170
- `mul(Ljava.lang.Ypr;)V` → 0x4e7220

### java.net.NetworkEngine (2)
- `httpRequest(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;)Ljava.lang.String;` → 0x461910
- `httpRequestAsync(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.util.resource.GameRef;Ljava.lang.String;)I` → 0x461910

### java.render.Camera (3)
- `create(Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V` → 0x4d8de0
- `destroy()V` → 0x4d8fc0
- `setFog(IFF)V` → 0x4d9140

### java.render.GfxEngine (13)
- `forceRendering()V` → 0x4e5560
- `printScreen(Ljava.lang.String;)V` → 0x4e5780
- `numDisplayModes()I` → 0x4e5770
- `currDisplayMode()I` → 0x4e54f0
- `displayModeName(I)Ljava.lang.String;` → 0x4e5510
- `changeVideoMode(III)V` → 0x4e5480
- `setGlobalEnvmap(Ljava.util.resource.ResourceRef;)V` → 0x4dc910
- `flush()V` → 0x4dc840
- `renderOK()I` → 0x4dc900
- `openVideo(Ljava.lang.String;II)I` → 0x4dc870
- `openVideo(Ljava.lang.String;III)I` → 0x4dc8b0
- `closeVideo()V` → 0x4dc830
- `isPlayingVideo()I` → 0x4dc860

### java.render.Text (11)
- `setScale(FF)V` → 0x4e5d80
- `setDefaultColor(I)V` → 0x4e5cd0
- `getDefaultColor()I` → 0x4e58d0
- `create(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;FF)V` → 0x4e5b30
- `changeColor(I)V` → 0x4e5c40
- `changeAlign(I)V` → 0x4e5bb0
- `setPos(FF)V` → 0x4e5d00
- `getPos()Ljava.lang.Vector3;` → 0x4e58e0
- `update()V` → 0x4e5eb0
- `getWidthPixels(Ljava.lang.String;Ljava.util.resource.ResourceRef;)F` → 0x4e5ae0
- `getPos(Ljava.lang.Vector3;)Ljava.lang.Vector3;` → 0x4e5a20

### java.render.Viewport (11)
- `create(IFFFF)V` → 0x4e6af0
- `destroy()V` → 0x4e6c90
- `activate(I)V` → 0x4e6ab0
- `deactivate()V` → 0x4e6c50
- `getAspect()F` → 0x4e6db0
- `getWidth()F` → 0x4e6ee0
- `getHeight()F` → 0x4e6df0
- `getTop()F` → 0x4e6e90
- `getLeft()F` → 0x4e6e40
- `unproject(Ljava.lang.Vector3;I)Ljava.lang.Vector3;` → 0x4e6fb0
- `resize(FFFF)V` → 0x4e6f30

### java.sound.Sound (17)
- `changeMusicSet(I)V` → 0x4e48f0
- `nextTrack()V` → 0x4e4b10
- `prevTrack()V` → 0x4e4b20
- `firstTrack()V` → 0x4e4a30
- `randomTrack()V` → 0x4e4b30
- `playTrack(I)V` → 0x4e49b0
- `playTrack(Ljava.lang.String;)V` → 0x4e49e0
- `getTotalTracks()I` → 0x4e4a80
- `getNowPlaying()I` → 0x4e4a60
- `stopMusic()V` → 0x4e4c00
- `getMode()I` → 0x4e4a40
- `setMode(I)I` → 0x4e4b40
- `setVolume(IF)V` → 0x4e4b80
- `getVolume(I)F` → 0x4e4aa0
- `has3DHardware()I` → 0x4e4af0
- `hasMixHardware()I` → 0x4e4b00
- `enableDebugDump(Ljava.util.resource.ResourceRef;)I` → 0x4e4a10

### java.util.Replay (2)
- `writeFile(Ljava.lang.String;Ljava.lang.String;)V` → 0x4e30d0
- `readFile(ILjava.lang.String;Ljava.lang.String;)F` → 0x4e2f00

### java.util.Steam (5)
- `initAPI()I` → 0x4e4c80
- `neutralize()V` → 0x4e4d40
- `getPackage(I)Ljava.lang.String;` → 0x4e4c10
- `setAchievement(Ljava.lang.String;)V` → 0x4e4e00
- `resetAchievements()V` → 0x4e4d50

### java.util.resource.Animation (9)
- `<init>(Ljava.util.resource.RenderRef;Ljava.util.resource.ResourceRef;)` → 0x4d8a40
- `finalize()V` → 0x4d8980
- `setSpeed(F)V` → 0x4d8d70
- `setFade(F)V` → 0x4d8d00
- `getPos()F` → 0x4d89e0
- `seek(F)V` → 0x4d8c90
- `play()V` → 0x4d8c30
- `loopPlay()V` → 0x4d8b70
- `pause()V` → 0x4d8bd0

### java.util.resource.GameRef (23)
- `create(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.String;Ljava.lang.String;)Ljava.lang.GameType;` → 0x4da620
- `create_native(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.String;Ljava.lang.String;)V` → 0x4da7e0
- `getFlags()I` → 0x4daa40
- `setFlags(I)V` → 0x4db900
- `clearFlags(I)V` → 0x4da5e0
- `getPos()Ljava.lang.Vector3;` → 0x4e3890
- `getOri()Ljava.lang.Ypr;` → 0x4dac40
- `getVel()Ljava.lang.Vector3;` → 0x4db050
- `getInfo(II)I` → 0x4daa80
- `getInfo(ILjava.lang.String;)I` → 0x4daa80
- `queueEvent(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V` → 0x4db590
- `setParent(Ljava.util.resource.ResourceRef;)V` → 0x4dbbe0
- `setPos(Ljava.lang.Vector3;)V` → 0x4dbd00
- `setMatrix(Ljava.lang.Vector3;Ljava.lang.Ypr;)V` → 0x4db950
- `setState(Ljava.lang.Vector3;Ljava.lang.Ypr;Ljava.lang.Vector3;Ljava.lang.Vector3;)V` → 0x4dbf20
- `isEmpty()I` → 0x4db3b0
- `isScripted(Ljava.lang.String;)I` → 0x4db460
- `getScriptInstance()Ljava.lang.Object;` → 0x4daf90
- `setActiveCollision()V` → 0x4db7f0
- `getDetail()F` → 0x4e3330
- `getPos(Ljava.lang.Vector3;)Ljava.lang.Vector3;` → 0x4e3a80
- `getOri(Ljava.lang.Ypr;)Ljava.lang.Ypr;` → 0x4dae20
- `getVel(Ljava.lang.Vector3;)Ljava.lang.Vector3;` → 0x4db240

### java.util.resource.GroundRef (30)
- `setWater(FFF)V` → 0x4dc400
- `setWater(Ljava.lang.Vector3;Ljava.lang.Vector3;FF)V` → 0x4dc2b0
- `addWaterLimit(Ljava.lang.Vector3;Ljava.lang.Vector3;)V` → 0x4da500
- `setFog(IFF)V` → 0x4de640
- `addTrafficCar(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I` → 0x4dd090
- `remTrafficCar(I)V` → 0x4de520
- `notifyTrafficCar(II)I` → 0x4de5f0
- `addTrafficN(Ljava.util.resource.GameRef;IFFF)I` → 0x4dc9e0
- `addTrafficP(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I` → 0x4dcdc0
- `delTraffic()V` → 0x4dd840
- `setTrafficCarBehaviour(II)V` → 0x4e6280
- `haltTrafficCross(Ljava.lang.Vector3;F)V` → 0x4de360
- `haltTrafficPath(Ljava.lang.Vector3;Ljava.lang.Vector3;)V` → 0x4dd700
- `haltTrafficRoad(Ljava.lang.Vector3;Ljava.lang.Vector3;)V` → 0x4de270
- `clearHalts()V` → 0x4dd6c0
- `setTrafficCrossStopOffset(F)V` → 0x4dd7f0
- `getNearestCross(Ljava.lang.Vector3;F)Ljava.lang.Vector3;` → 0x4dd880
- `getStartDirection(Ljava.lang.Vector3;Ljava.lang.Vector3;)Ljava.lang.Vector3;` → 0x4ddfb0
- `getRouteLength(Ljava.lang.Vector3;Ljava.lang.Vector3;)F` → 0x4ddc50
- `alignToRoad(Ljava.lang.Vector3;)[Ljava.lang.Vector3;` → 0x4dd470
- `findRoute(Ljava.lang.Vector3;Ljava.lang.Vector3;)F` → 0x4ddc50
- `getRouteLength()F` → 0x4ddc30
- `getRoutePos(F)Ljava.lang.Vector3;` → 0x4dde50
- `getRouteDist(Ljava.lang.Vector3;)F` → 0x4ddb50
- `setPedestrianDensityN(F)V` → 0x4de580
- `addPedestrianType(Ljava.util.resource.GameRef;)V` → 0x4dc940
- `removePedestrianType(Ljava.util.resource.GameRef;)V` → 0x4de4d0
- `pedestrianDistance(Ljava.lang.Vector3;I)F` → 0x4de420
- `getTrafficCarPos(ILjava.lang.Vector3;)V` → 0x4de1c0
- `getPedestrianPos(Ljava.lang.Vector3;Ljava.lang.Vector3;I)V` → 0x4dda10

### java.util.resource.ParticleSystem (9)
- `init(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V` → 0x4e0ad0
- `stop()V` → 0x4e1440
- `setFreq(F)V` → 0x4e10c0
- `setDirectSource(Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V` → 0x4e0de0
- `setSource(Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V` → 0x4e1160
- `delAction(Ljava.lang.String;)V` → 0x4e0970
- `setCounter(Ljava.lang.String;Ljava.lang.Vector3;F)V` → 0x4e0bf0
- `getCounter(Ljava.lang.String;)I` → 0x4e0a10
- `modePermanent(I)V` → 0x4e0b30

### java.util.resource.PhysicsRef (6)
- `create(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V` → 0x4e1470
- `createBox(Ljava.util.resource.ResourceRef;FFFLjava.lang.String;)V` → 0x4e16a0
- `createSphere(Ljava.util.resource.ResourceRef;FLjava.lang.String;)V` → 0x4e1880
- `setMatrix(Ljava.lang.Vector3;Ljava.lang.Ypr;)V` → 0x4e1e40
- `getPos()Ljava.lang.Vector3;` → 0x4e1c40
- `getOri()Ljava.lang.Ypr;` → 0x4e1a40

### java.util.resource.RenderRef (14)
- `create(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V` → 0x4e2140
- `getBoneId(Ljava.lang.String;)I` → 0x4e2290
- `setMatrix(ILjava.util.resource.ResourceRef;Ljava.lang.Vector3;Ljava.lang.Ypr;)V` → 0x4e2a50
- `setMatrix(Ljava.lang.Vector3;Ljava.lang.Ypr;)V` → 0x4e2c80
- `changeResource(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)V` → 0x4e2090
- `setColor(I)V` → 0x4e2830
- `setLight(III)V` → 0x4e29f0
- `setFlare(Ljava.util.resource.ResourceRef;IFFII)V` → 0x4e2950
- `setType(Ljava.util.resource.ResourceRef;)V` → 0x4e2eb0
- `getTypeID()I` → 0x4e23b0
- `getDetail()F` → 0x4e3330
- `lineCreate(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)I` → 0x4e25d0
- `lineAdd(Ljava.lang.Vector3;Ljava.lang.Vector3;IF)V` → 0x4e24f0
- `plotRoute(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;IFLjava.lang.Vector3;)I` → 0x4e2640

### java.util.resource.ResourceRef (20)
- `deleteNative()V` → 0x4e3220
- `newNative()V` → 0x4e4080
- `set(I)V` → 0x4e43a0
- `set(Ljava.util.resource.ResourceRef;)V` → 0x4e4350
- `load()V` → 0x4e3de0
- `unload()V` → 0x4e4500
- `destroy()V` → 0x4e32b0
- `type()I` → 0x4e43e0
- `id()I` → 0x4e3cd0
- `getParentID()I` → 0x4e36b0
- `getParent()Ljava.util.resource.ResourceRef;` → 0x4e37c0
- `getWTRoot()Ljava.util.resource.ResourceRef;` → 0x4e3c00
- `getFirstChild()Ljava.util.resource.ResourceRef;` → 0x4e3510
- `getNextChild()Ljava.util.resource.ResourceRef;` → 0x4e35e0
- `makeTexture(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V` → 0x4e3ff0
- `makeSound(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V` → 0x4e3f60
- `cache()V` → 0x4e3100
- `precache()V` → 0x4e40f0
- `duplicate(Ljava.util.resource.ResourceRef;)V` → 0x4e32e0
- `scaleMesh(FFF)V` → 0x4e4210

### java.util.resource.SfxRef (2)
- `nplay(Ljava.lang.Vector3;FFFII)I` → 0x4e4610
- `stop(I)V` → 0x4e47d0

## Native implementation conventions (decompiled: Math, System basics)
- Every native takes the single `ctx` pointer (see above). `FUN_004b1af0(ctx, 0, &a, &b, ...)` (named `Native__getArgs`) unpacks the argument list into C locals by the registered signature (int/float/pointer slots, in order). The `0` is presumably the first-arg index.
- Script errors from natives use the shared reporter: message prefix `vm_<class>_<method>: ...`, then ` in file "<script>" at line <n>` from `[vm+0x74]` / `[vm+0x70]` (this is the origin of the `vm_*` strings).
- Natives are plain cdecl functions, many just a few instructions (some have no Ghidra function because they are tail-merged/undefined code, e.g. 0x4e5500, 0x4e5550, 0x4e5470; decompile via disassembly there).
- Several registrations share one stub: `exit(String)`, `analyze(int)`, `analyze(float)` → 0x4e5260 (parse args, do nothing); `runFinalization`, `NetworkEngine.httpRequest[Async]` → 0x461910 (`return 0`, i.e. disabled); `Replay.writeFile` = `Debug.writeFile` → 0x4e30d0; `GroundRef.getRouteLength` = `findRoute` → 0x4ddc50.
- **Quirk to preserve**: `Math.ln` and `Math.log10` share 0x4decf0 — both compute the *natural* log (errors on `x <= 0` with "vm_math_ln: incorrect argument %f").

### java.lang.Math
| method | impl | behaviour |
|---|---|---|
| `random()` | 0x4dee50 | CRT `rand()` & 0x7fff, mapped to `[0,1)` by `x * 3.051851e-05` (=1/32768); 0x7fff is decremented first so 1.0 is never returned |
| `randomize()` | 0x4dee90 | `srand((int)timeGetTime-like ms clock FUN_00569730)` |
| `setrandseed(int)` | 0x4deeb0 | `srand(seed)` |
| `sqrt(F)` | 0x4deee0 | x87 `fsqrt` (no domain check) |
| `exp(F)` / `pow(F,F)` / `atan2(F,F)` | 0x4decc0 / 0x4dee10 / 0x4dec90 | CRT `exp`, `pow`, `fpatan(a, b)` (arg order as pushed: y, x) |
| `ln(F)`, `log10(F)` | 0x4decf0 | natural log; error for x ≤ 0 |

### java.lang.System (partial)
- `log(String)` 0x4e5700: writes the string to the game log (`FUN_005695c0`), `"<null>"` for null.
- `gc()` 0x4e55b0 → `FUN_00559e30` (JVM garbage/finalize pass); `runFinalization()` 0x461910 returns 0.
- `simTime()` 0x4e5810 returns the float at `*PTR_DAT_005dadf0` (level-2 simulation time).
- `timeWarp(F m)` 0x4e5880: if `m >= 0` calls `FUN_0047dfd0(m)` (sets sim time speed), returns `DAT_005fe10c` (previous/current warp).
- `arraycopy` 0x4e5280: bounds check against both arrays' `length` field ("System.arraycopy: copy out of bounds"), then element-wise copy (temp buffer when src == dst).

### java.util.resource.PhysicsRef (0x4e1470 create, 0x4e16a0 createBox, 0x4e1e40 setMatrix, 0x4e1c40 getPos)
- Script arg 0 is the `PhysicsRef` Instance; `FUN_004ecc70(instance, DAT_007a2620)` converts it to the native handle (`DAT_007a2620` = the PhysicsRef script class). Null handle → "vm_physicsref_<method>: scripted resource %x not found in file ...".
- `create(ResourceRef res, int, String)` / `createBox(res, sx, sy, sz, name)`: builds a physics body (`FUN_004f7b50` generic / `FUN_004f7d60(0, 5, hx, hy, hz, 0)` box with **half extents** = size*0.5), sets flag bit 0 at `body+0xb4`, then `FUN_004e8a50(handle, body, ...)` attaches it to the scripted resource (a linked list in `[res+0x78]`).
- `body+0xb4` → physics state block with **two pose buffers of stride 0xd4** at `+0xa4 + idx*0xd4` (matrix) and `+0x64 + idx*0xd4` (position xyz), `idx = [state+0x20c]` (0/1). `setMatrix` writes the position into **both** buffers and the orientation (Ypr → matrix `FUN_005666d0/FUN_00567ea0`) into the active one, then `FUN_00504180(state)` (refresh). `getPos` reads the active buffer and allocates a `java.lang.Vector3` Instance, setting its three float fields by name (`FUN_004ed080`). So the physics thread/step is double-buffered against the render side.
- Vector3/Ypr script objects are read field-by-field with `FUN_004ec980(instance, "x"/"y"/"z" | "yaw"/"pitch"/"roll")` and written with `FUN_004ed080`.
