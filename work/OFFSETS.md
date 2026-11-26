# OneShot offsets port — libblackrussia-client.so

Target: APK launcher.apk, package `com.br.top`, versionName **16.116.0** (versionCode 1611).
Lib (v7a): `apk-decoded/lib/armeabi-v7a/libblackrussia-client.so`
- .text 0x4b0980..0x14c9780 (Thumb-2), .rodata 0x328e00..0x4b0980
- .data.rel.ro 0x14d4140..0x156cf38, .data/.bss -> end ~0x2890380
- PLT map: disasm/plt_stub2sym.txt ; funcs: disasm/funcs.txt (exidx, 59234)
- xref idx: disasm/idx_bl.txt (caller->target), idx_data.txt (insn->literal/movwt val), idx_picref.txt (ldr->slot->content)
- strings: disasm/new_strings.txt ; tools: tools/wb.py (dis/xref/funcrefs/str/syms/fn)

Conventions: cheat uses `libAddr + off` (or +1 for Thumb call targets / MSHook targets are function starts).

## DONE
| old | new | what |
|-----|-----|------|
| 0x41AD5C | 0xECCD54 | Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent (dynsym export; hook target) |

## NETWORK / RakNet (exported — easy)
- RakClient::peer (singleton) = 0x100EB48  (W sym)
- RakClient::RPC(bs) = 0x100D634 ; RakClient::RPC(raw) = 0x100D54C
- RakClient::Send(raw) = 0x100CF14 ; RakClient::Send(bs) = 0x100CFF8
- RakClient::RegisterAsRemoteProcedureCall = 0x100D40C
- RPCMap::AddIdentifierWithFunction = 0x1014632 (PLT stub 0x14cc180; sole callsite func 0x1018988)
- RakPeer::Send = 0x1010100/0x1010260, RakPeer::RPC = 0x1010B54/0x1010C20

## TODO — cheat lib offsets (old -> TBD)
Globals (bss/data):
- 0x825AB8 g_NetGame (passed as arg to NetGame methods) — start: JNILib chain 0x21c5016 bss struct (getPlayerId)
- 0x4B624EC g_pPlayerPool, 0x4B624F0 g_pVehiclePool, 0x4B62508 g_pObjectPool
- 0x4B6263D packed time (TimeChanger writes int), 0x4B62628 radar zoom dist?, 0x6F9410 g_localPed
- 0x464507C CTimer ms_nMilliseconds? (timer.cpp reads int) , 0x464508C CTimer::ms_fTimeScale (GameSpeed)
- 0x48E6CC4 camera matrix ptr, 0x496F61C CModelInfo::ms_modelInfoPtrs, 0x46E6700 colmodel array base (main.h uses libAddr+0x46E6700+4*modelidx)+28
- 0x459E298 model load-state array (+28*i+8 byte flag), 0x459C400/0x459C404 view pos?
- 0x7B7268/0x7B726C/0x7B726D camera position x,y,z
- 0x825CEC/0x825CFC aim/view angles, 0x8264BA byte flag (CarChanger), 0x59D8C4/0x59D8B4 aim angles
- 0x5EAC70/0x5EAC80 map/radar x,y , 0x43BE5CC some int global (GetGameState?)
- 0x392FDC jump-velocity global float (WriteMemoryFloat, LongJump)

Functions:
- 0x2C9F19 CGame::Process hook — via JNILib_step 0xecc8cc chain
- 0x3CC6BD ChatWindowInputHandler — gui chat
- 0x2EFC25 CWorld::GetIsLineOfSightClear
- 0x3B0B45 CSprintCalculator, 0x3A8CF9 CPedDamageCalculator, 0x4610B5 CVehicleDamageCalculator, 0x44F511 NoBikeCalculator
- 0x380D5D CPed::ProcessBuoyancy, 0x44C4F1 CBike::ProcessBuoyancy, 0x43C9A1 CAutomobile::ProcessBuoyancy
- 0x482BF5 log_3
- 0x3695B1 ScrServerJoin, 0x369675 ScrServerQuit, 0x368713 ScrServerNewPlayer, 0x3691C1 ScrInitGame, (0x36979D ScrRequestSpawn, 0x1F9329 ScrServerDeletePlayer, 0x1FA270 ScrConnectionRejected — commented out)
- 0x3BC300/0x3BC301 AddChatMessage (call + hook)
- 0x3447C1 SendChatMessage, 0x34487D SendChatCommand — via JNILib_sendChatMessage 0xecd4d4 internals
- 0x2F2689 FindGroundZ (CWorld::FindGroundZForCoors)
- 0x368702/0x368703 CPlayerPool::GetAt, 0x374378/0x374379 CVehiclePool::GetAt, 0x374389 CVehiclePool::New, 0x3681BA CObjectPool::GetAt
- 0x3AC93B GetBonePosition, 0x37DA34/0x37DA35 SetMoveAnim (ped.cpp calls gameLibAddr+0x37da34+1)
- 0x2D7B21 fn(Vec3*) [coors transform], 0x38CA41 fn(localped,x,y,z) [SetPosn/Teleport]
- 0x316C37 fn(veh+4,angle) [SetHeading], 0x3169C9 fn(veh+4) [FixVehicle?], 0x391085 fn(localped,veh,0) [PutInVehicle]
- 0x2BA95F attach trailer fn(trailer,truck,1)
- 0x20CDA7 fn(localped) [DeleteRwObject], 0x20CC59 fn(localped,skinID) [SetModelIndex], 0x20DCF4 fn(localped) [?]
- 0x2E712D fn(modelID,8) [RequestModel], 0x2E99FD fn(1) [LoadAllRequestedModels]
- 0x2A6B66/0x2AA6C8/0x5D9E1C fn(g_NetGame) — NetGame methods (UpdatePools/etc.)
- 0x45D485 fn(vehicle,-16777216,0,0,0) [SetColor?]
- 0x20DCF4,0x1A9EEE,0x1AA01A,0x1B3910,0x1BD7D0,0x1DED50,0x1FA270,0x203FF0,0x28D808,0x32301D (mostly commented-out hooks)

## Anchors (string va -> meaning)
- 0x32d642 NetGame.cpp ; 0x3319c6 RPCs.cpp ; 0x329e8b ActorPool.cpp ; 0x359bf1 PlayerPool.cpp
- 0x330057 entities/Physical.cpp/.h(0x33f281) ; 0x343850 peds/Ped.cpp ; 0x3478aa vehicles/Vehicle.cpp
- 0x32d642 NetGame.cpp ; 0x32c62a core/Cam.cpp ; 0x343c36 weapons/Weapon.cpp
- assert strings are a PACKED BLOB (path\0funcname\0 pairs) — NOT directly referenced; use as identity map once index->(file,func) scheme is decoded (or find the log/assert fn signature).
- Real format-string refs DO work via PIC chain (e.g. 'invalid phys state...' 0x3300a8 -> func 0xa8d5e0)

## v7a RESOLVED (16.116.0, verified by semantics/RPC-id)
### Chat
- send dispatcher = 0xda29a0 ; SendChatMessage = 0xda2a60 (RPC 0x19e/414) ; SendChatCommand = 0xda29c8 (RPC 0x17a/378)
- Chat::AddClientMessage(color,str) = 0x5db530  (old 0x3BC300/301 call+hook)
- arm64 twins: dispatcher 0x15d9944 (kolson hook!), SendChatMessage 0x15d9a7c, SendChatCommand 0x15d9990
- JNILib_sendChatMessage $_0::__invoke: v7a ref via strd[r6]; arm64 invoke = 0x1747dc8 -> tail b 0x15d9944

### Pools / NetGame (v7a)
- g_CNetGame object VA = 0x21c5030 ; m_pPlayerPool = *(0x21c5030+0)
- app ctx struct = 0x21c5016 : +0x10 RakClient obj ptr, +0x14 conn state (14=connected)
- CNetGame::m_playersColors base = 0x21bea20 (stride 0x10)
- CPlayerPool::GetLocalPlayer = 0xdb3236  (return *(pool+0))
- CPlayerPool::GetAt(pool,id) = 0xdb323a  (arr at pool+4, id bound (id>>5)<=0x2e)
- CPlayerPool::GetPedById = 0xdb324a
- CPlayerPool::New(pool,name,id) = 0xdb32e0
- CPlayerPool::Delete(pool,id) = 0xdb3080
- CLocalPlayer::Spawn = 0xd93230 ; CNetGame::ConnectVoice = 0xda2f48
- arm64 twins: GetLocalPlayer 0x15ee3fc, Spawn 0x15c75e0, ConnectVoice 0x15da290, g_pPlayerPool slot 0x34e7b30, rakclient 0x34e7b20, state 0x34e7b28

### RPC handlers (v7a) — table VA 0x151edb0, 125 x {u32 id, u32 fn|1}
- ServerJoin  id401 idx91  = 0xdb5338  (calls CPlayerPool::New + m_playersColors)
- ServerQuit  id326 idx74  = 0xdb546c  (calls CPlayerPool::Delete)
- WorldPlayerAdd    id419 idx89 = 0xdb5660
- WorldPlayerRemove id325 idx90 = 0xdb59d0
- InitGame    id421 idx117 = 0xdb4ddc
- QueueGame   id358 idx115 = 0xdb51dc
- UpdateQueuePos id450 idx119 = 0xdb51f4
- ClientMessage id360 idx58 = 0xdb54fc
- RequestSpawn id344 idx39 = 0xdb563c
- ConnectionRejected id319 idx111 = 0xdb5c30
- full table: disasm/RPC_TABLE.md ; decompile name-by-id table verified for these

### old-hook mapping (v7a)
- 0x3695B1 ScrServerJoin -> hook 0xdb5338 (or RPC dispatch)
- 0x369675 ScrServerQuit -> 0xdb546c
- 0x368713 ScrServerNewPlayer -> 0xdb5660
- 0x1F9329 ScrServerDeletePlayer -> 0xdb59d0
- 0x3691C1 ScrInitGame -> 0xdb4ddc
- 0x36979D ScrRequestSpawn -> 0xdb563c
- 0x1FA270 ScrConnectionRejected -> 0xdb5c30
- 0x3447C1 SendChatMessage -> 0xda2a60 ; 0x34487D SendChatCommand -> 0xda29c8
- 0x3BC300/301 AddChatMessage -> 0x5db530

## Tools fixed
- v7a PLT fully re-resolved: disasm/plt_v7a.json (2457 stubs, verified via .rel.plt order)
- sig7 rebuilt: disasm/sig7_callees.pkl (25140 funcs, callee sym-name sets)
- sig64: disasm/sig64_callees.pkl (24299)
- v7a RPC table extraction: records {id,fn} stride 8 at 0x151edb0

## Source update pass (this session) — main.cpp
Applied (verified v7a):
- AddChatMessage 0x5DB530+1 ; SendChatMessage 0xDA2A61 ; SendChatCommand 0xDA29C9
- FindGroundZ = 0x5B1339 (CWorld::FindGroundZForCoord; found via RPC416 handler 0xdbaec4: reads 3 floats, calls it, +1.5, then local-ped getter 0x58ded0 + vcall [ped]+0x40 = CEntity::SetPosition)
- CPlayerPool_GetAt = 0xDB323B
- multiTouchEvent JNI = 0xECCD54+1
- ScrServerJoin hook -> 0xDB5339 ; ScrServerQuit -> 0xDB546D ; ScrServerNewPlayer -> 0xDB5661 (WorldPlayerAdd) ; ScrInitGame -> 0xDB4DDD ; AddChatMessage_hook -> 0x5DB531

DISABLED (unverified, will crash if hooked): CGame::Process, GetIsLineOfSightClear, ChatWindowInputHandler, CSprintCalculator, CPedDamageCalculator, CVehicleDamageCalculator, NoBikeCalculator, ProcessBuoyancy x3, log_3.
Still-old (on-demand fn ptrs, TODO): CVehiclePool_GetAt/New, GetBonePosition, SetMoveAnim. Globals in main.h untouched.

## ABI WARNING
New client RPC handlers ALL take RPCParameters* (single arg). Cheat hooks like ScrServerNewPlayer(pool,nickname,playerID) assumed old samp-style args — hook bodies must be rewritten to parse BitStream (nickname, id) or the feature breaks (not a crash, just wrong data).

## Next-session targets
- CGame::Process / Idle (per-frame hook, critical): v7a RsEventHandler=0xed513c calls Idle in rsIDLE case; mainThread=0xec9f5f, RsEventHandler called w/ event id 0x26 in state-6 loop. Idle calls CGame::Process + CTimer::Update + renderQueue flush.
- GetIsLineOfSightClear: 9-arg fn calling only GetIsLineOfSightSectorClear; arm64 namemap cands 0xbaa108/0xbaa1d8 (unverified).
- Buoyancy x3, damage/sprint calculators: decomp vas known (CPed::ProcessBuoyancy decomp 0x18e42a4, CAuto 0x1a17564, CBike 0x1a28ba8) — decomp->ours arm64 window via anchors64.json (now 1355 anchors incl. 119 RPC-derived pairs).
- CNetVehiclePool::GetAt/New decomp 0x18bf3c0/0x18bf3e0.
- CPed::SetMoveAnim decomp 0x18e0590.

## Pass 2 (this session)
- mainThread v7a = 0xec9f5e(Thumb); RsEventHandler = 0xed513c; per-frame chain: ReEventProcess=0xed4178, Input_Process=0xed47fc (called at init AND every frame in state 6) → CGame_Process hook SUBSTITUTED to 0xED47FD. Real CGame::Process still unresolved.
- Local ped accessor = 0x58ded0 (g_localPlayerId u16 @ 0x1fed4e2 → ped array @ 0x34e801fe, stride 0xF0). FindLocalPlayer now calls it.
- GetPlayerPool: g_NetGame obj @ 0x21c5030, m_pPlayerPool=[obj+0]. GetAtPlayerPool -> 0xDB323A.
- Ped position fields: NEW layout CEntity:CPlaceable(base-first, no vptr) => m_matrix.px/py/pz at +0x30/+0x34/+0x38 (was 0x34/0x38/0x3C w/ vptr-first). Applied to all literal+OBFUSCATE sites. RUNTIME VERIFY REQUIRED — if coords wrong, flip back.
- Vehicle/object pools: holder slot resolves to 0x5034021c (out-of-range static value — likely relocated/obfuscated at runtime). TODO: read at runtime or find offset inside g_NetGame.
- Vehicle-pool GetAt candidate seen in handlers: 0xdd98ce (used by ScrVehicleNitro/ScrSetVehiclePos with (pool,id)).

## Session: APK packaging + first install test (2026-11-23)

### Built artifact
- `oneshot.apk` (121 MB) — signed debug keystore, zipaligned. Contains `lib/armeabi-v7a/liboneshot.so`.
- Injection point: patched `JNILib.<clinit>` in smali_classes4 — `System.loadLibrary("oneshot")` right after `blackrussia-client`, wrapped in try/catch UnsatisfiedLinkError so arm64 devices degrade gracefully.
- Kolson/Neizzir uses the same injection point (`KolsonLibrary.prepareGameStartup()` in JNILib clinit + JNIRenderer.renderFrame hooks).

### Emulator test (matreshka_root, arm64-only)
- APK installs and launches (pid alive, no FATAL/SIGSEGV).
- `libblackrussia-client.so`, `libsigner.so`, `libupdate-manager.so` loaded; game downloads ~3.3GB resources via updateManager.
- `liboneshot.so` NOT loaded — expected: AVD is arm64-only, our lib is v7a. try/catch prevented crash.
- CONCLUSION: packaging pipeline verified end-to-end. Cheat itself requires arm64 build for this emulator, or a 32-bit-capable device.

### arm64 offsets verified this session (same build as v7a!)
- g_NetGame / pool-slot global: `0x34e7b30` (ldr gives pool base; embedded CPlayerPool, slots at +8, 8-byte entries, max id 0x5df)
- `CPlayerPool::GetAt` arm64 = `0x15ee404` (id mask 0xffff, array[id] at pool+8+id*8)
- GetPedById / local-ped resolver = `0x15ee424` (remoteplayer+0x10 = ped; local via field0 + GetId check)
- get m_pPlayerPool / local slot accessor = `0x15ee3fc` (ldr x0,[x0]; ret)
- RequestSpawn arm64 = `0x15f13d8`; ScrSetPlayerPosFindZ arm64 = `0x15f13fc`
- arm64 globals cluster: g_RakClient `0x34e7b20`, g_NetGame `0x34e7b30` (parallel to v7a 0x21c5016/0x21c5030)
- Ped position fields arm64: expected +0x34/+0x38/+0x3C (vptr 8B shifts v7a +0x30 layout by 4) — NEEDS verification via SetPosition vcall
- Chat arm64: dispatcher 0x15d9944, SendChatCommand 0x15d9990 (RPC378), SendChatMessage 0x15d9a7c (RPC414)
- JNI arm64: step 0x1745098, multiTouchEvent 0x17455c8, sendChatMessage 0x1745f64

### TODO (blocking arm64 build)
- Build liboneshot arm64-v8a; source needs ABI-aware offset table (v7a +1 thumb vs arm64 no-thumb)
- arm64: FindGroundZ, AddClientMessage, Input_Process/mainloop hook, CVehiclePool, GetBonePosition, SetMoveAnim
- Position offsets arm64 verify (+0x34/38/3C hypothesis)
- RPC hook bodies: new client passes RPCParameters*, old samp signature mismatch — needs BitStream parse rewrite
- anticheat: libsigner.so / blackhub_event_reporter untested

## CPed arm64 field map (derived from client .h struct layouts, validated by gap markers gap2C4/gap70)

Object model: `CPlaceable` (non-polymorphic base) at +0x00, `_vptr$CEntity` at **+0x48**
(Itanium ABI: vptr follows non-polymorphic base), then CEntity/CPhysical/CPed fields.

- m_matrix @ +0x00 → pos.x/y/z = +0x30/+0x34/+0x38 (same as v7a)
- m_vecMoveSpeed (velocity) = +0x130/0x134/0x138   (v7a: +0x7C/0x80/0x84)
- m_vecTurnSpeed = +0x140
- m_fMass = +0x18C  (v7a "jumpMult" +0xC4; default ~70.f, reduce => higher jump)
- phys flags bitfield = +0x230 — bIsInWater = bit4 (mask 0x10)
- m_nPedState = +0x46C   (v7a 0x250)
- m_nLastPedState = +0x470
- m_nMoveState = +0x474
- m_fHealth = +0x488     (v7a 0x360)
- m_fHealthLimit = +0x48C
- m_fArmour = +0x490     (v7a 0x364)
- m_headingRate = +0x4C0 (v7a 0x38C)
- m_pMyVehicle = +0x510  (v7a 0x3B0)
- bInVehicle = +0x518    (v7a 0x3B8)
- m_invulnerabilityTimestamp = +0x6B8 — engine sets m_enableInvulnerability =
  (CTimer::m_snTimeInMilliseconds - timestamp < 0x7D1); pinning timestamp to
  0x7FFFFFFF keeps invulnerability true (godmode without damage-calc hook)
- m_enableInvulnerability = +0x6BC (bool, engine-maintained)
- m_jumpTimestamp = +0x764, m_bEnableJump = +0x762
- m_pPedIntelligence = +0x750
- CPlayerPed (local ped): m_fMoveSpeed +0x7D8, m_fCurrentStamina +0x7E0,
  m_fMaxStamina +0x7E4 (stamina ~150; keep current=max => infinite sprint)

Feature impl notes (arm64):
- Бессмертие: write ped+0x6B8=0x7FFFFFFF + ped+0x6BC=1 (per-frame); restore ts=0 on off
- Бесконечный бег: ped+0x7E0 = ped+0x7E4 (guard: max in 1..100000)
- Ходьба под водой: clear *(uint32*)(ped+0x230) bit 0x10
- Полёт: velocity +0x130/134/138 from m_fRotationCur +0x4B8 (camera global not yet mapped)
- Высокий прыжок: m_fMass +0x18C = 70/mult; restore 70 on off
- Длинный прыжок: v7a global 0x392FDC has no arm64 mapping — implemented as
  horizontal velocity boost while PEDSTATE_JUMP/FALL
- CPed ctor/setters use PED_OFF_* defines (ped.cpp); v7a path unchanged
