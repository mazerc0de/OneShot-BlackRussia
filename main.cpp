#include "includes.h"

uintptr_t libAddr = 0;
volatile int g_clockSet = -1;  // one-shot hour write for arm64 TimeChanger
static JavaVM *g_JavaVM = nullptr;
int glWidth, glHeight;
bool setup = false;

// client built-in debug menu: Java_..._JNILib_setDebugMenuVisible @ +0x1745e4C
void ToggleClientDebugMenu(bool visible)
{
    if(!g_JavaVM || !libAddr) return;
    JNIEnv *env = nullptr;
    if(g_JavaVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        if(g_JavaVM->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    }
    ((void (*)(JNIEnv *, jclass, jboolean))(libAddr + 0x1745E4C))(env, nullptr, visible ? JNI_TRUE : JNI_FALSE);
}

// ===== RakNet hooks (arm64): RakClient::Send @ 0x18D531C, ProcessNetworkPacket @ 0x18D6A1C =====
static bool rakHooksEnabled = true;
static int (*orig_RakClient_Send)(void *self, const char *data, int length, int priority, int reliability, char orderingChannel) = nullptr;
static int (*orig_RakClient_SendBS)(void *self, void *bitStream, int priority, int reliability, char orderingChannel) = nullptr;
static int (*orig_RakPeer_Send)(void *self, const char *data, int length, int priority, int reliability, char orderingChannel, void *playerID, void *broadcastList) = nullptr;
static int (*orig_RakPeer_SendBS)(void *self, void *bitStream, int priority, int reliability, char orderingChannel, void *playerID, void *broadcastList) = nullptr;
static void (*orig_RakClient_ProcessNetworkPacket)(void *self, unsigned int binaryAddress, unsigned short port, const char *data, int length, void *a5, void *a6, void *a7) = nullptr;
static unsigned rakSendCount[256] = {};
static unsigned rakRecvCount[256] = {};
static bool hospitalUnlock = false;
static int slapTargetId = -1;
static int slapTicks = 0;
static int slapPedOff = PLAYER_PED_OFF;
static float slapBaseZ = 0.f;
static void *g_rakPeer = nullptr;
static void *g_rakServerID = nullptr;
static unsigned char lastWpnUpd[16] = {};
static int lastWpnUpdLen = 0;
static uintptr_t aimbotVictimPed = 0;
uintptr_t g_camMatrixAddr = 0;  // arm64: camera RwMatrix found by data-segment scan
void FindCameraMatrix();
bool RangeResident(uintptr_t p, size_t n);
bool SafeRead(uintptr_t addr, void *dst, size_t n);

// send a raw outgoing packet via client RakClient::Send @0x18D531C —
// it resolves the server playerID itself through the vtable
static void SendRawPacket(const unsigned char *pkt, int len, int pri = 1, int rel = 1)
{
    static void (*RakClientSend)(void *, const char *, int, int, int, char) = nullptr;
    if(!RakClientSend) RakClientSend = (void (*)(void *, const char *, int, int, int, char))(libAddr + 0x18D531C);
    if(g_rakPeer)
    {
        RakClientSend(g_rakPeer, (const char *)pkt, len, pri, rel, 0);
        __android_log_print(ANDROID_LOG_INFO, "TRACE_LAST_FUNC", "BULLET sent len=%d peer=%p", len, g_rakPeer);
    }
    else
        __android_log_print(ANDROID_LOG_INFO, "TRACE_LAST_FUNC", "BULLET no peer!");
}

// forge ID_BULLET_SYNC (0x1D) hitting `targetId` — server relays it to the
// victim whose own client applies damage + knockback (real, visible to all)
static void SendBulletHit(int targetId, uintptr_t victimPed)
{
    uintptr_t myped = cheatClient->FindLocalPlayer();
    if(!myped || !victimPed) return;
    unsigned char b[41] = {};
    b[0] = 0x1D;                 // ID_BULLET_SYNC
    b[1] = 1;                    // BULLET_HIT_TYPE_PLAYER
    *(uint16_t *)(b + 2) = (uint16_t)targetId;
    float *org = (float *)(b + 4);   // shot origin = our pos
    org[0] = *(float *)(myped + 0x38); org[1] = *(float *)(myped + 0x3C); org[2] = *(float *)(myped + 0x40);
    float *tgt = (float *)(b + 16);  // hit pos = victim pos (+1m)
    tgt[0] = *(float *)(victimPed + 0x38); tgt[1] = *(float *)(victimPed + 0x3C); tgt[2] = *(float *)(victimPed + 0x40) + 1.f;
    float *off = (float *)(b + 28);  // spread offsets (real client sends small rand)
    off[0] = ((rand() % 200) - 100) / 2000.f;
    off[1] = ((rand() % 200) - 100) / 2000.f;
    off[2] = ((rand() % 200) - 100) / 2000.f;
    b[40] = lastWpnUpdLen >= 8 ? lastWpnUpd[6] : 0x18; // weapon id seen in WEAPONS_UPDATE (must match held weapon)
    SendRawPacket(b, sizeof(b));
    // real shots are followed by WEAPONS_UPDATE (id=35) with ammo-1 — replay it
    if(lastWpnUpdLen >= 8)
    {
        unsigned char w[16];
        memcpy(w, lastWpnUpd, lastWpnUpdLen);
        *(uint16_t *)(w + 7) -= 1;     // ammo decrement like a real shot
        SendRawPacket(w, lastWpnUpdLen, 1, 0);
    }
}

// arm64: client Chat::AddClientMessage dispatcher @0xBE4D40 has signature
// (int color, const char* msg) — NOT (char*). Calling it with one arg
// puts the string into w0 and reads garbage x1 -> crash. Use this wrapper.
static void ChatMsg(const char *s)
{
#ifdef __aarch64__
    ((void (*)(int, const char *))(libAddr + 0xBE4D40))(0xFFFFFFFF, s);
#else
    AddChatMessage((char *)s);
#endif
}

static int RakClient_Send_hook(void *self, const char *data, int length, int priority, int reliability, char orderingChannel)
{
    if (data && length > 0) {
        unsigned id = (unsigned char)data[0];
        rakSendCount[id]++;
        if (rakSendCount[id] <= 20 || (rakSendCount[id] % 300) == 0)
            LOGI("RAKSEND id=%u len=%d pri=%d rel=%d", id, length, priority, reliability);
    }
    return orig_RakClient_Send ? orig_RakClient_Send(self, data, length, priority, reliability, orderingChannel) : 0;
}

static int RakClient_SendBS_hook(void *self, void *bitStream, int priority, int reliability, char orderingChannel)
{
    // BitStream: +0x0 bitsUsed(u32), +0x10 data ptr
    unsigned bitsUsed = *(unsigned *)bitStream;
    const char *data = *(const char **)((uintptr_t)bitStream + 0x10);
    unsigned len = bitsUsed / 8;
    if (data && len > 0) {
        unsigned id = (unsigned char)data[0];
        rakSendCount[id]++;
        if (rakSendCount[id] <= 20 || (rakSendCount[id] % 300) == 0) {
            char hex[3*24+1] = {};
            unsigned hd = len < 24 ? len : 24;
            for (unsigned i = 0; i < hd; i++) snprintf(hex + i*3, 4, "%02x ", (unsigned char)data[i]);
            LOGI("RAKSEND id=%u len=%u pri=%d rel=%d | %s", id, len, priority, reliability, hex);
        }
    }
    return orig_RakClient_SendBS ? orig_RakClient_SendBS(self, bitStream, priority, reliability, orderingChannel) : 0;
}

// CRemotePlayer layout (arm64, verified via pool dumps):
// +0x10 ped ptr, +0x18 name[49], +0x4A m_playerId u16 (before OnFootSyncData@+0x50)
static int RemotePlayerId(uintptr_t p)
{
    int id = *(uint16_t *)(p + 0x4A);
    if(id <= 0 || id >= 2000) id = *(int *)(p + 0x4C); // fallback u32
    return (id > 0 && id < 2000) ? id : -1;
}

static inline bool HeapPtr(uintptr_t p)
{
    // strip MTE tag byte, then accept plausible userland heap VAs (this device
    // allocates at ~0x78_xxxx_xxxx — the old 0x70..0xc0..e11 window missed it)
    p &= 0x00FFFFFFFFFFFFFFULL;
    return p >= 0x0000000100000000ULL && p < 0x0000FF0000000000ULL;
}

static uintptr_t RemotePedFromRp(uintptr_t p)
{
    uintptr_t q = *(uintptr_t *)(p + 0x10);       // verified ped offset
    if(HeapPtr(q)) return q;
    for(int off = 0x8; off < 0x200; off += 8)      // fallback: first heap qword
    {
        q = *(uintptr_t *)(p + off);
        if(HeapPtr(q)) return q;
    }
    return 0;
}

static uintptr_t FindNearestRemotePed(int *slotOut, float *distOut = nullptr)
{
    uintptr_t myped = cheatClient->FindLocalPlayer();
    if(!myped) return 0;
    Vec3 mp = cheatClient->GetPos(myped);
    float mx = mp.x, my = mp.y, mz = mp.z;
    int bs = -1; uintptr_t bped = 0; float bd = 1e9f;
    for(int i = 0; i < 600; i++)
    {
        uintptr_t p = cheatClient->GetAtPlayerPool(i);
        if(!HeapPtr(p)) continue;
        uintptr_t ped = RemotePedFromRp(p);
        if(!ped) continue;
        Vec3 vp = cheatClient->GetPos(ped);
        float dx = vp.x - mx, dy = vp.y - my, dz = vp.z - mz;
        float d = dx*dx + dy*dy + dz*dz;
        if(d > 0.01f && d < bd) { bd = d; bs = i; bped = ped; }
    }
    if(slotOut) *slotOut = bs;
    if(distOut) *distOut = sqrtf(bd);
    return bped;
}

static void RakLogSend(const char *data, unsigned len, int priority, int reliability, const char *tag)
{
    if (!data || len == 0) return;
    unsigned id = (unsigned char)data[0];
    if (id == 35 && len <= sizeof(lastWpnUpd)) { memcpy(lastWpnUpd, data, len); lastWpnUpdLen = len; }
    // silent aim: a real outgoing bullet (id=29) carries genuine firing
    // context, so retargeting its hit fields to the nearest streamed player
    // makes the server register a real hit — damage + knockback, no aiming
    if (id == 29 && len >= 41 && cheatClient &&
        (cheatClient->Aimbot.state || cheatClient->PlayerCheat.Rage.AimBot.state))
    {
        int slot = -1; float dist = 0.f;
        uintptr_t vped = FindNearestRemotePed(&slot, &dist);
        uintptr_t vrp = vped ? cheatClient->GetAtPlayerPool(slot) : 0;
        int hitId = vrp ? RemotePlayerId(vrp) : -1;
        if(vped && hitId > 0) aimbotVictimPed = vped;
        {
            unsigned char *b = (unsigned char *)data;
            b[1] = 1;                                  // BULLET_HIT_TYPE_PLAYER
            *(uint16_t *)(b + 2) = (uint16_t)hitId;    // hit player id (not pool slot!)
            float *t = (float *)(b + 16);              // target pos = victim chest
            Vec3 vp = cheatClient->GetPos(vped);
            t[0] = vp.x; t[1] = vp.y; t[2] = vp.z + 0.5f;
            __android_log_print(ANDROID_LOG_INFO, "PEDDBG", "SILENTAIM slot=%d pid=%d dist=%.1f",
                slot, hitId, dist);
        }
    }
    // capture camera state from every aim-sync for the WorldToScreen matrix scan
    if (id == 34 && len >= 37)
    {
        const float *fr = (const float *)(data + 9);
        g_camFront[0] = fr[0]; g_camFront[1] = fr[1]; g_camFront[2] = fr[2];
        const float *cp = (const float *)(data + 25);
        g_camPos[0] = cp[0]; g_camPos[1] = cp[1]; g_camPos[2] = cp[2];
        g_camValid = 1;
    }
    // keep the aim state consistent with retargeted bullets: camFront @ +9
    // (3 floats) + aimZ @ +21 point where the server thinks we aim
    if (id == 34 && len >= 37 && HeapPtr(aimbotVictimPed) && cheatClient &&
        (cheatClient->Aimbot.state || cheatClient->PlayerCheat.Rage.AimBot.state))
    {
        uintptr_t myped = cheatClient->FindLocalPlayer();
        if(myped)
        {
            float *fr = (float *)((unsigned char *)data + 9);
            Vec3 vp = cheatClient->GetPos(aimbotVictimPed);
            Vec3 mp = cheatClient->GetPos(myped);
            float dx = vp.x - mp.x;
            float dy = vp.y - mp.y;
            float dz = vp.z + 0.5f - mp.z;
            float l = sqrtf(dx*dx + dy*dy + dz*dz);
            if(l > 0.001f) { fr[0] = dx/l; fr[1] = dy/l; fr[2] = dz/l; g_camFront[0]=fr[0]; g_camFront[1]=fr[1]; g_camFront[2]=fr[2]; }
            *(float *)((unsigned char *)data + 21) = fr[2];  // aimZ
        }
    }
    // while a slap is active, mark the outgoing onfoot sync as "firing"
    // (keys u16 @ +3/+4 = 0x80 0xff seen in real shots) so the server
    // accepts our forged bullet hit as part of a real shot
    if (id == 30 && len >= 7 && slapTicks > 0) { ((unsigned char *)data)[3] = 0x80; ((unsigned char *)data)[4] = 0xff; }
    rakSendCount[id]++;
    unsigned logEvery = (id == 30) ? 30 : 300;
    if (rakSendCount[id] <= 20 || id == 27 || (rakSendCount[id] % logEvery) == 0) {
        unsigned hd = len < 24 ? len : 24;
        if (id == 30) hd = len < 64 ? len : 64;
        if (id == 27) hd = len < 200 ? len : 200;
        if (id == 29 || id == 35) hd = len < 66 ? len : 66;
        if (id == 34) hd = len < 66 ? len : 66;
        char hex[3*200+1] = {};
        for (unsigned i = 0; i < hd; i++) snprintf(hex + i*3, 4, "%02x ", (unsigned char)data[i]);
        LOGI("%s id=%u len=%u pri=%d rel=%d | %s", tag, id, len, priority, reliability, hex);
    }
}

static int RakPeer_Send_hook(void *self, const char *data, int length, int priority, int reliability, char orderingChannel, void *playerID, void *broadcastList)
{
    RakLogSend(data, (unsigned)length, priority, reliability, "RAKSEND");
    return orig_RakPeer_Send ? orig_RakPeer_Send(self, data, length, priority, reliability, orderingChannel, playerID, broadcastList) : 0;
}

static int RakPeer_SendBS_hook(void *self, void *bitStream, int priority, int reliability, char orderingChannel, void *playerID, void *broadcastList)
{
    if(!g_rakPeer) g_rakPeer = self;
    if(!g_rakServerID && playerID) g_rakServerID = playerID;
    RakLogSend(*(const char **)((uintptr_t)bitStream + 0x10), *(unsigned *)bitStream / 8, priority, reliability, "RAKSENDBS");
    return orig_RakPeer_SendBS ? orig_RakPeer_SendBS(self, bitStream, priority, reliability, orderingChannel, playerID, broadcastList) : 0;
}

static void RakClient_ProcessNetworkPacket_hook(void *self, unsigned int binaryAddress, unsigned short port, const char *data, int length, void *a5, void *a6, void *a7)
{
    if (data && length > 0) {
        unsigned id = (unsigned char)data[0];
        rakRecvCount[id]++;
        if (rakRecvCount[id] <= 20 || (rakRecvCount[id] % 300) == 0)
            LOGI("RAKRECV id=%u len=%d", id, length);
    }
    if (orig_RakClient_ProcessNetworkPacket)
        orig_RakClient_ProcessNetworkPacket(self, binaryAddress, port, data, length, a5, a6, a7);
}

int gpeer = 0, ga7 = 0, ga8 = 0;
Vec3 start_shot = {0,0,0}, end_shot = {0,0,0};
Vec3 screen_start = Vec3(0,0,0), screen_end = Vec3(0,0,0);
int last_ms = 0;
bool bot_miner = false;
float crosshair_offset_x = 0.53f, crosshair_offset_y = 0.43f;
bool logga = false;
bool trianglesvh = false;

// Menu
int alphaWindowBg = 255;
int alphaChildBg = 255;
int icon_size = 130;
int start_touch_x = 0, start_touch_y = 0;
int current_menu_category = 0;
int current_player_category = 0;
int current_other_category = 0;
int damager_settings_active_tab = 0;

bool m_bNeedClearMousePos = false;
bool menuOpen = true;
bool hideIcon = false;
bool textureInited = false;
bool needShowText = false;
bool icon_is_hovered = false;
bool menu_is_hovered = false;
bool wallhack_menu_is_hovered = false;
bool game_speed_menu_is_hovered = false;
bool change_car_model_menu_is_hovered = false;
bool tp_toPeds_menu_is_hovered = false;

bool wh_window_not_inited = true;
bool tp_toPeds_inited = false;
bool game_speed_inited = false;

// Trash
void log1(int,int,int,int, float);

GLuint LoadTexture(const char* path);
GLuint icon_texture = 0;
GLuint icon_alpha_texture = 0;

ImVec4 rgba(int r, int g, int b, int a = 255)
{
	float retR = (float)r / 255.f;
	float retG = (float)g / 255.f;
	float retB = (float)b / 255.f;
	float retA = (float)a / 255.f;
	
	return ImVec4(retR, retG, retB, retA);
}

void startService(JNIEnv *env, jobject ctx){
    jclass native_context = env->GetObjectClass(ctx);
    jclass intentClass = env->FindClass(OBFUSCATE("android/content/Intent"));
    jclass actionString = env->FindClass(OBFUSCATE("wrapper/MyService"));
    jmethodID newIntent = env->GetMethodID(intentClass, OBFUSCATE("<init>"), OBFUSCATE("(Landroid/content/Context;Ljava/lang/Class;)V"));
    jobject intent = env->NewObject(intentClass,newIntent,ctx,actionString);
    jmethodID startActivityMethodId = env->GetMethodID(native_context, OBFUSCATE("startService"), OBFUSCATE("(Landroid/content/Intent;)Landroid/content/ComponentName;"));
    env->CallObjectMethod(ctx, startActivityMethodId, intent);
}

EGLBoolean (*old_eglSwapBuffers)(EGLDisplay dpy, EGLSurface surface);
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
   eglQuerySurface(dpy, surface, EGL_WIDTH, &glWidth);
   eglQuerySurface(dpy, surface, EGL_HEIGHT, &glHeight);

	if(cheatClient) {
		cheatClient->screen_width = glWidth;
		cheatClient->screen_height = glHeight;
	}
   
	if (!setup) {
        IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
		ImGui::StyleColorsDark();
		ImGui_ImplOpenGL3_Init(OBFUSCATE("#version 100"));
		io.Fonts->AddFontFromMemoryTTF(custom_font, sizeof(custom_font), 30.f, NULL, io.Fonts->GetGlyphRangesCyrillic());
		
		ImFontConfig config;
		config.MergeMode = true;
		config.GlyphMinAdvanceX = 0.f;
		config.GlyphOffset = ImVec2(0,5.f);
		static const ImWchar icon_ranges[] = { ICON_MIN, ICON_MAX, 0 };
		io.Fonts->AddFontFromMemoryTTF(boxicons_ttf, sizeof(boxicons_ttf), 32.f, &config, icon_ranges);
		
		ImGui::GetStyle().ScaleAllSizes(3.0f);
		
        setup = true;
    }
	
	if(!textureInited)
	{
		const char* path_to_icon = OBFUSCATE("/storage/emulated/0/Android/data/com.br.top/files/icon.png");
		const char* path_to_alpha_icon = OBFUSCATE("/storage/emulated/0/Android/data/com.br.top/files/alpha_icon.png");
		std::ofstream outFile1(path_to_icon, std::ios::binary);
		
		if(outFile1.is_open())
		{
			outFile1.write(reinterpret_cast<char*>(icon_image.data()), icon_image.size());
			outFile1.close();
			icon_texture = LoadTexture(OBFUSCATE("/storage/emulated/0/Android/data/com.br.top/files/icon.png"));
		}
		
		std::ofstream outFile2(path_to_alpha_icon, std::ios::binary);
		
		if(outFile2.is_open())
		{
			outFile2.write(reinterpret_cast<char*>(alpha_icon_image.data()), alpha_icon_image.size());
			outFile2.close();
			icon_alpha_texture = LoadTexture(OBFUSCATE("/storage/emulated/0/Android/data/com.br.top/files/alpha_icon.png"));
		}
		textureInited = true;
	}

    ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
	io.FontGlobalScale = (float)glHeight / 1080.0f;
	
    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
	
	if(!hideIcon)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
	
		ImGui::Begin(OBFUSCATE("mod-icon"), NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize);
		{
			if(textureInited)
			{
				ImGui::SetWindowSize(ImVec2(icon_size + 1, icon_size + 1));
				if(!icon_is_hovered) {
					ImGui::Image((void*)(intptr_t)icon_texture, ImVec2(icon_size, icon_size));
				} else {
					ImGui::Image((void*)(intptr_t)icon_alpha_texture, ImVec2(icon_size, icon_size));
				}
			}
		}
		ImGui::End();
		
		ImGui::PopStyleVar();
	}
	
	if(menuOpen)
	{
		// Build menu
    	//ImGui::ShowDemoWindow();
	
		ImGuiStyle& style = ImGui::GetStyle();
		style.Colors[ImGuiCol_Text] = rgba(255,255,255);
		style.Colors[ImGuiCol_WindowBg] = rgba(17,17,17,alphaWindowBg);
		style.Colors[ImGuiCol_ChildBg] = rgba(31,31,31,alphaChildBg);
		style.Colors[ImGuiCol_FrameBg] = rgba(51,51,51);
		style.Colors[ImGuiCol_PopupBg] = rgba(17,17,17);
		style.Colors[ImGuiCol_Text] = rgba(255,255,255);
		style.Colors[ImGuiCol_Button] = rgba(51,51,51);
		style.Colors[ImGuiCol_ButtonHovered] = rgba(255,70,70);
		style.Colors[ImGuiCol_ButtonActive] = rgba(255,55,55);
		style.Colors[ImGuiCol_SliderGrab] = rgba(255,55,55);
		style.Colors[ImGuiCol_SliderGrabActive] = rgba(255,55,55,200);
		style.Colors[ImGuiCol_CheckMark] = rgba(255,55,55);
		style.Colors[ImGuiCol_FrameBgHovered] = rgba(255,55,55,100);
		style.Colors[ImGuiCol_FrameBgActive] = rgba(255,55,55,100);
		style.Colors[ImGuiCol_Header] = rgba(80,80,80);
		style.Colors[ImGuiCol_HeaderHovered] = rgba(255,55,55);
		style.Colors[ImGuiCol_HeaderActive] = rgba(255,55,55);
		style.Colors[ImGuiCol_Tab] = rgba(51,51,51);
		style.Colors[ImGuiCol_TabHovered] = rgba(255,70,70);
		style.Colors[ImGuiCol_TabActive] = rgba(255,55,55);
		style.Colors[ImGuiCol_TitleBg] = rgba(17,17,17);
		style.Colors[ImGuiCol_TitleBgActive] = rgba(255,70,70);
		
		ImGui::GetStyle().WindowRounding = 15.f;
		ImGui::GetStyle().ChildRounding = 15.f;
		ImGui::GetStyle().FrameRounding = 15.f;
		ImGui::GetStyle().GrabRounding = 10.f;
		
	
		ImGui::SetNextWindowSize(ImVec2(ImGui::CalcTextSize(OBFUSCATE("ХХХХХХХХХХ")).x * 2.f + 800.f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 24.f), ImGuiCond_FirstUseEver);
		ImGui::Begin(OBFUSCATE("OneShot"), NULL, ImGuiWindowFlags_NoTitleBar);
		{
			float categoryButtonWidth = ImGui::CalcTextSize(OBFUSCATE("ХХХХХХХХХХ")).x * 2.f;
			float categoryButtonHeight = ImGui::CalcTextSize(OBFUSCATE("X")).y * 2.f;
		
			ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(17,17,17,0));
			ImGui::BeginChild(OBFUSCATE("#left-tab"), ImVec2(categoryButtonWidth, ImGui::GetContentRegionAvail().y), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			{
				ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(31,31,31));
				ImGui::BeginChild(OBFUSCATE("#expire-info"), ImVec2(categoryButtonWidth, ImGui::CalcTextSize(OBFUSCATE("X")).y * 5.f));
				{
					ImGui::Spacing();
					ImGui::Spacing();
					
					ImGui::SetCursorPosX((categoryButtonWidth - (ImGui::CalcTextSize(OBFUSCATE("ONESHOT 3.0.0")).x)) / 2);
					ImGui::Text(OBFUSCATE("ONESHOT 3.0.0"));
					
					ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(17,17,17));
					ImGui::SetCursorPosX((categoryButtonWidth - (categoryButtonWidth * 0.9f)) / 2);
					ImGui::BeginChild(OBFUSCATE("##settabs"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight * 1.2f));
					{
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2));
						ImGui::SetCursorPosY((categoryButtonHeight * 1.2f - categoryButtonHeight) / 2);
						if(current_menu_category == -1)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							ImGui::Button(ICON_INFO_CIRCLE, ImVec2(((categoryButtonWidth * 0.9f) / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2), categoryButtonHeight));
							ImGui::PopStyleColor();
						}
						else
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(17,17,17));
							if(ImGui::Button(ICON_INFO_CIRCLE, ImVec2(((categoryButtonWidth * 0.9f) / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2), categoryButtonHeight)))
							{
								current_menu_category = -1;
							}
							ImGui::PopStyleColor();
						}
						
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2) + (((categoryButtonWidth * 0.9f) / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2)));
						ImGui::SetCursorPosY((categoryButtonHeight * 1.2f - categoryButtonHeight) / 2);
						if(current_menu_category == -2)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							ImGui::Button(ICON_BXS_COG, ImVec2(((categoryButtonWidth * 0.9f) / 2) - ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2), categoryButtonHeight));
							ImGui::PopStyleColor();
						}
						else
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(17,17,17));
							if(ImGui::Button(ICON_BXS_COG, ImVec2(((categoryButtonWidth * 0.9f) / 2) - ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2), categoryButtonHeight)))
							{
								current_menu_category = -2;
							}
							ImGui::PopStyleColor();
						}
					}
					ImGui::EndChild();
					ImGui::PopStyleColor();
				}
				ImGui::EndChild();
				
				ImGui::Spacing();
				
				ImGui::BeginChild(OBFUSCATE("#category"), ImVec2(categoryButtonWidth, ImGui::CalcTextSize(OBFUSCATE("X")).y * 16.f));
				{
					ImGui::Spacing();
					ImGui::Spacing();
					
					ImGui::SetCursorPosX((categoryButtonWidth - (categoryButtonWidth * 0.9f)) / 2);
					if(current_menu_category == 0) {
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255, 55, 55));
						ImGui::Button(OBFUSCATE(ICON_USER u8" Персонаж"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight));
						ImGui::PopStyleColor();
					} else {
						if(ImGui::Button(OBFUSCATE(ICON_USER u8" Персонаж"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight)))
						{
							current_menu_category = 0;
						}
					}
				
					ImGui::Spacing();
					
					ImGui::SetCursorPosX((categoryButtonWidth - (categoryButtonWidth * 0.9f)) / 2);
					if(current_menu_category == 1) {
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255, 55, 55));
						ImGui::Button(OBFUSCATE(ICON_TRUCK u8" Транспорт"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight));
						ImGui::PopStyleColor();
					} else {
						if(ImGui::Button(OBFUSCATE(ICON_TRUCK u8" Транспорт"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight)))
						{
							current_menu_category = 1;
						}
					}
					
					ImGui::Spacing();
					
					ImGui::SetCursorPosX((categoryButtonWidth - (categoryButtonWidth * 0.9f)) / 2);
					if(current_menu_category == 2) {
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255, 55, 55));
						ImGui::Button(OBFUSCATE(ICON_LOW_VISION u8" Визуалы"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight));
						ImGui::PopStyleColor();
					} else {
						if(ImGui::Button(OBFUSCATE(ICON_LOW_VISION u8" Визуалы"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight)))
						{
							current_menu_category = 2;
						}
					}
				
					ImGui::Spacing();
					
					ImGui::SetCursorPosX((categoryButtonWidth - (categoryButtonWidth * 0.9f)) / 2);
					if(current_menu_category == 3) {
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255, 55, 55));
						ImGui::Button(OBFUSCATE(ICON_MENU u8" Другое"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight));
						ImGui::PopStyleColor();
					} else {
						if(ImGui::Button(OBFUSCATE(ICON_MENU u8" Другое"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight)))
						{
							current_menu_category = 3;
						}
					}
				
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::Spacing();
					
					ImGui::SetCursorPosX((categoryButtonWidth - (categoryButtonWidth * 0.9f)) / 2);
					if(ImGui::Button(OBFUSCATE(ICON_X u8" Закрыть"), ImVec2(categoryButtonWidth * 0.9f, categoryButtonHeight)))
					{
						menuOpen = false;
					}
				}
				ImGui::EndChild();
				ImGui::PopStyleColor();
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
			
			ImGui::SameLine();
			
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 10.f));
			ImGui::BeginChild(OBFUSCATE("#main-tab"), ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y));
			{
				if(current_menu_category == -1)
				{
					ImGui::BeginChild(OBFUSCATE("##info-menu"), ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y));
					{
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::Spacing();
						
						ImGui::SameLine();
						if (ImGui::CollapsingHeader(OBFUSCATE("Damager ( Дамагер )"), ImGuiTreeNodeFlags_None))
        				{
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Дамагер – наносит урон всем на ближнем расстоянии"));
						}
						
						ImGui::Spacing();
						ImGui::SameLine();
						
						ImGui::Text(cheatClient->response.c_str());
						
						/*if (ImGui::CollapsingHeader(OBFUSCATE("Debug Info"), ImGuiTreeNodeFlags_None))
        				{
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text("Кол-во игроков на сервере: %d", cheatClient->ServerPlayersCount);
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text("Размер кастом пула игроков: %d", sizeof(cheatClient->ServerPlayers));
							ImGui::Spacing();
							ImGui::SameLine();
							
							for(int i = 0; i < sizeof(cheatClient->ServerPlayers); i++)
							{
								ImGui::Text("%s [%d]", cheatClient->ServerPlayers[i].nickname, cheatClient->ServerPlayers[i].id);
								
							}
						}*/
					}
					ImGui::EndChild();
				}
				
				if(current_menu_category == -2)
				{
					ImGui::BeginChild(OBFUSCATE("##settings-menu"), ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y));
					{
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::Spacing();
						
						ImGui::SameLine();
						if (ImGui::CollapsingHeader(OBFUSCATE("Настройки меню"), ImGuiTreeNodeFlags_None))
        				{
							ImGui::Spacing();
							
							ImGui::SameLine();
							
							if(ImGui::Checkbox(OBFUSCATE("Скрыть иконку"), &hideIcon))
							{
								cheatClient->SaveSettings();
							}
							
							ImGui::Spacing();
							ImGui::Spacing();
							
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Размер иконки"));
						
							ImGui::Spacing();
						
							ImGui::SameLine();
							if(ImGui::SliderInt("", &icon_size, 50, 200))
							{
								cheatClient->SaveSettings();
							}
						
							ImGui::Spacing();
							ImGui::Spacing();
						
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Прозрачность меню"));
							ImGui::Spacing();
							ImGui::SameLine();
							if(ImGui::SliderInt(OBFUSCATE("##menubgalpha"), &alphaWindowBg, 0, 255))
							{
								alphaChildBg = alphaWindowBg;
								cheatClient->SaveSettings();
							}
							
							ImGui::Spacing();
							ImGui::Spacing();
						
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Кастомный прицел"));
							ImGui::Spacing();
							ImGui::SameLine();
							/*if(ImGui::SliderFloat("##customsightwidth", &cross_offset, 0.0f, 0.5f))
							{
								
							}
							
							ImGui::Spacing();
							ImGui::SameLine();
							if(ImGui::SliderFloat("##customsightheight", &crosshair_offset_y, 0.350f, 0.7f))
							{
								
							}*/
						}
						
						ImGui::Spacing();
					
						ImGui::SameLine();
						if (ImGui::CollapsingHeader(OBFUSCATE("Настройка подкидывания"), ImGuiTreeNodeFlags_None))
        				{
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Подкидывание вверх"));
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::SliderFloat(OBFUSCATE("##slapUp"), &cheatClient->PlayerCheat.Base.Slap.up, 1.f, 10.f);
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Подкидывание вниз"));
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::SliderFloat(OBFUSCATE("##slapDown"), &cheatClient->PlayerCheat.Base.Slap.down, 1.f, 10.f);
							
						}
					
						ImGui::Spacing();
					
						ImGui::SameLine();
						if (ImGui::CollapsingHeader(OBFUSCATE("Настройки персонажа"), ImGuiTreeNodeFlags_None))
        				{
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Скорость персонажа"));
							ImGui::Spacing();
							ImGui::SameLine();
							// ImGui::SliderFloat("##speedhackped", &speed_hack_ped, 1.f, 5.f);
							ImGui::Spacing();
							ImGui::Separator();
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Длинный прыжок"));
							ImGui::Spacing();
							ImGui::SameLine();
							if(ImGui::SliderFloat(OBFUSCATE("##longjumpped"), &cheatClient->PlayerCheat.Base.Jumping.long__, 1.f, 3.f))
							{
								if(cheatClient->PlayerCheat.Base.Jumping.long_) {
#ifndef __aarch64__
									WriteMemoryFloat(libAddr + 0x392FDC, 0.17f * cheatClient->PlayerCheat.Base.Jumping.long__);
#endif
								}
							}
							ImGui::Spacing();
							ImGui::Separator();
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Высокий прыжок"));
							ImGui::Spacing();
							ImGui::SameLine();
							if(ImGui::SliderFloat(OBFUSCATE("##highjumpped"), &cheatClient->PlayerCheat.Base.Jumping.high__, 1.f, 3.f))
							{
								if(cheatClient->PlayerCheat.Base.Jumping.high) {
									uintptr_t localplayer = cheatClient->FindLocalPlayer();
									if(localplayer)
									{
										CPed localped = CPed(localplayer);
										localped.SetHighJumpMult(70.f / cheatClient->PlayerCheat.Base.Jumping.high__);
									}									
								}
							}
							ImGui::Spacing();
							ImGui::Separator();
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("Скорость поворота"));
							ImGui::Spacing();
							ImGui::SameLine();
							if(ImGui::SliderFloat(OBFUSCATE("##headingrateped"), &cheatClient->PlayerCheat.Base.Control.rate, 1.f, 4.f))
							{
								if(cheatClient->PlayerCheat.Base.Control.fastRotate) {
									uintptr_t localplayer = cheatClient->FindLocalPlayer();
									if(localplayer)
									{
										CPed localped = CPed(localplayer);
										localped.SetHeadingRate(7.5f * cheatClient->PlayerCheat.Base.Control.rate);
									}									
								}
							}
						}
					
						ImGui::Spacing();
					
						ImGui::SameLine();
						if (ImGui::CollapsingHeader(OBFUSCATE("Настройки транспорта"), ImGuiTreeNodeFlags_None))
        				{
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text(OBFUSCATE("CarShot ( ускорение )"));
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::SliderFloat(OBFUSCATE("##carshotAccelerate"), &cheatClient->VehicleCheat.CarShot.accelerate, 1.f, 10.f);
							
						}
						
						ImGui::Spacing();
					
						ImGui::SameLine();
						
					}
					ImGui::EndChild();
				}
				
				if(current_menu_category == 0)
				{
					ImGui::Spacing();
					ImGui::Spacing();
					
					ImVec2 currentCursorPos = ImGui::GetCursorPos();
					
					ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(17,17,17));
					ImGui::SameLine();
					ImGui::BeginChild(OBFUSCATE("##player-category"), ImVec2(ImGui::GetContentRegionAvail().x, categoryButtonHeight * 1.2f));
					{
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2));
						ImGui::SetCursorPosY((categoryButtonHeight * 1.2f - categoryButtonHeight) / 2);
						if(current_player_category == 0)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							ImGui::Button(OBFUSCATE(u8"Основное"), ImVec2((ImGui::GetWindowWidth() / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2), categoryButtonHeight));
							ImGui::PopStyleColor();
						}
						else
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(17,17,17));
							if(ImGui::Button(OBFUSCATE(u8"Основное"), ImVec2((ImGui::GetWindowWidth() / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2), categoryButtonHeight)))
							{
								current_player_category = 0;
							}
							ImGui::PopStyleColor();
						}
						
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2) + ((ImGui::GetWindowWidth() / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2)));
						ImGui::SetCursorPosY((categoryButtonHeight * 1.2f - categoryButtonHeight) / 2);
						if(current_player_category == 1)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							ImGui::Button(OBFUSCATE(u8"Боевой режим"), ImVec2((ImGui::GetWindowWidth() / 2) - ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2), categoryButtonHeight));
							ImGui::PopStyleColor();
						}
						else
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(17,17,17));
							if(ImGui::Button(OBFUSCATE(u8"Боевой режим"), ImVec2((ImGui::GetWindowWidth() / 2) - ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2), categoryButtonHeight)))
							{
								current_player_category = 1;
							}
							ImGui::PopStyleColor();
						}
					}
					ImGui::EndChild();
					ImGui::PopStyleColor();
					
					ImGui::Spacing();
					
					if(current_player_category == 0)
					{
						ImGui::SameLine();
						ImGui::SameLine();
						if(cheatClient->PlayerCheat.Base.Invulnerability.health)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Бессмертие"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Invulnerability.health =! cheatClient->PlayerCheat.Base.Invulnerability.health;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Бессмертие"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Invulnerability.health =! cheatClient->PlayerCheat.Base.Invulnerability.health;
							}
						}
						/*if(ImGui::Button("Бессмертие", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
						{
							cheatClient->gmped =! cheatClient->gmped;
							for(int i = 0; i < 1500; i++)
							{
								int playerpool = cheatClient->GetPlayerPool();
								if(!playerpool) { continue; }
								uintptr_t player = CPlayerPool_GetAt(playerpool, i);
								if(player)
								{
									player = *(uintptr_t *)(player + PLAYER_PED_OFF);
									if(player > 0x100000)
									{
										int number = *(int *)(player + 80);
										number &= ~0x100;
										*(int *)(player + 80) = number;
									}
								}
							}
							
							BrNotification(TYPE_TEXT_GREEN, "Бессмертие работает только от физики игры!", 5);
						}*/
						ImGui::SameLine();
						if(cheatClient->PlayerCheat.Base.Invulnerability.endurance)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Бесконечный Бег"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Invulnerability.endurance =! cheatClient->PlayerCheat.Base.Invulnerability.endurance;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Бесконечный Бег"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Invulnerability.endurance =! cheatClient->PlayerCheat.Base.Invulnerability.endurance;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(cheatClient->PlayerCheat.Base.FlyPed.state)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Полет"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.FlyPed.state =! cheatClient->PlayerCheat.Base.FlyPed.state;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Полет"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.FlyPed.state =! cheatClient->PlayerCheat.Base.FlyPed.state;
							}
						}
						ImGui::SameLine();
						if(cheatClient->PlayerCheat.Base.Control.fastRotate)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Быстрый поворот"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Control.fastRotate =! cheatClient->PlayerCheat.Base.Control.fastRotate;
								uintptr_t localplayer = cheatClient->FindLocalPlayer();
								if(localplayer)
								{
									CPed localped = CPed(localplayer);
									localped.SetHeadingRate(7.5f);
								}
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Быстрый поворот"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Control.fastRotate =! cheatClient->PlayerCheat.Base.Control.fastRotate;
								uintptr_t localplayer = cheatClient->FindLocalPlayer();
								if(localplayer)
								{
									CPed localped = CPed(localplayer);
									localped.SetHeadingRate(7.5f * cheatClient->PlayerCheat.Base.Control.rate);
								}
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(cheatClient->PlayerCheat.Base.Jumping.long_)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Длинный прыжок"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Jumping.long_ =! cheatClient->PlayerCheat.Base.Jumping.long_;
#ifndef __aarch64__
								WriteMemoryFloat(libAddr + 0x392FDC, 0.17f);
#endif
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Длинный прыжок"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Jumping.long_ =! cheatClient->PlayerCheat.Base.Jumping.long_;
#ifndef __aarch64__
								WriteMemoryFloat(libAddr + 0x392FDC, 0.17f * cheatClient->PlayerCheat.Base.Jumping.long__);
#endif
							}
						}
						ImGui::SameLine();
						if(cheatClient->PlayerCheat.Base.Jumping.high)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Высокий прыжок"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Jumping.high =! cheatClient->PlayerCheat.Base.Jumping.high;
								uintptr_t localplayer = cheatClient->FindLocalPlayer();
								if(localplayer)
								{
									CPed localped = CPed(localplayer);
									localped.SetHighJumpMult(70.f);
								}
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Высокий прыжок"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Jumping.high =! cheatClient->PlayerCheat.Base.Jumping.high;
								uintptr_t localplayer = cheatClient->FindLocalPlayer();
								if(localplayer)
								{
									CPed localped = CPed(localplayer);
									localped.SetHighJumpMult(70.f / cheatClient->PlayerCheat.Base.Jumping.high__);
								}
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(ImGui::Button(OBFUSCATE("Умереть"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							uintptr_t localplayer = cheatClient->FindLocalPlayer();
							if(localplayer)
							{
								CPed localped = CPed(localplayer);
								localped.SetHealth(0.f);
								localped.SetArmour(0.f);
							}
						}
						ImGui::SameLine();
						if(cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Ходьба под водой"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater =! cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Ходьба под водой"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater =! cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater;
							}
						}
						/*ImGui::SameLine();
						if(ImGui::Button("Высокий прыжок", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
						{
							BrNotification(TYPE_TEXT_GREEN, "CarShot", 5);
						}*/
					}
					
					if(current_player_category == 1)
					{
						ImGui::SameLine();
						ImGui::SameLine();
						if(cheatClient->PlayerCheat.Rage.AimBot.state)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("АимБот"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.AimBot.state =! cheatClient->PlayerCheat.Rage.AimBot.state;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("АимБот"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.AimBot.state =! cheatClient->PlayerCheat.Rage.AimBot.state;
							}
						}
						
						ImGui::SameLine();
						
						if(cheatClient->PlayerCheat.Rage.NoReload.state)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Стрельба без\nперезарядки"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.NoReload.state =! cheatClient->PlayerCheat.Rage.NoReload.state;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Стрельба без\nперезарядки"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.NoReload.state =! cheatClient->PlayerCheat.Rage.NoReload.state;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(cheatClient->PlayerCheat.Rage.Damager.state)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Дамагер"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.Damager.state =! cheatClient->PlayerCheat.Rage.Damager.state;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Дамагер"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.Damager.state =! cheatClient->PlayerCheat.Rage.Damager.state;
							}
						}
						ImGui::SameLine();
						if(cheatClient->PlayerCheat.Rage.Invulnerability.autoHeal)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Авто-аптечка"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.Invulnerability.autoHeal =! cheatClient->PlayerCheat.Rage.Invulnerability.autoHeal;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Авто-аптечка"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.Invulnerability.autoHeal =! cheatClient->PlayerCheat.Rage.Invulnerability.autoHeal;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(cheatClient->PlayerCheat.Rage.Invulnerability.autoArmour)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Авто-броня"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.Invulnerability.autoArmour =! cheatClient->PlayerCheat.Rage.Invulnerability.autoArmour;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Авто-броня"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->PlayerCheat.Rage.Invulnerability.autoArmour =! cheatClient->PlayerCheat.Rage.Invulnerability.autoArmour;
							}
						}
						ImGui::SliderFloat(OBFUSCATE("##healbelow"), &cheatClient->PlayerCheat.Rage.Invulnerability.healBelow, 1.f, 100.f);
						ImGui::SameLine(); ImGui::Text(OBFUSCATE("ХП для аптечки"));
						ImGui::SliderFloat(OBFUSCATE("##armourbelow"), &cheatClient->PlayerCheat.Rage.Invulnerability.armourBelow, 1.f, 100.f);
						ImGui::SameLine(); ImGui::Text(OBFUSCATE("Броня для авто-брони"));
						if(ImGui::Button(OBFUSCATE("Заспавнить себя"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							RakNet::BitStream bsSpawn, bsRequestSpawn;
							
							int requestSpawn = 129;
							int spawn = 52;
							
							//log_3(gpeer, &spawn, &bsSpawn, 1, 10, 0, ga7, ga8, 0, 0, -1, 0xFFFF, 0xFFFF, 0);
							log_3(gpeer, &requestSpawn, &bsRequestSpawn, 1, 8, 0, ga7, ga8, 0, 0, -1, 0xFFFF, 0xFFFF, 0);
						}
						
						/*ImGui::Spacing();
						ImGui::Separator();
						ImGui::Spacing();
						ImGui::SameLine();
						if(ImGui::Button("Test", ImVec2(ImGui::CalcTextSize("Test").x * 1.5f, ImGui::CalcTextSize("X").y * 2.f)))
						{
							uintptr_t localped = cheatClient->FindLocalPlayer();
							if(localped)
							{
								//SetSeek(localped, *(float *)(localped + 0x30) + 15.f, *(float *)(localped + 0x34) + 10.f, *(float *)(localped + 0x38), 0.5f);
								
								*(int *)(localped + 592) = 31;
								*(int *)(localped + 596) = 0;
								*(int *)(localped + 600) = 4;
								*(int *)(localped + 0x3EC) = 0;
								
								/**(int *)(localped + 612) = 0;
								*(int *)(localped + 616) = 0;
								*/
								//last_ms = time(0);
								/*SetMoveAnim(localped);
							}
						}
						
						uintptr_t localped = cheatClient->FindLocalPlayer();
						if(localped)
						{
							ImGui::Text("PedState: %d\nStoredState: %d\nMoveState: %d", *(int *)(localped + 592), *(int *)(localped + 596), *(int *)(localped + 600));
							ImGui::Text("bUsesCollision: %d", (*(int *)(localped + 0x50) & 0x100));
						}
						if(ImGui::Button("idle", ImVec2(ImGui::CalcTextSize("Test").x * 1.5f, ImGui::CalcTextSize("X").y * 2.f)))
						{
							uintptr_t localped = cheatClient->FindLocalPlayer();
							if(localped)
							{
								*(int *)(localped + 592) = 1;
								*(int *)(localped + 596) = 0;
								*(int *)(localped + 600) = 1;
								/**(int *)(localped + 612) = 0;
								*(int *)(localped + 616) = 0;
								//last_ms = time(0);*//*
								SetMoveAnim(localped);
							}
						}*/ 
					}
				}
				
				// Транспорт
				if(current_menu_category == 1)
				{
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					
					if(cheatClient->VehicleCheat.Invulnerability.health)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Бессмертие"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.Invulnerability.health =! cheatClient->VehicleCheat.Invulnerability.health;
							BrNotification(TYPE_TEXT_GREEN, OBFUSCATE("Бессмертие работает только от физики игры!"), 5);
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Бессмертие"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.Invulnerability.health =! cheatClient->VehicleCheat.Invulnerability.health;
							BrNotification(TYPE_TEXT_GREEN, OBFUSCATE("Бессмертие работает только от физики игры!"), 5);
						}
					}
					ImGui::SameLine();
					if(cheatClient->VehicleCheat.FlyCar.state)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("FlyCar"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.FlyCar.state =! cheatClient->VehicleCheat.FlyCar.state;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("FlyCar"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.FlyCar.state =! cheatClient->VehicleCheat.FlyCar.state;
						}
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					if(ImGui::Button(OBFUSCATE("Лаунч-контроль"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
					{
						uintptr_t localped = cheatClient->FindLocalPlayer();
						if(localped)
						{
							CPed localplayer = CPed(localped);
							if(localplayer.GetVehicle() != 0)
							{
								CVehicle localvehicle = CVehicle(localplayer.GetVehicle());
								if(localvehicle.GetDriver() == localplayer.GetMe())
								{
									localvehicle.SetLaunchState(1);
								}
							}
						}
					}
					ImGui::SameLine();
					if(ImGui::Button(OBFUSCATE("Гидравлика"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
					{
						uintptr_t localped = cheatClient->FindLocalPlayer();
						if(localped)
						{
							CPed localplayer = CPed(localped);
							if(localplayer.GetVehicle() != 0)
							{
								CVehicle localvehicle = CVehicle(localplayer.GetVehicle());
								if(localvehicle.GetDriver() == localplayer.GetMe())
								{
									if(localvehicle.GetHydraulic()) {
										localvehicle.SetHydraulic(false);
									} else {
										localvehicle.SetHydraulic(true);
									}
								}
							}
						}
					}
						
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					if(cheatClient->VehicleCheat.CarShot.state)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("CarShot"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.CarShot.state =! cheatClient->VehicleCheat.CarShot.state;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("CarShot"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.CarShot.state =! cheatClient->VehicleCheat.CarShot.state;
						}
					}
					ImGui::SameLine();
					if(cheatClient->VehicleCheat.Invulnerability.engine)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Вечный двигатель"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.Invulnerability.engine =! cheatClient->VehicleCheat.Invulnerability.engine;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Вечный двигатель"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.Invulnerability.engine =! cheatClient->VehicleCheat.Invulnerability.engine;
						}
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					if(ImGui::Button(OBFUSCATE("Сломать т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
					{
						uintptr_t localped = cheatClient->FindLocalPlayer();
						if(localped)
						{
							CPed localplayer = CPed(localped);
							if(localplayer.GetVehicle() != 0)
							{
								CVehicle localvehicle = CVehicle(localplayer.GetVehicle());
								if(localvehicle.GetDriver() == localped)
								{
									localvehicle.SetHealth(269.f);
								}
							}
							else
							{
								BrNotification(TYPE_TEXT_RED, OBFUSCATE("Вы должны находиться в транспорте"), 5);
							}
						}
					}
					ImGui::SameLine();
					if(cheatClient->Tuning.menu)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Тюнинг т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->Tuning.menu =! cheatClient->Tuning.menu;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Тюнинг т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->Tuning.menu =! cheatClient->Tuning.menu;
						}
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					if(cheatClient->PlayerCheat.Base.Invulnerability.noBike)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Не падать\nс мотоцикла"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->PlayerCheat.Base.Invulnerability.noBike =! cheatClient->PlayerCheat.Base.Invulnerability.noBike;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Не падать\nс мотоцикла"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->PlayerCheat.Base.Invulnerability.noBike =! cheatClient->PlayerCheat.Base.Invulnerability.noBike;
						}
					}
					ImGui::SameLine();
					if(cheatClient->VehicleCheat.Invulnerability.driveUnderwater)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Езда под водой"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.Invulnerability.driveUnderwater =! cheatClient->VehicleCheat.Invulnerability.driveUnderwater;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Езда под водой"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.Invulnerability.driveUnderwater =! cheatClient->VehicleCheat.Invulnerability.driveUnderwater;
						}
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					if(cheatClient->VehicleCheat.CarChanger.menu)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Поменять т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.CarChanger.menu =! cheatClient->VehicleCheat.CarChanger.menu;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Поменять т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							uintptr_t localped = cheatClient->FindLocalPlayer();
							if(localped)
							{
								CPed localplayer = CPed(localped);
								if(localplayer.GetVehicle() != 0)
								{
									CVehicle localvehicle = CVehicle(localplayer.GetVehicle());
									if(localvehicle.GetDriver() == localplayer.GetMe())
									{
										cheatClient->VehicleCheat.CarChanger.menu =! cheatClient->VehicleCheat.CarChanger.menu;
									}
									else
									{
										BrNotification(TYPE_TEXT_RED, OBFUSCATE("Вы должны находиться в транспорте!"), 5);
									}
								}
								else
								{
									BrNotification(TYPE_TEXT_RED, OBFUSCATE("Вы должны находиться в транспорте!"), 5);
								}
							}
						}
					}
					ImGui::SameLine();
					if(cheatClient->VehicleCheat.AntiDetachTrailer.state)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Анти Отцеп"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VehicleCheat.AntiDetachTrailer.state =! cheatClient->VehicleCheat.AntiDetachTrailer.state;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Анти Отцеп"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							uintptr_t localped = cheatClient->FindLocalPlayer();
							if(localped)
							{
								int trailer = cheatClient->GetAtVehiclePool(999);
								if(trailer)
								{
									uintptr_t veh = CPed(localped).GetVehicle();
									if(veh)
									{
										((void (*)(int,int,int))(libAddr + string2Offset(OBFUSCATE("0x2BA95F")) + 1))(trailer, veh, 1);
									}
								}
							}
							cheatClient->VehicleCheat.AntiDetachTrailer.state =! cheatClient->VehicleCheat.AntiDetachTrailer.state;
						}
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
				}
				
				// Визуал
				if(current_menu_category == 2)
				{
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					
					if(cheatClient->Wallhack.menu || cheatClient->Wallhack.Player.lines || cheatClient->Wallhack.Player.box || cheatClient->Wallhack.Player.collision || cheatClient->Wallhack.Player.skeleton || cheatClient->Wallhack.Player.model_id || cheatClient->Wallhack.Player.nametags
					|| cheatClient->Wallhack.Vehicle.lines || cheatClient->Wallhack.Vehicle.box || cheatClient->Wallhack.Vehicle.collision || cheatClient->Wallhack.Vehicle.model_id || cheatClient->Wallhack.Vehicle.model_name
					|| cheatClient->Wallhack.Object.lines || cheatClient->Wallhack.Object.box || cheatClient->Wallhack.Object.collision || cheatClient->Wallhack.Object.model_id || cheatClient->Wallhack.Object.model_name)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Wallhack"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->Wallhack.menu =! cheatClient->Wallhack.menu;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Wallhack"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->Wallhack.menu =! cheatClient->Wallhack.menu;
						}
					}
					ImGui::SameLine();
					if(cheatClient->CarSpawn.menu)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Создать т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->CarSpawn.menu =! cheatClient->CarSpawn.menu;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Создать т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->CarSpawn.menu =! cheatClient->CarSpawn.menu;
						}
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					if(cheatClient->VisualCheat.SkinChanger.menu)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Выдать скин"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VisualCheat.SkinChanger.menu =! cheatClient->VisualCheat.SkinChanger.menu;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Выдать скин"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VisualCheat.SkinChanger.menu =! cheatClient->VisualCheat.SkinChanger.menu;
						}
					}
					ImGui::SameLine();
					if(cheatClient->VisualCheat.MoneyChanger.menu)
					{
						ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
						if(ImGui::Button(OBFUSCATE("Выдать деньги"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VisualCheat.MoneyChanger.menu =! cheatClient->VisualCheat.MoneyChanger.menu;
						}
						ImGui::PopStyleColor();
					}
					else
					{
						if(ImGui::Button(OBFUSCATE("Выдать деньги"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							cheatClient->VisualCheat.MoneyChanger.menu =! cheatClient->VisualCheat.MoneyChanger.menu;
						}
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.8f);
					if (ImGui::CollapsingHeader(OBFUSCATE("Игровой мир"), ImGuiTreeNodeFlags_None))
        			{
						ImGui::Spacing();
						ImGui::SameLine();
						if(ImGui::SliderInt(OBFUSCATE("Время"), &cheatClient->VisualCheat.TimeChanger.time, 0, 24))
						{
#ifndef __aarch64__
							*(int *)(libAddr + 0x4B6263D) = cheatClient->VisualCheat.TimeChanger.time;
#else
							g_clockSet = cheatClient->VisualCheat.TimeChanger.time;
#endif
						}
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::Checkbox(OBFUSCATE("Заморозить время"), &cheatClient->VisualCheat.TimeChanger.freeze);
						ImGui::Spacing();
						ImGui::SameLine();
					}
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SameLine();
					
					/*ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.8f);
					if (ImGui::CollapsingHeader("Field of View (угол обзора)", ImGuiTreeNodeFlags_None))
        			{
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SliderFloat("FOV", &cheatClient->VisualCheat.Camera.FOV, 10.f, 170.f);
						ImGui::Spacing();
						ImGui::SameLine();
						if(ImGui::Button("Сбросить FOV", ImVec2(ImGui::CalcTextSize("Сбросить FOV").x * 2.5f, ImGui::CalcTextSize("W").y * 2.f)))
						{
							cheatClient->VisualCheat.Camera.FOV = 70.f;
						}
					}*/
				}
					
				// Другое
				if(current_menu_category == 3)
				{
					ImGui::Spacing();
					ImGui::Spacing();
					
					ImVec2 currentCursorPos = ImGui::GetCursorPos();
					
					ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(17,17,17));
					ImGui::SameLine();
					ImGui::BeginChild(OBFUSCATE("##player-category"), ImVec2(ImGui::GetContentRegionAvail().x, categoryButtonHeight * 1.2f));
					{
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2));
						ImGui::SetCursorPosY((categoryButtonHeight * 1.2f - categoryButtonHeight) / 2);
						if(current_other_category == 0)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							ImGui::Button(OBFUSCATE(u8"Основное"), ImVec2((ImGui::GetWindowWidth() / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2), categoryButtonHeight));
							ImGui::PopStyleColor();
						}
						else
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(17,17,17));
							if(ImGui::Button(OBFUSCATE(u8"Основное"), ImVec2((ImGui::GetWindowWidth() / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2), categoryButtonHeight)))
							{
								current_other_category = 0;
							}
							ImGui::PopStyleColor();
						}
						
						ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2) + ((ImGui::GetWindowWidth() / 2) - (((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2)));
						ImGui::SetCursorPosY((categoryButtonHeight * 1.2f - categoryButtonHeight) / 2);
						if(current_other_category == 1)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							ImGui::Button(OBFUSCATE(u8"Авто-фарм"), ImVec2((ImGui::GetWindowWidth() / 2) - ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2), categoryButtonHeight));
							ImGui::PopStyleColor();
						}
						else
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(17,17,17));
							if(ImGui::Button(OBFUSCATE(u8"Авто-фарм"), ImVec2((ImGui::GetWindowWidth() / 2) - ((((categoryButtonHeight * 1.2f) - categoryButtonHeight) / 2) * 2), categoryButtonHeight)))
							{
								current_other_category = 1;
							}
							ImGui::PopStyleColor();
						}
					}
					ImGui::EndChild();
					ImGui::PopStyleColor();
					
					ImGui::Spacing();
					
					if(current_other_category == 0)
					{
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(cheatClient->GameSpeed.menu || cheatClient->GameSpeed.state)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Скорость игры"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->GameSpeed.menu =! cheatClient->GameSpeed.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Скорость игры"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->GameSpeed.menu =! cheatClient->GameSpeed.menu;
							}
						}
						ImGui::SameLine();
						if(cheatClient->Collision.menu || cheatClient->Collision.ped || cheatClient->Collision.vehicle || cheatClient->Collision.objects)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Колизии"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->Collision.menu =! cheatClient->Collision.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Колизии"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->Collision.menu =! cheatClient->Collision.menu;
							}
						}

						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();

						static bool debugMenu = false;
						if(debugMenu)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Дебаг меню"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								debugMenu = false;
								ToggleClientDebugMenu(false);
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Дебаг меню"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								debugMenu = true;
								ToggleClientDebugMenu(true);
							}
						}

						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();

						if(hospitalUnlock)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Выход из больницы"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
								hospitalUnlock = false;
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Выход из больницы"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
								hospitalUnlock = true;
						}

						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						/*if(cheatClient->FakeChat.menu || cheatClient->FakeChat.state)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button("Fake Chat", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->FakeChat.menu =! cheatClient->FakeChat.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button("Fake Chat", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->FakeChat.menu =! cheatClient->FakeChat.menu;
							}
						}
						ImGui::SameLine();
						if(cheatClient->Flooder.menu || cheatClient->Flooder.working)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button("Флудер", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->Flooder.menu =! cheatClient->Flooder.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button("Флудер", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->Flooder.menu =! cheatClient->Flooder.menu;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();*/
						
						if(ImGui::Button(OBFUSCATE("ТП к маркеру"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							if(cheatClient->Teleport.toCheckpoint)
							{
								cheatClient->Teleport.toMarker = true;
								cheatClient->Teleport.toCheckpoint = false;
							}
							else
							{
								cheatClient->Teleport.toMarker = true;
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("ТП к чекпоинту"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
						{
							if(cheatClient->Teleport.toMarker)
							{
								cheatClient->Teleport.toMarker = false;
								cheatClient->Teleport.OnFoot.speed = 0;
								cheatClient->Teleport.OnFoot.speed_max = 50.f;
								cheatClient->Teleport.toCheckpoint = true;
							}
							else
							{
								cheatClient->Teleport.toCheckpoint = true;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(cheatClient->Teleport.toPeds)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("ТП к ближ.\n   игрокам"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->Teleport.toPeds =! cheatClient->Teleport.toPeds;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("ТП к ближ.\n   игрокам"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->Teleport.toPeds =! cheatClient->Teleport.toPeds;
							}
						}
						ImGui::SameLine();
						if(cheatClient->Teleport.toCars)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("ТП к ближ. т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->Teleport.toCars =! cheatClient->Teleport.toCars;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("ТП к ближ. т/с"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->Teleport.toCars =! cheatClient->Teleport.toCars;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
					}
					
					if(current_other_category == 1)
					{
						ImGui::SameLine();
						ImGui::SameLine();
						if(cheatClient->AutoFarm.BotMiner.menu)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Бот Шахта"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->AutoFarm.BotMiner.menu =! cheatClient->AutoFarm.BotMiner.menu;
#ifndef __aarch64__
								uintptr_t localped = cheatClient->FindLocalPlayer();
								if(localped)
								{
									// Сброс анимации
									*(int *)(localped + 592) = 1;
									*(int *)(localped + 596) = 0;
									*(int *)(localped + 600) = 1;
									*(int *)(localped + 612) = 0;
									*(int *)(localped + 616) = 0;
									SetMoveAnim(localped);
								}
#endif
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Бот Шахта"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->AutoFarm.BotMiner.menu =! cheatClient->AutoFarm.BotMiner.menu;
							}
						}
						ImGui::SameLine();
						if(cheatClient->AutoFarm.BotFactory.menu)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Бот Завод"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->AutoFarm.BotFactory.menu =! cheatClient->AutoFarm.BotFactory.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Бот Завод"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->AutoFarm.BotFactory.menu =! cheatClient->AutoFarm.BotFactory.menu;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						/*if(cheatClient->AutoFarm.BotFarm.menu)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button("Бот Ферма", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotFarm.menu =! cheatClient->AutoFarm.BotFarm.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button("Бот Ферма", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotFarm.menu =! cheatClient->AutoFarm.BotFarm.menu;
							}
						}
						ImGui::SameLine();
						if(cheatClient->AutoFarm.BotCourier.menu)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button("Бот Курьер", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotCourier.menu =! cheatClient->AutoFarm.BotCourier.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button("Бот Курьер", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotCourier.menu =! cheatClient->AutoFarm.BotCourier.menu;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();
						
						if(cheatClient->AutoFarm.BotBus.menu)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button("Бот Автобус", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotBus.menu =! cheatClient->AutoFarm.BotBus.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button("Бот Автобус", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotBus.menu =! cheatClient->AutoFarm.BotBus.menu;
							}
						}
						ImGui::SameLine();
						if(cheatClient->AutoFarm.BotTrucker.menu)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button("Бот Дальнобойщик", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotTrucker.menu =! cheatClient->AutoFarm.BotTrucker.menu;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button("Бот Дальнобойщик", ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize("X").y * 3.f)))
							{
								cheatClient->AutoFarm.BotTrucker.menu =! cheatClient->AutoFarm.BotTrucker.menu;
							}
						}
						
						ImGui::Spacing();
						ImGui::Spacing();
						ImGui::SameLine();
						ImGui::SameLine();*/
						
						if(cheatClient->AutoFarm.AntiAfk.state)
						{
							ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
							if(ImGui::Button(OBFUSCATE("Анти АФК"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
								cheatClient->AutoFarm.AntiAfk.state =! cheatClient->AutoFarm.AntiAfk.state;
							}
							ImGui::PopStyleColor();
						}
						else
						{
							if(ImGui::Button(OBFUSCATE("Анти АФК"), ImVec2(ImGui::GetWindowWidth() * 0.425f, ImGui::CalcTextSize(OBFUSCATE("X")).y * 3.f)))
							{
#ifndef __aarch64__
								uintptr_t localped = cheatClient->FindLocalPlayer();
								*(int *)(localped + 592) = 5;
								*(int *)(localped + 596) = 0;
								*(int *)(localped + 600) = 4;
								*(int *)(localped + 612) = 0;
								*(int *)(localped + 616) = 0;
								SetMoveAnim(localped);
#endif
								cheatClient->AutoFarm.AntiAfk.state =! cheatClient->AutoFarm.AntiAfk.state;
							}
						}
					}
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
		}
		ImGui::End();
	}
	
	if(cheatClient->GameSpeed.menu)
	{
		ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
		if(!game_speed_inited)
		{
			ImGui::SetNextWindowSize(ImVec2(350, 250));
			game_speed_inited = true;
		}
		ImGui::Begin(OBFUSCATE("Скорость игры"), 0);
		{
			ImGui::SliderFloat(OBFUSCATE("##gamespeed"), &cheatClient->GameSpeed.speed, 1.f, 5.f);
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}
	
	if(cheatClient->VehicleCheat.CarChanger.menu)
	{
		uintptr_t localped = cheatClient->FindLocalPlayer();
		if(localped)
		{
			CPed localplayer = CPed(localped);
			if(localplayer.GetVehicle())
			{
				CVehicle localvehicle = CVehicle(localplayer.GetVehicle());
				if(localvehicle.GetDriver() ==  localplayer.GetMe())
				{
					ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
					ImGui::Begin(OBFUSCATE("Подмена т/с"), 0);
					{
						ImGui::BeginChild(OBFUSCATE("##changecarnumpadinput"), ImVec2(((ImGui::CalcTextSize(OBFUSCATE("1")).x * 7.f) * 3.f), ImGui::CalcTextSize(OBFUSCATE("1")).y * 2.5f));
						{
							ImGui::Spacing();
							ImGui::Spacing();
							ImGui::Spacing();
							ImGui::SameLine();
							ImGui::Text("%s", cheatClient->VehicleCheat.CarChanger.numpad.c_str());
						}
						ImGui::EndChild();
				
						ImGui::Spacing();
				
						if(ImGui::Button(OBFUSCATE("1"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("1");
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("2"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("2");
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("3"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("3");
							}
						}
						ImGui::Spacing();
						if(ImGui::Button(OBFUSCATE("4"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("4");
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("5"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("5");
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("6"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("6");
							}
						}
						ImGui::Spacing();
						if(ImGui::Button(OBFUSCATE("7"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("7");
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("8"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("8");
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("9"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("9");
							}
						}
						ImGui::Spacing();
						if(ImGui::Button(OBFUSCATE("<<"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() > 0)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.erase(cheatClient->VehicleCheat.CarChanger.numpad.size() - 1);
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("0"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							if(cheatClient->VehicleCheat.CarChanger.numpad.size() < 5 && cheatClient->VehicleCheat.CarChanger.numpad.size() > 0)
							{
								cheatClient->VehicleCheat.CarChanger.numpad.append("0");
							}
						}
						ImGui::SameLine();
						if(ImGui::Button(OBFUSCATE("OK"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
						{
							cheatClient->VehicleCheat.CarChanger.entered = true;
							cheatClient->VehicleCheat.CarChanger.menu = false;
						}
					}
					ImGui::End();
					ImGui::PopStyleVar();
				}				
			}
		}
	}
	
	if(cheatClient->CarSpawn.menu)
	{
		uintptr_t localped = cheatClient->FindLocalPlayer();
		if(localped)
		{
			ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
			ImGui::Begin(OBFUSCATE("Создание т/с"), 0);
			{
				ImGui::BeginChild(OBFUSCATE("##createcarnumpadinput"), ImVec2(((ImGui::CalcTextSize("1").x * 7.f) * 3.f), ImGui::CalcTextSize("1").y * 2.5f));
				{
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::Text("%s", cheatClient->CarSpawn.numpad.c_str());
				}
				ImGui::EndChild();
				
				ImGui::Spacing();
				
				if(ImGui::Button(OBFUSCATE("1"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("1");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("2"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("2");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("3"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("3");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("4"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("4");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("5"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("5");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("6"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("6");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("7"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("7");
					}
				}
				ImGui::SameLine();
					
				if(ImGui::Button(OBFUSCATE("8"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("8");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("9"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5)
					{
						cheatClient->CarSpawn.numpad.append("9");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("<<"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() > 0)
					{
						cheatClient->CarSpawn.numpad.erase(cheatClient->CarSpawn.numpad.size() - 1);
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("0"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->CarSpawn.numpad.size() < 5 && cheatClient->CarSpawn.numpad.size() > 0)
					{
						cheatClient->CarSpawn.numpad.append("0");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("OK"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					cheatClient->CarSpawn.entered = true;
					cheatClient->CarSpawn.menu = false;
				}
			}
			ImGui::End();
			ImGui::PopStyleVar();
		}				
	}
	
	if(cheatClient->VisualCheat.SkinChanger.menu)
	{
		uintptr_t localped = cheatClient->FindLocalPlayer();
		if(localped)
		{
			ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
			ImGui::Begin(OBFUSCATE("Выдача скина"), 0);
			{
				ImGui::BeginChild(OBFUSCATE("##giveskinnumpadinput"), ImVec2(((ImGui::CalcTextSize("1").x * 7.f) * 3.f), ImGui::CalcTextSize("1").y * 2.5f));
				{
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::Text("%s", cheatClient->VisualCheat.SkinChanger.numpad.c_str());
				}
				ImGui::EndChild();
				
				ImGui::Spacing();
				
				if(ImGui::Button(OBFUSCATE("1"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("1");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("2"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("2");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("3"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("3");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("4"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("4");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("5"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("5");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("6"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("6");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("7"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("7");
					}
				}
				ImGui::SameLine();
					
				if(ImGui::Button(OBFUSCATE("8"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("8");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("9"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("9");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("<<"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() > 0)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.erase(cheatClient->VisualCheat.SkinChanger.numpad.size() - 1);
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("0"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.SkinChanger.numpad.size() < 5 && cheatClient->VisualCheat.SkinChanger.numpad.size() > 0)
					{
						cheatClient->VisualCheat.SkinChanger.numpad.append("0");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("OK"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					cheatClient->VisualCheat.SkinChanger.entered = true;
					cheatClient->VisualCheat.SkinChanger.menu = false;
				}
			}
			ImGui::End();
			ImGui::PopStyleVar();
		}				
	}
	
	if(cheatClient->VisualCheat.MoneyChanger.menu)
	{
		uintptr_t localped = cheatClient->FindLocalPlayer();
		if(localped)
		{
			ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
			ImGui::Begin(OBFUSCATE("Выдача денег"), 0);
			{
				ImGui::BeginChild(OBFUSCATE("##givemoneynumpadinput"), ImVec2(((ImGui::CalcTextSize("1").x * 7.f) * 3.f), ImGui::CalcTextSize("1").y * 2.5f));
				{
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::Text("%s", cheatClient->VisualCheat.MoneyChanger.numpad.c_str());
				}
				ImGui::EndChild();
				
				ImGui::Spacing();
				
				if(ImGui::Button(OBFUSCATE("1"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("1");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("2"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("2");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("3"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("3");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("4"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("4");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("5"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("5");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("6"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("6");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("7"), ImVec2(ImGui::CalcTextSize("1").x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("7");
					}
				}
				ImGui::SameLine();
					
				if(ImGui::Button(OBFUSCATE("8"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("8");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("9"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("9");
					}
				}
				ImGui::Spacing();
				if(ImGui::Button(OBFUSCATE("<<"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() > 0)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.erase(cheatClient->VisualCheat.MoneyChanger.numpad.size() - 1);
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("0"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					if(cheatClient->VisualCheat.MoneyChanger.numpad.size() < 10 && cheatClient->VisualCheat.MoneyChanger.numpad.size() > 0)
					{
						cheatClient->VisualCheat.MoneyChanger.numpad.append("0");
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("OK"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("1")).x * 6.f, ImGui::CalcTextSize(OBFUSCATE("1")).y * 3.f)))
				{
					cheatClient->VisualCheat.MoneyChanger.entered = true;
					cheatClient->VisualCheat.MoneyChanger.menu = false;
				}
			}
			ImGui::End();
			ImGui::PopStyleVar();
		}				
	}
	
	if(cheatClient->Teleport.toPeds)
	{
		ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
		if(!tp_toPeds_inited)
		{
			ImGui::SetNextWindowSize(ImVec2(1000, 400));
			tp_toPeds_inited = true;
		}
		ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(17,17,17,175));
		ImGui::Begin(OBFUSCATE("ТП к ближайшим игрокам"), 0);
		{
			for(int i = 0; i <= 1500; i++)
			{
				int player = cheatClient->GetAtPlayerPool(i);
				if(player)
				{
					player = *(uintptr_t *)(player + PLAYER_PED_OFF);
					if(player > 0x100000)
					{
						const char* nickname = cheatClient->ServerPlayers[i].nickname;
						int id = i;
						
						for(int a = 0; a < sizeof(cheatClient->ServerPlayers); a++)
						{
							if(cheatClient->ServerPlayers[a].id == i)
							{
								nickname = cheatClient->ServerPlayers[a].nickname;
								id = cheatClient->ServerPlayers[a].id;
								break;
							}
						}
						
						uintptr_t localped = cheatClient->FindLocalPlayer();
						if(localped)
						{
							Vec3 localcoors = cheatClient->GetPos(localped);
							Vec3 targetcoors = cheatClient->GetPos(player);
							
							float dist = Get3dDistance(*(float *)(localped + ENT_POS_X), *(float *)(localped + ENT_POS_Y), *(float *)(localped + ENT_POS_Z), *(float *)(player + ENT_POS_X), *(float *)(player + ENT_POS_Y), *(float *)(player + ENT_POS_Z));
							
							ImGui::Text(OBFUSCATE("%s [%d] - Дистанция: %.2f"), nickname, id, dist);
							ImGui::SameLine();
							float offset = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().ItemInnerSpacing.x - ImGui::CalcTextSize("Телепортация").x;
							ImGui::Dummy(ImVec2(offset, 0));
							ImGui::SameLine();
							ImGui::PushID(i);
							if (ImGui::Button(OBFUSCATE("Телепортация")))
							{
    							// Телепорт
								cheatClient->SetPos(localped, targetcoors);
							}
							ImGui::PopID();
						}
					}
				}
			}
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
	
	if(cheatClient->Teleport.toCars)
	{
		ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
		if(!tp_toPeds_inited)
		{
			ImGui::SetNextWindowSize(ImVec2(900, 400));
			tp_toPeds_inited = true;
		}
		ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(17,17,17,175));
		ImGui::Begin(OBFUSCATE("ТП к ближайшему транспорту"), 0);
		{
			for(int i = 0; i < 1500; i++)
			{
				int vehicle = cheatClient->GetAtVehiclePool(i);
				if(vehicle)
				{
					unsigned short car_model_id = *(unsigned short *)(vehicle + 0x5A);
					
					CVehicle veh(vehicle);
					uintptr_t driver = veh.GetDriver();
					
					Vec3 local_coors = cheatClient->GetPos(cheatClient->FindLocalPlayer());
					Vec3 car_coors = cheatClient->GetPos(vehicle);
					float dist = Get2dDistance(local_coors, car_coors);
					
					if(driver)
					{
						int driver_id = NULL;
						for(int i = 0; i < 1500; i++)
						{
							uintptr_t player = cheatClient->GetAtPlayerPool(i);
							if(player)
							{
								player = *(uintptr_t *)(player + PLAYER_PED_OFF);
								if(player > 0x100000)
								{
									if(player == driver)
									{
										driver_id = i;
										break;
									}
								}
							}
						}
						
						char buff[64];
						sprintf(buff, OBFUSCATE("Водитель - %s [%d], ID модели: %d, Дистанция: %.2f"), cheatClient->ServerPlayers[driver_id].nickname, driver_id, car_model_id, dist);
						ImGui::Text(buff);
						ImGui::SameLine();
						float offset = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().ItemInnerSpacing.x - ImGui::CalcTextSize("Телепортация").x;
						ImGui::Dummy(ImVec2(offset, 0));
						ImGui::SameLine();
						ImGui::PushID(i);
						if (ImGui::Button(OBFUSCATE("Телепортация")))
						{
    						// Телепорт
							cheatClient->SetPos(cheatClient->FindLocalPlayer(), car_coors);
						}
						ImGui::PopID();
					}
					else
					{
						char buff[64];
						sprintf(buff, OBFUSCATE("Водитель - NULL, ID модели: %d, Дистанция: %.2f"), car_model_id, dist);
						ImGui::Text(buff);
						ImGui::SameLine();
						float offset = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().ItemInnerSpacing.x - ImGui::CalcTextSize("Телепортация").x;
						ImGui::Dummy(ImVec2(offset, 0));
						ImGui::SameLine();
						ImGui::PushID(i);
						if (ImGui::Button(OBFUSCATE("Телепортация")))
						{
    						// Телепорт
							cheatClient->SetPos(cheatClient->FindLocalPlayer(), car_coors);
						}
						ImGui::PopID();
					}
				}
			}
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
	
	if(cheatClient->Flooder.menu)
	{
		ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(17,17,17,225));
		ImGui::Begin(OBFUSCATE("Флудер"), 0);
		{
			ImGui::Text(OBFUSCATE(u8"Текст: %s"), cheatClient->Flooder.message.c_str());
			ImGui::SameLine();
			float offset = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().ItemInnerSpacing.x - ImGui::CalcTextSize("Телепортация").x;
			ImGui::Dummy(ImVec2(offset, 0));
			ImGui::SameLine();
			ImGui::PushID(71817);
			if (ImGui::Button(OBFUSCATE("Изменить")))
			{
				cheatClient->Flooder.wait_text_input = true;
				cheatClient->Flooder.menu = false;
				BrNotification(TYPE_TEXT_GREEN, OBFUSCATE("Введите сообщение для флуда в чат"), 10);
			}
			
			ImGui::Text(OBFUSCATE("Задержка: %d мс"), cheatClient->Flooder.delay_after_message);
			ImGui::SameLine();
			offset = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().ItemInnerSpacing.x - ImGui::CalcTextSize("Телепортация").x;
			ImGui::Dummy(ImVec2(offset, 0));
			ImGui::SameLine();
			ImGui::PushID(7187);
			if (ImGui::Button(OBFUSCATE("Изменить")))
			{
    			// Телепорт
				cheatClient->Flooder.wait_delay_input = true;
				cheatClient->Flooder.menu = false;
				BrNotification(TYPE_TEXT_GREEN, OBFUSCATE("Введите в чат желаемую задержку в миллисекундах.\n1000 миллисекунд равны 1 секунде!"), 10);
			}
			
			if(cheatClient->Flooder.working)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, rgba(255,55,55));
				if(ImGui::Button(OBFUSCATE("Стоп"), ImVec2(ImGui::GetWindowWidth() * 0.9f, ImGui::CalcTextSize("i").y * 3.5f)))
				{
					cheatClient->Flooder.working = false;
				}
				ImGui::PopStyleColor();
			}
			else
			{
				if(ImGui::Button(OBFUSCATE("Старт"), ImVec2(ImGui::GetWindowWidth() * 0.9f, ImGui::CalcTextSize("i").y * 3.5f)))
				{
					cheatClient->Flooder.working = true;
				}
			}
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
	
	if(cheatClient->Tuning.menu)
	{
		ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
		ImGui::Begin(OBFUSCATE("Тюнинг меню"), 0);
		{
			ImGui::BeginChild(OBFUSCATE("##tuning-menu"), ImVec2(1200, ImGui::CalcTextSize("X").y * 21.f));
			{
				ImGui::Spacing();
				ImGui::Spacing();
				ImGui::Spacing();
						
				ImGui::SameLine();
				if (ImGui::CollapsingHeader(OBFUSCATE("Настройка цвета"), ImGuiTreeNodeFlags_None))
        		{
					ImGui::SetNextItemWidth(500);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
					ImGui::ColorEdit3(OBFUSCATE("Кузов"), cheatClient->Tuning.body_color);
					ImGui::PopStyleVar();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
					ImGui::SetNextItemWidth(500);
					ImGui::ColorEdit3(OBFUSCATE("Диски"), cheatClient->Tuning.wheels_color);
					ImGui::PopStyleVar();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
					ImGui::SetNextItemWidth(500);
					ImGui::ColorEdit3(OBFUSCATE("Фары"), cheatClient->Tuning.lights_color);
					ImGui::PopStyleVar();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
					ImGui::SetNextItemWidth(500);
					ImGui::ColorEdit3(OBFUSCATE("Свечение под авто"), cheatClient->Tuning.glow_color);
					ImGui::PopStyleVar();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
					ImGui::SetNextItemWidth(500);
					ImGui::ColorEdit3(OBFUSCATE("Свечение под авто (справа)"), cheatClient->Tuning.right_glow_color);
					ImGui::PopStyleVar();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
					ImGui::SetNextItemWidth(500);
					ImGui::ColorEdit3(OBFUSCATE("Свечение под авто (слева)"), cheatClient->Tuning.left_glow_color);
					ImGui::PopStyleVar();
				}
				ImGui::Spacing();
				ImGui::SameLine();
				if (ImGui::CollapsingHeader(OBFUSCATE("Настройка фар"), ImGuiTreeNodeFlags_None))
        		{
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::Checkbox(OBFUSCATE("Страбоскопы"), &cheatClient->Tuning.stroboscopes);
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(500);
					ImGui::SliderInt(OBFUSCATE("Тип страбоскопов (0 - отсутствуют)"), &cheatClient->Tuning.stroboscopesType, 0, 4);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::Checkbox(OBFUSCATE("Дальний свет"), &cheatClient->Tuning.longLight);
					ImGui::Spacing();
				}
				ImGui::Spacing();
				ImGui::SameLine();
				if (ImGui::CollapsingHeader(OBFUSCATE("Настройка подвески"), ImGuiTreeNodeFlags_None))
        		{
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::Checkbox(OBFUSCATE("Общий клиренс"), &cheatClient->Tuning.totalClearance);
					ImGui::SameLine();
					ImGui::Checkbox(OBFUSCATE("Отдельный клиренс"), &cheatClient->Tuning.separateClearance);
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(700);
					ImGui::SliderInt(OBFUSCATE("Общий клиренс##1"), &cheatClient->Tuning.total_clearance_val, 0, 100);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(700);
					ImGui::SliderInt(OBFUSCATE("Отдельный клиренс##1"), &cheatClient->Tuning.separate_clearance_val, 0, 100);
					ImGui::Spacing();
				}
				ImGui::Spacing();
				ImGui::SameLine();
				if (ImGui::CollapsingHeader(OBFUSCATE("Настройка колес"), ImGuiTreeNodeFlags_None))
        		{
					ImGui::SetNextItemWidth(500);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SliderInt(OBFUSCATE("Радиус колес"), &cheatClient->Tuning.wheels_radius, 0, 100);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(500);
					ImGui::SliderInt(OBFUSCATE("Вылет передних колес"), &cheatClient->Tuning.departure_front_wheels, 0, 100);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(500);
					ImGui::SliderInt(OBFUSCATE("Вылет задних колес"), &cheatClient->Tuning.departure_back_wheels, 0, 100);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(500);
					ImGui::SliderInt(OBFUSCATE("Развал передних колес"), &cheatClient->Tuning.collapse_front_wheels, 0, 100);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(500);
					ImGui::SliderInt(OBFUSCATE("Развал задних колес"), &cheatClient->Tuning.collapse_back_wheels, 0, 100);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(500);
					ImGui::SliderInt(OBFUSCATE("Ширина передних колес"), &cheatClient->Tuning.width_front_wheels, 0, 100);
					ImGui::Spacing();
					ImGui::SameLine();
					ImGui::SetNextItemWidth(500);
					ImGui::SliderInt(OBFUSCATE("Ширина задних колес"), &cheatClient->Tuning.width_back_wheels, 0, 100);
					ImGui::Separator();
				}
				ImGui::Spacing();
				ImGui::SameLine();
				if(ImGui::Button(OBFUSCATE("Применить"), ImVec2(ImGui::CalcTextSize(OBFUSCATE("Применить")).x * 2.5f, ImGui::CalcTextSize(OBFUSCATE("I")).y * 2.f)))
				{
					uintptr_t localped = cheatClient->FindLocalPlayer();
					if(localped)
					{
						CPed localplayer = CPed(localped);
						if(localplayer.InVehicle())
						{
							uintptr_t vehicle = localplayer.GetVehicle();
							if(vehicle)
							{
								// цвет кузова
								*(char *)(vehicle + 0x7DE) = cheatClient->Tuning.body_color[0];
								*(char *)(vehicle + 0x7DD) = cheatClient->Tuning.body_color[1];
								*(char *)(vehicle + 0x7DC) = cheatClient->Tuning.body_color[2];
								// цвет дисков
								*(char *)(vehicle + 0x7E2) = cheatClient->Tuning.wheels_color[0];
								*(char *)(vehicle + 0x7E1) = cheatClient->Tuning.wheels_color[1];
								*(char *)(vehicle + 0x7E0) = cheatClient->Tuning.wheels_color[2];
								// цвет фар
								*(char *)(vehicle + 0x7B2) = cheatClient->Tuning.lights_color[0];
								*(char *)(vehicle + 0x7B1) = cheatClient->Tuning.lights_color[1];
								*(char *)(vehicle + 0x7B0) = cheatClient->Tuning.lights_color[2];
								// цвет свечения под авто
								*(char *)(vehicle + 0x7EE) = cheatClient->Tuning.glow_color[0];
								*(char *)(vehicle + 0x7ED) = cheatClient->Tuning.glow_color[1];
								*(char *)(vehicle + 0x7EC) = cheatClient->Tuning.glow_color[2];
								// справа
								*(char *)(vehicle + 0x7F2) = cheatClient->Tuning.right_glow_color[0];
								*(char *)(vehicle + 0x7F1) = cheatClient->Tuning.right_glow_color[1];
								*(char *)(vehicle + 0x7F0) = cheatClient->Tuning.right_glow_color[2];
								// слева
								*(char *)(vehicle + 0x7F6) = cheatClient->Tuning.left_glow_color[0];
								*(char *)(vehicle + 0x7F5) = cheatClient->Tuning.left_glow_color[1];
								*(char *)(vehicle + 0x7F4) = cheatClient->Tuning.left_glow_color[2];
								
								// фары
								*(int *)(vehicle + 0x7FC) = cheatClient->Tuning.stroboscopesType;
								*(bool *)(vehicle + 0x7F8) = cheatClient->Tuning.stroboscopes;
								*(bool *)(vehicle + 0x2BF) = cheatClient->Tuning.longLight;
								
								// подвеска и колеса
								
								*(float *)(vehicle + 0x77C) = 0.2f + (0.018f * cheatClient->Tuning.total_clearance_val);
								*(float *)(vehicle + 0x780) = 0.1f + (0.008f * cheatClient->Tuning.separate_clearance_val);
								
								*(float *)(vehicle + 0x7CC) = 0.7f + (0.004f * cheatClient->Tuning.wheels_radius);
								
								*(float *)(vehicle + 0x7BC) = -0.125f + (0.0025f * cheatClient->Tuning.departure_front_wheels);
								*(float *)(vehicle + 0x7C0) = -0.125f + (0.0025f * cheatClient->Tuning.departure_back_wheels);
								
								*(float *)(vehicle + 0x7B4) = -0.5f + (0.005f * cheatClient->Tuning.collapse_front_wheels);
								*(float *)(vehicle + 0x7B8) = -0.5f + (0.005f * cheatClient->Tuning.collapse_back_wheels);
								
								*(float *)(vehicle + 0x7C4) = 0.5f + (0.0125f * cheatClient->Tuning.width_front_wheels);
								*(float *)(vehicle + 0x7C8) = 0.5f + (0.0125f * cheatClient->Tuning.width_back_wheels);
								
							}
						}
					}
				}
				ImGui::Spacing();
			}
			ImGui::EndChild();
		}
		ImGui::End();
	}
	
	if(cheatClient->PlayerCheat.Rage.AimBot.state)
	{
		ImGui::GetBackgroundDrawList()->AddCircleFilled(ImVec2(cheatClient->screen_width * crosshair_offset_x, cheatClient->screen_height * crosshair_offset_y), 3.f, IM_COL32(255, 0, 0, 255), 32);
	}
	
	if(cheatClient->Wallhack.menu)
	{
		ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(17,17,17,200));
		ImGui::Begin(OBFUSCATE("Wallhack"), 0);
		{
			if (ImGui::BeginTabBar(OBFUSCATE("##wallhacktabs"))) {
    			// Создание первой вкладки
    			if (ImGui::BeginTabItem(OBFUSCATE("Игроки"))) {
        			// Код для содержимого первой вкладки
        			ImGui::Spacing();
					ImGui::SameLine();
					
					ImGui::Checkbox(OBFUSCATE("Линии"), &cheatClient->Wallhack.Player.lines);
					
					ImGui::SameLine();
					ImGui::SameLine();
					
					ImGui::Checkbox(OBFUSCATE("Коробка"), &cheatClient->Wallhack.Player.box);
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					
					ImGui::Checkbox(OBFUSCATE("Скелет"), &cheatClient->Wallhack.Player.skeleton);
					
					ImGui::SameLine();
					ImGui::SameLine();
					
					/*ImGui::Checkbox(OBFUSCATE("3D Коробка"), &cheatClient->Wallhack.Player.collision);
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					*/
					ImGui::Checkbox(OBFUSCATE("ID скина"), &cheatClient->Wallhack.Player.model_id);
					
					/*ImGui::SameLine();
					ImGui::SameLine();*/
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					
					ImGui::Checkbox(OBFUSCATE("Никнеймы"), &cheatClient->Wallhack.Player.nametags);
					
					ImGui::EndTabItem();
   				}
				
    			// Создание второй вкладки
    			if (ImGui::BeginTabItem(OBFUSCATE("Транспорт"))) {
        		// Код для содержимого второй вкладки
        			ImGui::Spacing();
					ImGui::SameLine();
					
					ImGui::Checkbox(OBFUSCATE("Линии"), &cheatClient->Wallhack.Vehicle.lines);
					
					ImGui::SameLine();
					ImGui::SameLine();
					
					//ImGui::Checkbox(OBFUSCATE("Коробка"), &cheatClient->Wallhack.Vehicle.box);
					
					/*ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					
					/*ImGui::Checkbox(OBFUSCATE("3D Коробка"), &cheatClient->Wallhack.Vehicle.collision);
					
					ImGui::SameLine();
					ImGui::SameLine();
					*/
					ImGui::Checkbox(OBFUSCATE("ID транспорта"), &cheatClient->Wallhack.Vehicle.model_id);
					
					/*ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();*/
    			}
				
				if (ImGui::BeginTabItem(OBFUSCATE("Объекты"))) {
        		// Код для содержимого второй вкладки
        			ImGui::Spacing();
					ImGui::SameLine();
					
					ImGui::Checkbox(OBFUSCATE("Линии"), &cheatClient->Wallhack.Object.lines);
					
					ImGui::SameLine();
					ImGui::SameLine();
					
					/*ImGui::Checkbox(OBFUSCATE("Коробка"), &cheatClient->Wallhack.Object.box);
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
					
					/*ImGui::Checkbox(OBFUSCATE("3D Коробка"), &cheatClient->Wallhack.Object.collision);
					
					ImGui::SameLine();
					ImGui::SameLine();*/
					
					ImGui::Checkbox(OBFUSCATE("ID объекта"), &cheatClient->Wallhack.Object.model_id);
					
					ImGui::Spacing();
					ImGui::Spacing();
					ImGui::SameLine();
    			}

    			// Закрытие контейнера вкладок
    			ImGui::EndTabBar();
			}
			if(wh_window_not_inited)
			{
				ImGui::SetWindowSize(ImVec2(720, 450));
				wh_window_not_inited = false;
			}
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
	
	if(cheatClient->Collision.menu)
	{
		ImGui::GetStyle().WindowTitleAlign.y = 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 15.f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(17,17,17,200));
		ImGui::Begin(OBFUSCATE("Колизии"), 0);
		{
			ImGui::SameLine();
			ImGui::SameLine();
			ImGui::Checkbox(OBFUSCATE("Игроки"), &cheatClient->Collision.ped);
			ImGui::Spacing();
			ImGui::Spacing();
			ImGui::SameLine();
			ImGui::Checkbox(OBFUSCATE("Транспорт"), &cheatClient->Collision.vehicle);
			ImGui::Spacing();
			ImGui::Spacing();
			ImGui::SameLine();
			/*ImGui::Checkbox("Объекты", &cheatClient->Collision.objects);
			ImGui::Spacing();
			ImGui::Spacing();
			ImGui::SameLine();*/
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
	
	// Wallhack
#ifdef __aarch64__
	FindCameraMatrix();  // warm the camera scan — ESP latches instantly on toggle
#endif
	if(cheatClient->Wallhack.Player.lines)
	{
#ifdef __aarch64__
		static int whDbg = 0, whPeds = 0, whNearI = -1, whNearOn = 0;
		static float whNearD = 1e30f;
		static Vec3 whNearP = {0,0,0};
#endif
		uintptr_t whLocalPed = cheatClient->FindLocalPlayer();
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t player = cheatClient->GetAtPlayerPool(i);
			if(player)
			{
				player = *(uintptr_t *)(player + PLAYER_PED_OFF);
				if(player > 0x100000)
				{
					Vec3 ped_coors = cheatClient->GetPos(player);
					Vec3 local_coors = cheatClient->GetPos(whLocalPed);

					ped_coors.z -= 0.85f;
					local_coors.z -= 0.8f;
					
					Vec3 local_2d_coors = {0,0,0};
					Vec3 ped_2d_coors = {0,0,0};
					
					if(WorldToScreen(&ped_coors, &ped_2d_coors))
					{
						if(ped_2d_coors.z >= 1.f)
						{
							ImGui::GetBackgroundDrawList()->AddLine(ImVec2(cheatClient->screen_width * 0.5f, 0.f), ImVec2(ped_2d_coors.x, ped_2d_coors.y), ImGui::ColorConvertFloat4ToU32(cheatClient->Wallhack.Player.Config.Lines.color), cheatClient->Wallhack.Player.Config.Lines.tickness);
						}
					}
#ifdef __aarch64__
					if(player != whLocalPed)
					{
						float dx = ped_coors.x - local_coors.x, dy = ped_coors.y - local_coors.y, dz = ped_coors.z - local_coors.z;
						float d = dx*dx + dy*dy + dz*dz;
						if(d < whNearD) { whNearD = d; whNearI = i; whNearP = ped_coors; whNearOn = (ped_2d_coors.x != 0.f || ped_2d_coors.y != 0.f); }
						whPeds++;
					}
#endif
				}
			}
		}
#ifdef __aarch64__
		if(++whDbg >= 180)
		{
			whDbg = 0;
			__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "WH peds=%d nearest slot=%d dist=%.1f pos=(%.1f,%.1f,%.1f) onscreen=%d local=(%.1f,%.1f,%.1f)",
				whPeds, whNearI, sqrtf(whNearD), whNearP.x, whNearP.y, whNearP.z, whNearOn,
				cheatClient->GetPos(cheatClient->FindLocalPlayer()).x,
				cheatClient->GetPos(cheatClient->FindLocalPlayer()).y,
				cheatClient->GetPos(cheatClient->FindLocalPlayer()).z);
			whPeds = 0; whNearD = 1e30f; whNearI = -1; whNearOn = 0;
		}
#endif
	}
	
	if(cheatClient->Wallhack.Player.box)
	{
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t player = cheatClient->GetAtPlayerPool(i);
			if(player)
			{
				player = *(uintptr_t *)(player + PLAYER_PED_OFF);
				if(player > 0x100000)
				{
					Vec3 ped_coors = cheatClient->GetPos(player);
					float rotate = *(float *)(player + 0x380);
#ifdef __aarch64__
					uintptr_t pm = cheatClient->GetMatrix(player);
					if(pm) rotate = atan2f(*(float *)(pm + 0x24), *(float *)(pm + 0x20));
#endif
					DrawBox(ped_coors, rotate, cheatClient->Wallhack.Player.Config.Box.color, cheatClient->Wallhack.Player.Config.Box.tickness);
				}
			}
		}
	}
	
	if(cheatClient->Wallhack.Player.skeleton)
	{
		if(!cheatClient->Wallhack.Player.Config.Skeleton.rendered)
		{
			for(int i = 0; i < cheatClient->Wallhack.Player.Config.Skeleton.buffer.size(); i++)
			{
				ImGui::GetBackgroundDrawList()->AddLine(cheatClient->Wallhack.Player.Config.Skeleton.buffer[i].start, cheatClient->Wallhack.Player.Config.Skeleton.buffer[i].end, ImGui::ColorConvertFloat4ToU32(cheatClient->Wallhack.Player.Config.Skeleton.color), cheatClient->Wallhack.Player.Config.Skeleton.tickness);
			}
			cheatClient->Wallhack.Player.Config.Skeleton.buffer.clear();
			cheatClient->Wallhack.Player.Config.Skeleton.rendered = true;
		}
	}
	
	if(cheatClient->Wallhack.Player.model_id)
	{
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t rpObj = cheatClient->GetAtPlayerPool(i);
			uintptr_t player = rpObj;
			if(player)
			{
				player = *(uintptr_t *)(player + PLAYER_PED_OFF);
				if(player > 0x100000)
				{
					static int skinDumped = 0;
					if(skinDumped < 3)
					{
						skinDumped++;
						// dump the NEAREST streamed ped + its rp so the model id can be
						// cross-checked with /dl — log carries the player slot id
						uintptr_t lp0 = cheatClient->FindLocalPlayer();
						Vec3 lpos = lp0 ? cheatClient->GetPos(lp0) : Vec3(0,0,0);
						float bd = 1e9f; int bi = -1; uintptr_t bped = 0, brp = 0;
						for(int j = 0; j < 1500; j++)
						{
							uintptr_t r = cheatClient->GetAtPlayerPool(j);
							if(!r) continue;
							uintptr_t pp = 0;
							if(!SafeRead(r + PLAYER_PED_OFF, &pp, 8) || !(pp & 0x00FFFFFFFFFFFFFFULL)) continue;
							Vec3 cp = cheatClient->GetPos(pp & 0x00FFFFFFFFFFFFFFULL);
							float dd = (cp.x-lpos.x)*(cp.x-lpos.x) + (cp.y-lpos.y)*(cp.y-lpos.y);
							if(dd < bd && dd > 0.01f) { bd = dd; bi = j; bped = pp & 0x00FFFFFFFFFFFFFFULL; brp = r; }
						}
						if(bped)
						{
							uint8_t hdr[0x300];
							if(SafeRead(bped, hdr, sizeof(hdr)))
							{
								for(int part = 0; part < 6; part++)
								{
									char dl[700]; int n = snprintf(dl, sizeof(dl), "SKIN slot=%d ped=%p %02x:", bi, (void*)bped, 0x10 + part*0x80);
									for(int off = 0x10 + part*0x80; off < 0x10 + part*0x80 + 0x80 && off < 0x300 && n < (int)sizeof(dl) - 10; off += 4)
										n += snprintf(dl + n, sizeof(dl) - n, " %x:%u", off, *(unsigned int*)(hdr + off));
									__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "%s", dl);
								}
							}
							uint8_t rh[0x200];
							if(SafeRead(brp & 0x00FFFFFFFFFFFFFFULL, rh, sizeof(rh)))
							{
								for(int part = 0; part < 4; part++)
								{
									char rl[700]; int rn = snprintf(rl, sizeof(rl), "RPSKIN slot=%d rp=%p %02x:", bi, (void*)brp, 0x10 + part*0x7C);
									for(int off = 0x10 + part*0x7C; off < 0x10 + part*0x7C + 0x7C && off < 0x200 && rn < (int)sizeof(rl) - 10; off += 4)
										rn += snprintf(rl + rn, sizeof(rl) - rn, " %x:%u", off, *(unsigned int*)(rh + off));
									__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "%s", rl);
								}
							}
						}
					}
					Vec3 ped_coors = cheatClient->GetPos(player);
					Vec3 ped_2d_coors = {0,0,0};
					unsigned int ped_skin_id;
#ifdef __aarch64__
					if(!SafeRead((player & 0x00FFFFFFFFFFFFFFULL) + 0x98, &ped_skin_id, 4)) ped_skin_id = 0;
#else
					ped_skin_id = *(unsigned short *)(player + 0x5A);
#endif
					
					if(WorldToScreen(&ped_coors, &ped_2d_coors))
					{
						if(ped_2d_coors.z >= 1.f)
						{
							char buff[32];
							sprintf(buff, OBFUSCATE("ID скина: %u"), ped_skin_id);

							ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    						TextWithOutline(draw_list, ImVec2(ped_2d_coors.x, ped_2d_coors.y), IM_COL32(255, 255, 255, 255), buff, 30.0f, IM_COL32(0, 0, 0, 255));
						}
					}
				}
			}
		}
	}

	if(cheatClient->Wallhack.Player.nametags)
	{
		uintptr_t ntLocal = cheatClient->FindLocalPlayer();
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t rp = cheatClient->GetAtPlayerPool(i);
			if(rp)
			{
				uintptr_t player = *(uintptr_t *)(rp + PLAYER_PED_OFF);
				if(player > 0x100000 && player != ntLocal)
				{
					int pid = i;
#ifdef __aarch64__
					{ int t = *(uint16_t *)(rp + 0x4A); if(t > 0 && t < 2000) pid = t; }
#endif
					const char* name = cheatClient->ServerPlayers[pid].nickname;
					if(!name[0]) name = cheatClient->ServerPlayers[i].nickname;

					Vec3 ped_coors = cheatClient->GetPos(player);
					ped_coors.z += 1.1f;
					Vec3 scr = {0,0,0};
					if(WorldToScreen(&ped_coors, &scr) && scr.z >= 1.f)
					{
						char buff[72];
						snprintf(buff, sizeof(buff), "%s [%d]", name[0] ? name : "?", pid);
						ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
						TextWithOutline(draw_list, ImVec2(scr.x - ImGui::CalcTextSize(buff).x * 0.5f, scr.y), IM_COL32(255,255,255,255), buff, 26.0f, IM_COL32(0,0,0,255));
					}
				}
			}
		}
	}

	if(cheatClient->Wallhack.Vehicle.lines)
	{
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t vehicle = cheatClient->GetAtVehiclePool(i);
			if(vehicle)
			{
				Vec3 vehicle_coors = cheatClient->GetPos(vehicle);
				Vec3 vehicle_screen_coors = {0,0,0};

				Vec3 local_coors = cheatClient->GetPos(cheatClient->FindLocalPlayer());
				Vec3 local_2d_coors = {0,0,0};
				local_coors.z -= 0.8f;
				
				if(WorldToScreen(&vehicle_coors, &vehicle_screen_coors))
				{
					if(vehicle_screen_coors.z >= 1.f)
					{
						if(WorldToScreen(&local_coors, &local_2d_coors))
						{
							if(local_2d_coors.z >= 1.f)
							{
								ImGui::GetBackgroundDrawList()->AddLine(ImVec2(local_2d_coors.x, local_2d_coors.y), ImVec2(vehicle_screen_coors.x, vehicle_screen_coors.y), ImGui::ColorConvertFloat4ToU32(cheatClient->Wallhack.Vehicle.Config.Lines.color), cheatClient->Wallhack.Vehicle.Config.Lines.tickness);
							}
						}
					}
				}
			}
		}
	}

	if(cheatClient->Wallhack.Vehicle.model_id)
	{
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t vehicle = cheatClient->GetAtVehiclePool(i);
			if(vehicle)
			{
				Vec3 vehicle_coors = cheatClient->GetPos(vehicle);
				Vec3 vehicle_2d_coors = {0,0,0};
				unsigned short vehicle_model_id = *(unsigned short *)(vehicle + 0x5A);
					
				if(WorldToScreen(&vehicle_coors, &vehicle_2d_coors))
				{
					if(vehicle_2d_coors.z >= 1.f)
					{
						char buff[32];
						sprintf(buff, OBFUSCATE("ID модели: %hu"), vehicle_model_id);
						ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    					TextWithOutline(draw_list, ImVec2(vehicle_2d_coors.x, vehicle_2d_coors.y), IM_COL32(255, 255, 255, 255), buff, 30.0f, IM_COL32(0, 0, 0, 255));
					}
				}
			}
		}
	}

	if(cheatClient->Wallhack.Object.lines)
	{
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t object = cheatClient->GetAtObjectPool(i);
			if(object)
			{
				Vec3 object_coors = cheatClient->GetPos(object);
				Vec3 object_screen_coors = {0,0,0};

				Vec3 local_coors = cheatClient->GetPos(cheatClient->FindLocalPlayer());
				Vec3 local_2d_coors = {0,0,0};
				local_coors.z -= 0.8f;
				
				if(WorldToScreen(&object_coors, &object_screen_coors))
				{
					if(object_screen_coors.z >= 1.f)
					{
						if(WorldToScreen(&local_coors, &local_2d_coors))
						{
							if(local_2d_coors.z >= 1.f)
							{
								ImGui::GetBackgroundDrawList()->AddLine(ImVec2(local_2d_coors.x, local_2d_coors.y), ImVec2(object_screen_coors.x, object_screen_coors.y), ImGui::ColorConvertFloat4ToU32(cheatClient->Wallhack.Object.Config.Lines.color), cheatClient->Wallhack.Object.Config.Lines.tickness);
							}
						}
					}
				}
			}
		}
	}

	if(cheatClient->Wallhack.Object.model_id)
	{
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t object = cheatClient->GetAtObjectPool(i);
			if(object)
			{
				Vec3 object_coors = cheatClient->GetPos(object);
				Vec3 object_2d_coors = {0,0,0};
				unsigned short object_model_id = *(unsigned short *)(object + 0x5A);
					
				if(WorldToScreen(&object_coors, &object_2d_coors))
				{
					if(object_2d_coors.z >= 1.f)
					{
						char buff[32];
						sprintf(buff, OBFUSCATE("ID объекта: %hu"), object_model_id);
						ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    					TextWithOutline(draw_list, ImVec2(object_2d_coors.x, object_2d_coors.y), IM_COL32(255, 255, 255, 255), buff, 30.0f, IM_COL32(0, 0, 0, 255));
					}
				}
			}
		}
	}
	
	/*for(int i = 0; i < 1500; i++)
	{
		uintptr_t player = cheatClient->GetAtPlayerPool(i);
		if(player)
		{
			player = *(uintptr_t *)(player + PLAYER_PED_OFF);
			if(player > 0x100000)
			{
				CPed _player = CPed(player);
				Vec3 ped_coors = cheatClient->GetPos(player);
				Vec3 scr_coors = {0,0,0};
				if(WorldToScreen(&ped_coors, &scr_coors))
				{
					if(scr_coors.z >= 1.f)
					{
						int collision = -1;
						if((*(int *)(player + 80) & 0x100) == 0)
						{
							collision = 0;
						} else { collision = 1; }
						char buff[32];
						sprintf(buff, "Collision: %d", collision);
						TextWithOutline(ImGui::GetBackgroundDrawList(), ImVec2(scr_coors.x, scr_coors.y), IM_COL32(255, 255, 255, 255), buff, 30.0f, IM_COL32(0, 0, 0, 255));
					}
				}
			}
		}
	}*/
	//////////////////
	
    // Rendering
    ImGui::EndFrame();
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	if(m_bNeedClearMousePos)
    {
        io.MousePos = ImVec2(-1, -1);
        m_bNeedClearMousePos = false;
    }

    return old_eglSwapBuffers(dpy, surface);
}

void SendChatText_hook(uintptr_t a0, char* a1);
void SendChatCommand_hook(uintptr_t a0, char* a1);
extern void (*old_SendChatTextHook)(uintptr_t, char*);
extern void (*old_SendChatCommandHook)(uintptr_t, char*);

void *hack_thread(void *) {
    sleep(5);
	
	if(cheatClient == nullptr) {
		cheatClient = new Cheat();
	}
	
    //Check if target lib is loaded
    do {
        sleep(1);
		libAddr = findLibrary(OBFUSCATE("libblackrussia-client.so"));
    } while (libAddr == 0);
	
	gameLibAddr = libAddr;
	LOGI("libAddr = %p", (void*)libAddr);
	{
		FILE* mf = fopen("/proc/self/maps","rt");
		char mb[1024];
		while(mf && fgets(mb,sizeof(mb),mf)) {
			if(strstr(mb,"blackrussia") || strstr(mb,"libEGL")) LOGI("MAP: %s", mb);
		}
		if(mf) fclose(mf);
	}

	cheatClient->SetLibAddr(libAddr);
	cheatClient->screen_width = 2214;
	cheatClient->screen_height = 1080;
	cheatClient->timer = new CTimer();
	cheatClient->LoadSettings();
	LOGI("settings loaded");

    // Hook eglSwapBuffers
    auto addr = (uintptr_t)dlsym(RTLD_NEXT, OBFUSCATE("eglSwapBuffers"));
    LOGI("hook eglSwapBuffers @ %p", (void*)addr);
    DobbyHook((void *)addr, (void *)hook_eglSwapBuffers, (void **)&old_eglSwapBuffers);
    LOGI("eglSwapBuffers hooked");
	
#ifdef __aarch64__
	AddChatMessage = (void(*)(char*,...))(libAddr + string2Offset(OBFUSCATE("0xBE4D40"))); // Chat::AddClientMessage dispatcher arm64 (called by RPC360 handler)
	SendChatMessage = (void(*)(char*))(libAddr + string2Offset(OBFUSCATE("0x15D9A7C"))); // RPC 414 arm64 verified
	SendChatCommand = (void(*)(char*))(libAddr + string2Offset(OBFUSCATE("0x15D9990"))); // RPC 378 arm64 verified
	// arm64 chat-command intercept: GUI sends user input through these two
	DobbyHook((void *)(libAddr + string2Offset(OBFUSCATE("0x15D9A7C"))), (void *)SendChatText_hook, (void **)&old_SendChatTextHook);
	DobbyHook((void *)(libAddr + string2Offset(OBFUSCATE("0x15D9990"))), (void *)SendChatCommand_hook, (void **)&old_SendChatCommandHook);
	FindGroundZ = (float(*)(float,float))(libAddr + string2Offset(OBFUSCATE("0xBB0B64"))); // FindGroundZ arm64 via RPC416 handler
	CPlayerPool_GetAt = (int(*)(int, unsigned short))(libAddr + string2Offset(OBFUSCATE("0x15EE404"))); // verified arm64
	CVehiclePool_GetAt = (int(*)(int , unsigned short))([](int, unsigned short) -> int { return 0; }); // TODO remap arm64
	GetBonePosition = (int(*)(int,int))([](int, int) -> int { return 0; }); // TODO remap arm64
	SetMoveAnim = (void(*)(int))([](int) {}); // TODO remap arm64
	CVehiclePool_New = (void(*)(int,unsigned short*))([](int, unsigned short*) {}); // TODO remap arm64
#else
	AddChatMessage = (void(*)(char*,...))(libAddr + string2Offset(OBFUSCATE("0x5DB530")) + 1); // Chat::AddClientMessage verified
	SendChatMessage = (void(*)(char*))(libAddr + string2Offset(OBFUSCATE("0xDA2A61"))); // RPC 414 verified
	SendChatCommand = (void(*)(char*))(libAddr + string2Offset(OBFUSCATE("0xDA29C9"))); // RPC 378 verified
	FindGroundZ = (float(*)(float,float))(libAddr + string2Offset(OBFUSCATE("0x5B1339"))); // CWorld::FindGroundZForCoord via RPC416 handler
	CPlayerPool_GetAt = (int(*)(int, unsigned short))(libAddr + string2Offset(OBFUSCATE("0xDB323B"))); // verified via ServerJoin/RequestSpawn
	CVehiclePool_GetAt = (int(*)(int , unsigned short))(libAddr + string2Offset(OBFUSCATE("0x374379"))); // TODO remap
	GetBonePosition = (int(*)(int,int))(libAddr + string2Offset(OBFUSCATE("0x3AC93B"))); // TODO remap
	////SetSeek = (void(*)(int,float,float,float,float))(libAddr + 0x1DED50 + 1);
	SetMoveAnim = (void(*)(int))(libAddr + string2Offset(OBFUSCATE("0x37DA35")));
	CVehiclePool_New = (void(*)(int,unsigned short*))(libAddr + string2Offset(OBFUSCATE("0x374389")));
#endif

#ifdef __aarch64__
	LOGI("hook multiTouch @ %p", (void*)(libAddr + string2Offset(OBFUSCATE("0x17455C8"))));
	DobbyHook((void *)(libAddr + string2Offset(OBFUSCATE("0x17455C8"))), (void *)Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent, (void **)&old_Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent); // verified JNI export arm64
	LOGI("multiTouch hooked");
	DobbyHook((void *)(libAddr + 0x18D531C), (void *)RakClient_Send_hook, (void **)&orig_RakClient_Send); // RakClient::Send arm64
	DobbyHook((void *)(libAddr + 0x18D53F8), (void *)RakClient_SendBS_hook, (void **)&orig_RakClient_SendBS); // RakClient::Send(BitStream) arm64
	DobbyHook((void *)(libAddr + 0x18D9414), (void *)RakPeer_Send_hook, (void **)&orig_RakPeer_Send); // RakPeer::Send arm64 — real chokepoint
	DobbyHook((void *)(libAddr + 0x18D9684), (void *)RakPeer_SendBS_hook, (void **)&orig_RakPeer_SendBS); // RakPeer::Send(BitStream) arm64
	DobbyHook((void *)(libAddr + 0x18D6A1C), (void *)RakClient_ProcessNetworkPacket_hook, (void **)&orig_RakClient_ProcessNetworkPacket); // RakClient::ProcessNetworkPacket arm64
	LOGI("raknet hooked");
#else
	DobbyHook((void *)(libAddr + string2Offset(OBFUSCATE("0xECCD54")) + 1), (void *)Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent, (void **)&old_Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent); // verified JNI export
#endif
    //MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x3CC6BD"))), (void *)ChatWindowInputHandler, (void **)&old_ChatWindowInputHandler); // TODO remap
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x2C9F19"))), (void *)CGame_Process, (void **)&old_CGame_Process); // TODO remap CGame::Process/Idle
#ifdef __aarch64__
	DobbyHook((void*)(libAddr + string2Offset(OBFUSCATE("0x1745098"))), (void *)JNILib_step_hook, (void **)&old_JNILib_step); // JNILib::step — per-frame hook arm64
	LOGI("step hooked");
#else
	MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0xED47FD"))), (void *)CGame_Process, (void **)&old_CGame_Process); // SUB: Input_Process — called every frame in game state 6
#endif
	//MSHookFunction((void*)(libAddr + 0x1BD7D0 + 1), (void *)SetVehicleVelocityRPC, (void **)&old_SetVehicleVelocityRPC);*/
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x2EFC25"))), (void *)GetIsLineOfSightClear, (void **)&old_GetIsLineOfSightClear); // TODO remap — DISABLED
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x32301D"))), (void *)CPlayerBubbleChatPool_New, (void **)&old_CPlayerBubbleChatPool_New);
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x3B0B45"))), (void *)CSprintCalculator, (void **)&old_CSprintCalculator); // TODO remap
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x3A8CF9"))), (void *)CPedDamageCalculator, (void **)&old_CPedDamageCalculator); // TODO remap
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x4610B5"))), (void *)CVehicleDamageCalculator, (void **)&old_CVehicleDamageCalculator); // TODO remap
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x44F511"))), (void *)NoBikeCalculator, (void **)&old_NoBikeCalculator); // TODO remap
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x380D5D"))), (void *)CPed_ProcessBuoyancy, (void **)&old_CPed_ProcessBuoyancy); // TODO remap
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x44C4F1"))), (void *)CBike_ProcessBuoyancy, (void **)&old_CBike_ProcessBuoyancy); // TODO remap
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x43C9A1"))), (void *)CAutomobile_ProcessBuoyancy, (void **)&old_CAutomobile_ProcessBuoyancy); // TODO remap
	//MSHookFunction((void*)(libAddr + 0x203FF0 + 1), (void *)log_1, (void **)&old_log1); // cvehiclepool_new
	//MSHookFunction((void*)(libAddr + 0x28D808 + 1), (void *)log_2, (void **)&old_log2);
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x482BF5"))), (void *)log_3, (void **)&old_log3); // TODO remap
	//MSHookFunction((void*)(libAddr + 0x1B3910 + 1), (void *)log_4, (void **)&old_log4);
#ifdef __aarch64__
	// RPC hooks DISABLED on arm64: handlers now take RPCParameters* (BitStream),
	// old signatures (pool, nickname, playerID) crash inside strncpy. TODO rewrite bodies.
	//DobbyHook((void*)(libAddr + string2Offset(OBFUSCATE("0x15F0F68"))), (void *)ScrServerJoin, (void **)&old_ScrServerJoin); // RPC401 ServerJoin arm64
	//DobbyHook((void*)(libAddr + string2Offset(OBFUSCATE("0x15F114C"))), (void *)ScrServerQuit, (void **)&old_ScrServerQuit); // RPC326 ServerQuit arm64
	//DobbyHook((void*)(libAddr + string2Offset(OBFUSCATE("0x15F13FC"))), (void *)ScrServerNewPlayer, (void **)&old_ScrServerNewPlayer); // RPC419 WorldPlayerAdd arm64
#else
	MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0xDB5339"))), (void *)ScrServerJoin, (void **)&old_ScrServerJoin); // RPC401 idx91 verified
	MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0xDB546D"))), (void *)ScrServerQuit, (void **)&old_ScrServerQuit); // RPC326 idx74 verified
	////MSHookFunction((void*)(libAddr + 0x1FA270 + 1), (void *)ScrConnectionRejected, (void **)&old_ScrConnectionRejected); // candidate 0xDB5C30
	MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0xDB5661"))), (void *)ScrServerNewPlayer, (void **)&old_ScrServerNewPlayer); // RPC419 WorldPlayerAdd verified
#endif
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x1F9329"))), (void *)ScrServerDeletePlayer, (void **)&old_ScrServerDeletePlayer); // candidate 0xDB59D0 WorldPlayerRemove
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x"))), (void *)WorldVehicleDelete, (void **)&old_WorldVehicleDelete);
#ifdef __aarch64__
	//DobbyHook((void*)(libAddr + string2Offset(OBFUSCATE("0x15F0838"))), (void *)ScrInitGame, (void **)&old_ScrInitGame); // RPC421 InitGame arm64 — disabled, RPCParameters* signature
#else
	MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0xDB4DDD"))), (void *)ScrInitGame, (void **)&old_ScrInitGame); // RPC421 InitGame verified
#endif
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x36979D"))), (void *)ScrRequestSpawn, (void **)&old_ScrRequestSpawn);
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x"))), (void *)ScrSetSpawnInfo, (void **)&old_ScrSetSpawnInfo);
	//MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x"))), (void *)LocalSetSpawnInfo, (void **)&old_LocalSetSpawnInfo);
#ifdef __aarch64__
	//DobbyHook((void*)(libAddr + string2Offset(OBFUSCATE("0xBE4D40"))), (void *)AddChatMessage_hook, (void **)&old_AddChatMessage); // arm64 signature unverified (color,msg)? disabled
	// arm64: any unhooked old_* stays null -> wrappers call into address 0. Point them at a stub instead.
	{
		void (*oneshot_oldstub)() = (void(*)())+[]{ return 0; };
#define OS_STUB(p) if (!(p)) { p = (decltype(p))oneshot_oldstub; }
		OS_STUB(old_CGame_Process);
		OS_STUB(old_GetIsLineOfSightClear);
		OS_STUB(old_CSprintCalculator);
		OS_STUB(old_CPedDamageCalculator);
		OS_STUB(old_CVehicleDamageCalculator);
		OS_STUB(old_NoBikeCalculator);
		OS_STUB(old_CPed_ProcessBuoyancy);
		OS_STUB(old_CBike_ProcessBuoyancy);
		OS_STUB(old_CAutomobile_ProcessBuoyancy);
		OS_STUB(old_CDraw_SetFOV);
		OS_STUB(old_ChatWindowInputHandler);
		OS_STUB(old_SetVehicleVelocityRPC);
		OS_STUB(old_CPlayerBubbleChatPool_New);
		OS_STUB(old_ScrServerJoin);
		OS_STUB(old_ScrServerQuit);
		OS_STUB(old_ScrServerDeletePlayer);
		OS_STUB(old_ScrConnectionRejected);
		OS_STUB(old_ScrServerNewPlayer);
		OS_STUB(old_WorldVehicleDelete);
		OS_STUB(old_ScrInitGame);
		OS_STUB(old_ScrRequestSpawn);
		OS_STUB(old_ScrSetSpawnInfo);
		OS_STUB(old_LocalSetSpawnInfo);
		OS_STUB(old_AddChatMessage);
		OS_STUB(old_log1);
		OS_STUB(old_log2);
		OS_STUB(old_log3);
		OS_STUB(old_log4);
#undef OS_STUB
	}
	LOGI("all arm64 hooks done");
#else
	MSHookFunction((void*)(libAddr + string2Offset(OBFUSCATE("0x5DB531"))), (void *)AddChatMessage_hook, (void **)&old_AddChatMessage); // Chat::AddClientMessage verified
#endif
	
    pthread_exit(nullptr);
    return nullptr;
}

void log_4(int a1, int a2, int a3, float a4, float a5, float a6)
{
	//AddChatMessage("a1: %d, a2: %d, a3: %d, a4: %f, a5: %f, a6: %f", a1,a2,a3,a4,a5,a6);
	old_log4(a1,a2,a3,a4,a5,a6);
}

extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *globalEnv;
    vm->GetEnv((void **) &globalEnv, JNI_VERSION_1_6);
    g_JavaVM = vm;
	pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
    return JNI_VERSION_1_6;
}

static int   gJoyPointer = -1;
static float gJoyOX = 0.f, gJoyOY = 0.f;
float gJoyX = 0.f, gJoyY = 0.f; // joystick deflection [-1,1] for fly control

static float MenuTouchX(int pointerId, int x1, int x2, int x3)
{
	return pointerId == 1 ? x2 : (pointerId == 2 ? x3 : x1);
}
static float MenuTouchY(int pointerId, int y1, int y2, int y3)
{
	return pointerId == 1 ? y2 : (pointerId == 2 ? y3 : y1);
}

extern "C"

JNIEXPORT void JNICALL Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent(JNIEnv* env, jclass cls, int actionMasked, int pointerId, int x1, int y1, int x2, int y2, int x3, int y3)
{
	const int ACTION_DOWN = 0;
	const int ACTION_POINTER_DOWN = 5;
	const int ACTION_UP = 1;
	const int ACTION_POINTER_UP = 6;
	const int ACTION_MOVE_UNK = 7;
	const int ACTION_MOVE = 2;

	ImGuiIO& io = ImGui::GetIO();

	// Menu is modal: while it (or any cheat window) is open, feed every touch to
	// ImGui and never forward to the game. No per-window bounds maze.
	bool cheatUiActive = menuOpen
		|| cheatClient->Wallhack.menu
		|| cheatClient->VehicleCheat.CarChanger.menu
		|| cheatClient->GameSpeed.menu
		|| cheatClient->Teleport.toPeds;
	if(cheatUiActive)
	{
		float tx = MenuTouchX(pointerId, x1, x2, x3);
		float ty = MenuTouchY(pointerId, y1, y2, y3);
		if(actionMasked == ACTION_MOVE || actionMasked == ACTION_MOVE_UNK
			|| actionMasked == ACTION_DOWN || actionMasked == ACTION_POINTER_DOWN)
		{
			io.MousePos = ImVec2(tx, ty);
			io.MouseDown[0] = true;
		}
		if(actionMasked == ACTION_UP || actionMasked == ACTION_POINTER_UP)
		{
			io.MousePos = ImVec2(tx, ty);
			io.MouseDown[0] = false;
			m_bNeedClearMousePos = true;
		}
		return;
	}

	// Joystick tracking for fly: left-half touches form a stick vector
	// (origin = touch-down point, deflection = current - origin)
	{
		float tx = MenuTouchX(pointerId, x1, x2, x3);
		float ty = MenuTouchY(pointerId, y1, y2, y3);
		float joyZoneX = cheatClient->screen_width * 0.42f;
		if(actionMasked == ACTION_DOWN || actionMasked == ACTION_POINTER_DOWN)
		{
			if(tx < joyZoneX && gJoyPointer < 0) {
				gJoyPointer = pointerId; gJoyOX = tx; gJoyOY = ty;
			}
		}
		else if(actionMasked == ACTION_MOVE || actionMasked == ACTION_MOVE_UNK)
		{
			if(pointerId == gJoyPointer) {
				gJoyX = (tx - gJoyOX) / 120.f;
				gJoyY = (ty - gJoyOY) / 120.f;
				float m = sqrtf(gJoyX * gJoyX + gJoyY * gJoyY);
				if(m > 1.f) { gJoyX /= m; gJoyY /= m; }
			}
		}
		else if(actionMasked == ACTION_UP || actionMasked == ACTION_POINTER_UP)
		{
			if(pointerId == gJoyPointer) { gJoyPointer = -1; gJoyX = gJoyY = 0.f; }
		}
	}

	ImGuiWindow* icon_window = ImGui::FindWindowByName(OBFUSCATE("mod-icon"));
	ImGuiWindow* menu_window = ImGui::FindWindowByName(OBFUSCATE("OneShot"));
	ImGuiWindow* wallhack_window = ImGui::FindWindowByName(OBFUSCATE("Wallhack"));
	ImGuiWindow* change_car_model_window = ImGui::FindWindowByName(OBFUSCATE("Подмена т/с"));
	ImGuiWindow* game_speed_window = ImGui::FindWindowByName(OBFUSCATE("Скорость игры"));
	ImGuiWindow* tp_toPeds_window = ImGui::FindWindowByName(OBFUSCATE("ТП к ближайшим игрокам"));
	
	if(actionMasked == ACTION_DOWN || actionMasked == ACTION_POINTER_DOWN)
	{
		// ImGui
		
		if(icon_window != nullptr && !hideIcon)
		{
			ImVec2 icon_window_pos = icon_window->Pos;
			ImVec2 icon_window_size = icon_window->Size;
				
			if(x1 > icon_window_pos.x && x1 < icon_window_pos.x + icon_window_size.x
			&& y1 > icon_window_pos.y && y1 < icon_window_pos.y + icon_window_size.y)
			{
				icon_is_hovered = true;
				start_touch_x = x1;
				start_touch_y = y1;
				io.MousePos = ImVec2(x1, y1);
            	io.MouseDown[0] = true;
				return;
			}
			else if(x2 > icon_window_pos.x && x2 < icon_window_pos.x + icon_window_size.x
			&& y2 > icon_window_pos.y && y2 < icon_window_pos.y + icon_window_size.y)
			{
				icon_is_hovered = true;
				start_touch_x = x2;
				start_touch_y = y2;
				io.MousePos = ImVec2(x2, y2);
            	io.MouseDown[0] = true;
				return;
			}
			else if(x3 > icon_window_pos.x && x3 < icon_window_pos.x + icon_window_size.x
			&& y3 > icon_window_pos.y && y3 < icon_window_pos.y + icon_window_size.y)
			{
				icon_is_hovered = true;
				start_touch_x = x3;
				start_touch_y = y3;
				io.MousePos = ImVec2(x3, y3);
            	io.MouseDown[0] = true;
				return;
			}
		}
			
		if(menu_window != nullptr && menuOpen)
		{
			ImVec2 menu_window_pos = menu_window->Pos;
			ImVec2 menu_window_size = menu_window->Size;
				
			if(x1 > menu_window_pos.x && x1 < menu_window_pos.x + menu_window_size.x
			&& y1 > menu_window_pos.y && y1 < menu_window_pos.y + menu_window_size.y)
			{
				menu_is_hovered = true;
				io.MousePos = ImVec2(x1, y1);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x2 > menu_window_pos.x && x2 < menu_window_pos.x + menu_window_size.x
			&& y2 > menu_window_pos.y && y2 < menu_window_pos.y + menu_window_size.y)
			{
				menu_is_hovered = true;
				io.MousePos = ImVec2(x2, y2);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x3 > menu_window_pos.x && x3 < menu_window_pos.x + menu_window_size.x
			&& y3 > menu_window_pos.y && y3 < menu_window_pos.y + menu_window_size.y)
			{
				menu_is_hovered = true;
				io.MousePos = ImVec2(x3, y3);
           		io.MouseDown[0] = true;
				return;
			}
		}
		
		if(cheatClient->Wallhack.menu && wallhack_window != nullptr)
		{
			ImVec2 menu_window_pos = wallhack_window->Pos;
			ImVec2 menu_window_size = wallhack_window->Size;
				
			if(x1 > menu_window_pos.x && x1 < menu_window_pos.x + menu_window_size.x
			&& y1 > menu_window_pos.y && y1 < menu_window_pos.y + menu_window_size.y)
			{
				wallhack_menu_is_hovered = true;
				io.MousePos = ImVec2(x1, y1);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x2 > menu_window_pos.x && x2 < menu_window_pos.x + menu_window_size.x
			&& y2 > menu_window_pos.y && y2 < menu_window_pos.y + menu_window_size.y)
			{
				wallhack_menu_is_hovered = true;
				io.MousePos = ImVec2(x2, y2);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x3 > menu_window_pos.x && x3 < menu_window_pos.x + menu_window_size.x
			&& y3 > menu_window_pos.y && y3 < menu_window_pos.y + menu_window_size.y)
			{
				wallhack_menu_is_hovered = true;
				io.MousePos = ImVec2(x3, y3);
           		io.MouseDown[0] = true;
				return;
			}
		}
		
		if(cheatClient->GameSpeed.menu && game_speed_window != nullptr)
		{
			ImVec2 menu_window_pos = game_speed_window->Pos;
			ImVec2 menu_window_size = game_speed_window->Size;
				
			if(x1 > menu_window_pos.x && x1 < menu_window_pos.x + menu_window_size.x
			&& y1 > menu_window_pos.y && y1 < menu_window_pos.y + menu_window_size.y)
			{
				game_speed_menu_is_hovered = true;
				io.MousePos = ImVec2(x1, y1);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x2 > menu_window_pos.x && x2 < menu_window_pos.x + menu_window_size.x
			&& y2 > menu_window_pos.y && y2 < menu_window_pos.y + menu_window_size.y)
			{
				game_speed_menu_is_hovered = true;
				io.MousePos = ImVec2(x2, y2);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x3 > menu_window_pos.x && x3 < menu_window_pos.x + menu_window_size.x
			&& y3 > menu_window_pos.y && y3 < menu_window_pos.y + menu_window_size.y)
			{
				game_speed_menu_is_hovered = true;
				io.MousePos = ImVec2(x3, y3);
           		io.MouseDown[0] = true;
				return;
			}
		}
		
		if(cheatClient->VehicleCheat.CarChanger.menu && change_car_model_window != nullptr)
		{
			ImVec2 menu_window_pos = change_car_model_window->Pos;
			ImVec2 menu_window_size = change_car_model_window->Size;
				
			if(x1 > menu_window_pos.x && x1 < menu_window_pos.x + menu_window_size.x
			&& y1 > menu_window_pos.y && y1 < menu_window_pos.y + menu_window_size.y)
			{
				change_car_model_menu_is_hovered = true;
				io.MousePos = ImVec2(x1, y1);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x2 > menu_window_pos.x && x2 < menu_window_pos.x + menu_window_size.x
			&& y2 > menu_window_pos.y && y2 < menu_window_pos.y + menu_window_size.y)
			{
				change_car_model_menu_is_hovered = true;
				io.MousePos = ImVec2(x2, y2);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x3 > menu_window_pos.x && x3 < menu_window_pos.x + menu_window_size.x
			&& y3 > menu_window_pos.y && y3 < menu_window_pos.y + menu_window_size.y)
			{
				change_car_model_menu_is_hovered = true;
				io.MousePos = ImVec2(x3, y3);
           		io.MouseDown[0] = true;
				return;
			}
		}
		
		if(cheatClient->Teleport.toPeds && tp_toPeds_window != nullptr)
		{
			ImVec2 menu_window_pos = tp_toPeds_window->Pos;
			ImVec2 menu_window_size = tp_toPeds_window->Size;
				
			if(x1 > menu_window_pos.x && x1 < menu_window_pos.x + menu_window_size.x
			&& y1 > menu_window_pos.y && y1 < menu_window_pos.y + menu_window_size.y)
			{
				tp_toPeds_menu_is_hovered = true;
				io.MousePos = ImVec2(x1, y1);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x2 > menu_window_pos.x && x2 < menu_window_pos.x + menu_window_size.x
			&& y2 > menu_window_pos.y && y2 < menu_window_pos.y + menu_window_size.y)
			{
				tp_toPeds_menu_is_hovered = true;
				io.MousePos = ImVec2(x2, y2);
           		io.MouseDown[0] = true;
				return;
			}
			else if(x3 > menu_window_pos.x && x3 < menu_window_pos.x + menu_window_size.x
			&& y3 > menu_window_pos.y && y3 < menu_window_pos.y + menu_window_size.y)
			{
				tp_toPeds_menu_is_hovered = true;
				io.MousePos = ImVec2(x3, y3);
           		io.MouseDown[0] = true;
				return;
			}
		}
		
		return old_Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent(env,cls,actionMasked,pointerId,x1,y1,x2,y2,x3,y3);
			
		// Other
		// ...
	}
	
	if(actionMasked == ACTION_MOVE || actionMasked == ACTION_MOVE_UNK)
	{
		// ImGui
		if(icon_is_hovered || menu_is_hovered || wallhack_menu_is_hovered || game_speed_menu_is_hovered || change_car_model_menu_is_hovered
		|| tp_toPeds_menu_is_hovered)
		{
			if(pointerId == 0) {
				io.MousePos = ImVec2(x1, y1);
			} else if (pointerId == 1) {
				io.MousePos = ImVec2(x2, y2);
			} else if (pointerId == 2) {
				io.MousePos = ImVec2(x3, y3);
			}
			return;
		}
			
		return old_Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent(env,cls,actionMasked,pointerId,x1,y1,x2,y2,x3,y3);

		// Other
		// ...
	}
	if(actionMasked == ACTION_UP || actionMasked == ACTION_POINTER_UP)
	{
		// ImGui
		io.MouseDown[0] = false;
        m_bNeedClearMousePos = true;
		
		int rawX = 0;
		int rawY = 0;
		
		if(icon_is_hovered)
		{
			if(pointerId == 0) {
				rawX = x1 - start_touch_x;
				rawY = y1 - start_touch_y;
			} else if (pointerId == 1) {
				rawX = x2 - start_touch_x;
				rawY = y2 - start_touch_y;
			} else if (pointerId == 2) {
				rawX = x3 - start_touch_x;
				rawY = y3 - start_touch_y;
			}
		
			if (rawX < 10 && rawY < 10)
			{
				menuOpen =! menuOpen;
			}
		}
		
		icon_is_hovered = false;
		menu_is_hovered = false;
		wallhack_menu_is_hovered = false;
		game_speed_menu_is_hovered = false;
		change_car_model_menu_is_hovered = false;
		tp_toPeds_menu_is_hovered = false;
			
		start_touch_x = 0;
		start_touch_y = 0;
			
		// Other
		// ...
		return old_Java_com_blackhub_bronline_game_core_JNILib_multiTouchEvent(env,cls,actionMasked,pointerId,x1,y1,x2,y2,x3,y3);
	}
}

extern "C"
JNIEXPORT void JNICALL
Java_com_oneshot_br_Touch_sendTouch(JNIEnv* env, jobject obj, jint type, jint x, jint y)
{
}

extern "C"
JNIEXPORT void JNICALL
Java_wrapper_Wrapper_next(JNIEnv* env, jobject obj, jobject mActivity)
{
	BrNotificationUpdate(env);
}

extern "C"
JNIEXPORT void JNICALL
Java_wrapper_Wrapper_initbaby(JNIEnv* env, jclass cls, jobject context)
{
	startService(env, context);
}

// Загрузка изображения с использованием stb_image.h
GLuint LoadTexture(const char* path) {
    int width, height, channels;
    unsigned char* image_data = stbi_load(path, &width, &height, &channels, 0);
    if (!image_data) {
        // Обработка ошибки загрузки изображения
        return 0;
    }

    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(image_data);

    return texture_id;
}

void AddChatMessage_hook(char* msg)
{
	/*if(cheatClient->trigger_gevent && strstr(msg, "/gevent"))
	{
		SendChatCommand("/gevent");
	}*/
	
	if(cheatClient->VehicleCheat.CarChanger.entered)
	{
		return;
	}
	
	old_AddChatMessage(msg);
}

// returns true if the input was consumed by the cheat
static bool InterceptCheatInput(char* input)
{
	if(!input) { return false; }
	if(!strcmp(input, OBFUSCATE("/oneshot")))
	{
		menuOpen =! menuOpen;
		return true;
	}

	if(!strncmp(input, OBFUSCATE("/slap"), 5))
	{
		int targetId = -1;
		uintptr_t ped = 0;
		if(sscanf(input + 5, "%d", &targetId) != 1)
		{
			// /slap <name>: scan pool slots, match name at +0x18
			const char *want = input + 5;
			while(*want == ' ') want++;
			if(!*want) { ChatMsg((char *)"[slap] /slap <id|nick>"); return true; }
			for(int i = 0; i < 600 && !ped; i++)
			{
				uintptr_t p = cheatClient->GetAtPlayerPool(i);
				// real pool objects live in the game heap range; OOB slots
				// return garbage indices (crash if derefed)
				if(p < 0xb400000000000000ULL || p > 0xb500000000000000ULL ||
					strncasecmp((char *)(p + 0x18), want, strlen(want)) != 0)
					continue;
				// find ped: first heap-pointer qword in +0x8..+0x1F8
				for(int off = 0x8; off < 0x200; off += 8)
				{
					uintptr_t q = *(uintptr_t *)(p + off);
					if(q >= 0xb400000000000000ULL && q <= 0xb500000000000000ULL)
					{
						__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
							"SLAP name=%s slot=%d pedcand=+0x%x q=%p", want, i, off, (void *)q);
						ped = q;
						targetId = i;
						slapPedOff = off;
						break;
					}
				}
			}
			if(ped > 0x100000)
			{
				slapTargetId = targetId;
				slapTicks = 120;
				SendBulletHit(targetId, ped);
				ChatMsg((char *)"[slap] ok");
			}
			else
				ChatMsg((char *)"[slap] name not found");
			return true;
		}
		if(targetId >= 0 && targetId < 1500)
		{
			uintptr_t rp = cheatClient->GetAtPlayerPool(targetId);
			if(rp >= 0xb400000000000000ULL && rp <= 0xb500000000000000ULL)
			{
				for(int off = 0x8; off < 0x200 && !ped; off += 8)
				{
					uintptr_t q = *(uintptr_t *)(rp + off);
					if(q >= 0xb400000000000000ULL && q <= 0xb500000000000000ULL)
					{
						ped = q;
						slapPedOff = off;
					}
				}
			}
			if(ped > 0x100000)
			{
				// remote slap: remote ped pos is re-synced every tick, so a
				// one-shot write is invisible — pin it every frame for ~2s
				slapTargetId = targetId;
				slapTicks = 120;
				SendBulletHit(targetId, ped);
				char slapmsg[64];
				snprintf(slapmsg, sizeof(slapmsg), "[slap] id=%d ok", targetId);
				ChatMsg(slapmsg);
			}
			else
			{
				char slapmsg[128];
				snprintf(slapmsg, sizeof(slapmsg), "[slap] id=%d rp=%p not found", targetId, (void *)rp);
				ChatMsg(slapmsg);
				// list streamed players (ped ptr nonzero): slot:name for /slap <slot>
				char ids[128] = "[slap] spawned: ";
				int n = 0;
				for(int i = 0; i < 2000 && n < 10; i++)
				{
					uintptr_t p = cheatClient->GetAtPlayerPool(i);
					if(p > 0x100000 && *(uintptr_t *)(p + 0x8) > 0x100000)
					{
						char tmp[40];
						snprintf(tmp, sizeof(tmp), "%d:%.12s ", i, (char *)(p + 0x18));
						strncat(ids, tmp, sizeof(ids) - strlen(ids) - 1);
						n++;
					}
				}
				if(!n) strncat(ids, "none", sizeof(ids) - strlen(ids) - 1);
				ChatMsg(ids);
			}
			return true;
		}
		ped = cheatClient->FindLocalPlayer();
		if(ped)
		{
			*(float *)(ped + ENT_POS_Z) += cheatClient->PlayerCheat.Base.Slap.up;
			*(float *)(ped + 0x2A8) += cheatClient->PlayerCheat.Base.Slap.up;
		}
		return true;
	}

	if(!strncmp(input, OBFUSCATE("/down"), 5))
	{
		uintptr_t ped = cheatClient->FindLocalPlayer();
		if(ped)
		{
			*(float *)(ped + ENT_POS_Z) -= cheatClient->PlayerCheat.Base.Slap.down;
			*(float *)(ped + 0x2A8) -= cheatClient->PlayerCheat.Base.Slap.down;
		}
		return true;
	}

	if(cheatClient->Flooder.wait_text_input)
	{
		cheatClient->Flooder.wait_text_input = false;
		cheatClient->Flooder.message = input;
		cheatClient->Flooder.menu = true;
		return true;
	}

	if(cheatClient->Flooder.wait_delay_input)
	{
		cheatClient->Flooder.wait_delay_input = false;
		cheatClient->Flooder.delay_after_message = std::stoi(input);
		cheatClient->Flooder.menu = true;
		return true;
	}

	return false;
}

void ChatWindowInputHandler(char* input)
{
	if(!InterceptCheatInput(input))
	{
		old_ChatWindowInputHandler(input);
	}
}

// arm64: user-typed chat goes through CNetGame::SendChatText / SendChatCommand —
// hooking them intercepts commands without touching RPC parsing.
void (*old_SendChatTextHook)(uintptr_t, char*);
void (*old_SendChatCommandHook)(uintptr_t, char*);
static char* chatMsgArg(uintptr_t a0, uintptr_t a1, const char* fn)
{
	// callee may be (char* msg) or (self, char* msg) — a0 is always safe to
	// peek (a live object or the string); if a0 starts with '/' or printable
	// text it's the single-arg form, otherwise the msg is a1.
	char* m = (*(char *)a0 == '/' || isprint(*(char *)a0)) ? (char *)a0 : (char *)a1;
	static bool logged = false;
	if(!logged) { logged = true; LOGI("CHATHOOK %s a0=%p a1=%p picked=%p first='%c'", fn, (void *)a0, (void *)a1, (void *)m, m > (char *)0x100000 ? *m : '?'); }
	return m;
}
void SendChatText_hook(uintptr_t a0, char* a1)
{
	char* msg = chatMsgArg(a0, (uintptr_t)a1, "SendChatText");
	if((uintptr_t)msg < 0x100000 || !InterceptCheatInput(msg))
	{
		old_SendChatTextHook(a0, a1);
	}
}
void SendChatCommand_hook(uintptr_t a0, char* a1)
{
	char* msg = chatMsgArg(a0, (uintptr_t)a1, "SendChatCommand");
	if((uintptr_t)msg < 0x100000 || !InterceptCheatInput(msg))
	{
		old_SendChatCommandHook(a0, a1);
	}
}

void CDraw_SetFOV(float val)
{
	old_CDraw_SetFOV(val);
}

void CheatFrameBody()
{
	cheatClient->timer->Update();
	
	// settings autosave: on menu close + every ~10s
	static bool lastMenuOpen = menuOpen;
	static int saveCounter = 0;
	if(lastMenuOpen && !menuOpen)
	{
		cheatClient->SaveSettings();
		// close sub-windows too — a left-open window keeps cheatUiActive
		// true and swallows every touch, blocking game controls
		cheatClient->Wallhack.menu = false;
		cheatClient->VehicleCheat.CarChanger.menu = false;
		cheatClient->GameSpeed.menu = false;
		cheatClient->Teleport.toPeds = false;
		cheatClient->CarSpawn.menu = false;
		cheatClient->Collision.menu = false;
		cheatClient->FakeChat.menu = false;
		cheatClient->Flooder.menu = false;
		cheatClient->Tuning.menu = false;
		cheatClient->VisualCheat.MoneyChanger.menu = false;
		cheatClient->VisualCheat.SkinChanger.menu = false;
		cheatClient->AutoFarm.BotBus.menu = false;
		cheatClient->AutoFarm.BotCourier.menu = false;
		cheatClient->AutoFarm.BotFactory.menu = false;
		cheatClient->AutoFarm.BotFarm.menu = false;
		cheatClient->AutoFarm.BotMiner.menu = false;
		cheatClient->AutoFarm.BotTrucker.menu = false;
	}
	lastMenuOpen = menuOpen;
	if(++saveCounter >= 600)
	{
		saveCounter = 0;
		cheatClient->SaveSettings();
	}
	
	uintptr_t localped = cheatClient->FindLocalPlayer();
#ifdef __aarch64__
	// arm64: fill ServerPlayers from CPlayerPool (CRemotePlayer: ped@+8, name@+0x10, id@+0x42)
	static int poolScanCounter = 0;
	if(++poolScanCounter >= 60)
	{
		poolScanCounter = 0;
		for(int i = 0; i < 1500; i++)
		{
			uintptr_t rp = cheatClient->GetAtPlayerPool(i);
			ServerPlayer sp = {0};
			sp.id = -1;
			if(rp > 0x100000)
			{
#ifdef __aarch64__
				strncpy(sp.nickname, (const char *)(rp + 0x18), 24);
				sp.nickname[24] = 0;
				sp.id = *(uint16_t *)(rp + 0x4A);
#else
				strncpy(sp.nickname, (const char *)(rp + 0x10), 24);
				sp.nickname[24] = 0;
				sp.id = *(uint16_t *)(rp + 0x42);
#endif
			}
			cheatClient->ServerPlayers[i] = sp;
		}
	}
	// debug: dump player slot + candidate ped objects every ~3s
#if 0 // crashed on stale lp — served its purpose
	static int dbgCounter = 0;
	if(++dbgCounter >= 180)
	{
		dbgCounter = 0;
		uintptr_t pool = cheatClient->GetPlayerPool();
		uintptr_t lp = (pool > 0x100000) ? *(uintptr_t *)pool : 0;    // CPlayerPool::m_pLocalPlayer
		uintptr_t poolPed = (lp > 0x100000) ? *(uintptr_t *)(lp + 0x38) : 0; // CLocalPlayer::m_pPlayerPed
		uintptr_t poolVeh = (lp > 0x100000) ? *(uintptr_t *)(lp + 0x40) : 0; // CLocalPlayer::m_pVehicle
		uintptr_t obj0 = ((uintptr_t (*)())(libAddr + 0xB85690))();   // accessor result for comparison
		__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
			"pool=%p lp=%p poolPed=%p poolVeh=%p accessor=%p lpName=%.20s",
			(void *)pool, (void *)lp, (void *)poolPed, (void *)poolVeh, (void *)obj0,
			(lp > 0x100000) ? (char *)(lp + 2) : "?");
		uintptr_t cand[4] = { obj0, poolPed, lp, 0 };
		for(int ci = 0; ci < 4; ci++)
		{
			uintptr_t o = cand[ci];
			if(!o) { __android_log_print(ANDROID_LOG_INFO, "PEDDBG", "obj%d = NULL", ci); continue; }
			uintptr_t vp = *(uintptr_t *)o;
			if(ci != 2 && (vp < libAddr || vp > libAddr + 0x2800000))
			{
				__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "obj%d=%p vptr=%llx — not entity, skip", ci, (void *)o, vp);
				continue;
			}
			__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
				"obj%d=%p vptr=%llx pos=%.1f,%.1f,%.1f hp=%.1f/%.1f arm=%.1f st=%d mv=%d stam=%.1f/%.1f rot=%.2f veh=%p inv=%x",
				ci, (void *)o,
				*(unsigned long long *)o,
				*(float *)(o + 0x38), *(float *)(o + 0x3C), *(float *)(o + 0x40),
				*(float *)(o + 0x4B0), *(float *)(o + 0x4B4), *(float *)(o + 0x4B8),
				*(int *)(o + 0x494), *(int *)(o + 0x49C),
				*(float *)(o + 0x808), *(float *)(o + 0x80C),
				*(float *)(o + 0x4E0),
				(void *)*(uintptr_t *)(o + 0x538),
				*(unsigned int *)(o + 0x6E0));
			// float scan: find where plausible hp/armour/stamina/velocity live
			for(int base = 0x80; base < 0x1600; base += 0x40)
			{
				__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
					"obj%d +%03X: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
					ci, base,
					*(float *)(o + base + 0x00), *(float *)(o + base + 0x04),
					*(float *)(o + base + 0x08), *(float *)(o + base + 0x0C),
					*(float *)(o + base + 0x10), *(float *)(o + base + 0x14),
					*(float *)(o + base + 0x18), *(float *)(o + base + 0x1C),
					*(float *)(o + base + 0x20), *(float *)(o + base + 0x24),
					*(float *)(o + base + 0x28), *(float *)(o + base + 0x2C),
					*(float *)(o + base + 0x30), *(float *)(o + base + 0x34),
					*(float *)(o + base + 0x38), *(float *)(o + base + 0x3C));
			}
		}
		__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
			"flags: inv=%d run=%d fly=%d rot=%d long=%d high=%d water=%d",
			cheatClient->PlayerCheat.Base.Invulnerability.health,
			cheatClient->PlayerCheat.Base.Invulnerability.endurance,
			cheatClient->PlayerCheat.Base.FlyPed.state,
			cheatClient->PlayerCheat.Base.Control.fastRotate,
			cheatClient->PlayerCheat.Base.Jumping.long_,
			cheatClient->PlayerCheat.Base.Jumping.high,
			cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater);
	}
#endif // #if 0 debug dump
#endif
	if(localped)
	{
		CPed localplayer = CPed(localped);

		Vec3 local_coors = cheatClient->GetPos(localped);
		
#ifndef __aarch64__
		*(float *)(libAddr + string2Offset(OBFUSCATE("0x464508C"))) = cheatClient->GameSpeed.speed;
		
		if(cheatClient->VisualCheat.TimeChanger.freeze)
		{
			*(int *)(gameLibAddr + string2Offset(OBFUSCATE("0x4B6263D"))) = cheatClient->VisualCheat.TimeChanger.time;
		}
#else
		// arm64 CLOCK: scan client writable LOAD segments for msPerGameMinute==1000.
		// Live clock layout (verified): u32 hour @base+4, u32 min @base+8,
		// u32 msPerMin @base+16, u32 lastTick @base+20 (monotonically increases).
		// Auto-pick: hit whose u32@-12 (hour)<=23, u32@-8 (min)<=59 and whose
		// tick field grew since the previous scan.
		static uintptr_t clockBase = libAddr + 0x234a0f0;  // verified: u32 h@+4, m@+8
		static int clkTick = 0;
		static uintptr_t clkSeen[64]; static uint32_t clkSeenTick[64]; static int clkSeenN = 0;
		if(false && clkTick++ % 600 == 120)  // scanner disabled — offset hardcoded
		{
			unsigned char eh[64] = {0};
			if(SafeRead(libAddr, eh, 64))
			{
				uint64_t phoff = *(uint64_t *)(eh + 0x20);
				uint16_t phents = *(uint16_t *)(eh + 0x36), phnum = *(uint16_t *)(eh + 0x38);
				for(int p = 0; p < phnum && !clockBase; p++)
				{
					unsigned char ph[56] = {0};
					if(!SafeRead(libAddr + phoff + p * phents, ph, 56)) continue;
					uint32_t ptype = *(uint32_t *)ph, pflags = *(uint32_t *)(ph + 4);
					uint64_t pv = *(uint64_t *)(ph + 0x10), pm = *(uint64_t *)(ph + 0x28);
					if(ptype != 1 || !(pflags & 2) || !pm) continue;
					static unsigned char buf[0x10000 + 64];
					for(uint64_t off = 0; off < pm && !clockBase; off += 0x10000)
					{
						uint64_t base = libAddr + pv + off;
						size_t rd = (pm - off < 0x10000) ? (size_t)(pm - off) : 0x10000;
						if(!SafeRead(base, buf, rd)) continue;
						for(size_t i = 16; i + 24 <= rd; i += 4)
						{
							if(*(uint32_t *)(buf + i) != 1000) continue;
							uint32_t hh = *(uint32_t *)(buf + i - 12), mm = *(uint32_t *)(buf + i - 8);
							uint32_t tk = *(uint32_t *)(buf + i + 4);
							if(hh > 23 || mm > 59) continue;
							uintptr_t hit = base + i;
							// known candidate? check tick growth
							for(int s = 0; s < clkSeenN; s++)
								if(clkSeen[s] == hit)
								{
									if(tk > clkSeenTick[s])
									{
										clockBase = hit - 16;
										__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
											"CLOCK FOUND base=%llx h=%u m=%u tick %x->%x",
											(unsigned long long)clockBase, hh, mm,
											clkSeenTick[s], tk);
									}
									clkSeenTick[s] = tk;
									goto next_hit;
								}
							if(clkSeenN < 64) { clkSeen[clkSeenN] = hit; clkSeenTick[clkSeenN++] = tk; }
							__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
								"CLKSCAN cand %llx h=%u m=%u tick=%x",
								(unsigned long long)(hit - libAddr), hh, mm, tk);
						next_hit:;
						}
					}
				}
			}
		}
		if(clockBase)  // clockBase is absolute (hit-16)
		{
			// apply pending one-shot set + hold frozen time (u32 hour/min fields)
			if(g_clockSet >= 0)
			{
				*(uint32_t *)(clockBase + 4) = (uint32_t)g_clockSet;
				*(uint32_t *)(clockBase + 8) = 0;
				__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "CLOCK set h=%d", g_clockSet);
				g_clockSet = -1;
			}
			if(cheatClient->VisualCheat.TimeChanger.freeze)
			{
				*(uint32_t *)(clockBase + 4) = (uint32_t)cheatClient->VisualCheat.TimeChanger.time;
				*(uint32_t *)(clockBase + 8) = 0;
			}
			static int clkRb = 0;
			if(clkRb++ % 120 == 0)  // readback every ~2s: does server stomp our write?
			{
				uint32_t rh = *(uint32_t *)(clockBase + 4), rm = *(uint32_t *)(clockBase + 8);
				__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
					"CLOCK readback h=%u m=%u freeze=%d", rh, rm,
					cheatClient->VisualCheat.TimeChanger.freeze);
			}
			// sky clock hunt: byte-packed CClock candidates [u16 days][u16 months]
			// [u8 year][u8 hour][u8 min][u8 sec]. Pass 1 collects all; later
			// passes keep only those whose min/sec fields TICK — the sky clock
			// is a live clock independent of the display clock we write.
			static int skyScan = 0;
			static uintptr_t skyCand[1024]; static unsigned char skyPrevM[1024], skyPrevS[1024];
			static int skyCandN = 0;
			if(skyScan < 4 && clkRb % 480 == 240)
			{
				uint32_t th = *(uint32_t *)(clockBase + 4), tm = *(uint32_t *)(clockBase + 8);
				FILE *mf = fopen("/proc/self/maps", "r");
				if(mf)
				{
					char line[512];
					static unsigned char sbuf[0x10000];
					int found = 0;
					while(fgets(line, sizeof(line), mf))
					{
						uintptr_t rs, re; char perm[8] = {0};
						if(sscanf(line, "%llx-%llx %4s", (unsigned long long *)&rs,
							(unsigned long long *)&re, perm) != 3) continue;
						if(perm[1] != 'w') continue;
						if(re - rs > 0x4000000) continue;
						for(uintptr_t off = 0; off + 8 <= re - rs; off += 0x10000)
						{
							uintptr_t base = rs + off;
							size_t rd = (re - rs - off < 0x10000) ? (size_t)(re - rs - off) : 0x10000;
							if(!SafeRead(base, sbuf, rd)) break;
							for(size_t i = 0; i + 8 <= rd; i++)
							{
								if(sbuf[i+5] > 23 || sbuf[i+6] > 59 || sbuf[i+7] > 59) continue;
								if(*(uint16_t *)(sbuf + i + 2) > 12 || *(uint16_t *)(sbuf + i) > 40) continue;
								if(!(sbuf[i+5] | sbuf[i+6] | sbuf[i+7])) continue;   // skip all-zero
								uintptr_t haddr = base + i;
								if(haddr >= clockBase && haddr <= clockBase + 16) continue;
								if(skyScan == 0)
								{
									if(skyCandN < 1024)
									{
										skyCand[skyCandN] = haddr;
										skyPrevM[skyCandN] = sbuf[i+6];
										skyPrevS[skyCandN] = sbuf[i+7];
										skyCandN++;
									}
								}
								else
								{
									for(int s = 0; s < skyCandN; s++)
										if(skyCand[s] == haddr)
										{
											unsigned char nm = sbuf[i+6], ns = sbuf[i+7];
											if(nm != skyPrevM[s] || ns != skyPrevS[s])
											{
												unsigned char b2[16] = {0};
												SafeRead(haddr, b2, 16);
												__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
													"SKYCLK LIVE %llx: %02x%02x %02x%02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x",
													(unsigned long long)haddr,
													b2[1],b2[0], b2[3],b2[2], b2[4],b2[5],b2[6],b2[7],
													b2[8],b2[9],b2[10],b2[11],b2[12],b2[13],b2[14],b2[15]);
												found++;
											}
											skyPrevM[s] = nm; skyPrevS[s] = ns;
											break;
										}
								}
							}
						}
					}
					fclose(mf);
					__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
						"SKYCLK pass=%d candN=%d live=%d th=%u tm=%u", skyScan, skyCandN, found, th, tm);
					skyScan++;
				}
			}
		}
#endif

		// Быстрый поворот
		if(cheatClient->PlayerCheat.Base.Control.fastRotate)
		{
			localplayer.SetHeadingRate(7.5f * cheatClient->PlayerCheat.Base.Control.rate);
		}
#ifndef __aarch64__
		else
		{
			localplayer.SetHeadingRate(7.5f);
		}
#endif

#ifdef __aarch64__
		// arm64: Бессмертие — pin hp at legal max 100 (server sync sees
		// normal hp; >100 anomalies may trip the anticheat) + proof flags
		if(cheatClient->PlayerCheat.Base.Invulnerability.health)
		{
			float hp = *(float *)(localped + 0x4B0);
			if(hp < 100.0f) { *(float *)(localped + 0x4B0) = 100.0f; }
			*(uint32_t *)(localped + 0x78) |= 0x3E200000;
			*(uint32_t *)(localped + 0x6E0) = 0x7FFFFFFF;
			*(uint32_t *)(localped + 0x6E4) = 0x7FFFFFFF;
		}
		else
		{
			if(*(uint32_t *)(localped + 0x6E0) == 0x7FFFFFFF)
			{
				*(uint32_t *)(localped + 0x6E0) = 0;
				*(uint32_t *)(localped + 0x6E4) = 0;
			}
		}

		// arm64: Бесконечный бег — pin sprint gauge +0x808/+0x810 to full;
		// the sprint button's own speed applies while gauge > 0
		if(cheatClient->PlayerCheat.Base.Invulnerability.endurance)
		{
			for(int off : {0x808, 0x810})
			{
				float v = *(float *)(localped + off);
				if(v >= 0.f && v < 1000.f) { *(float *)(localped + off) = 285.9f; }
			}
		}

		// arm64: Выход из больницы — instant heal + replicate the client's
		// unfreeze routine (client @0xB8594C): CPad+0x10E &= ~0x20,
		// ped+0x8C &= ~0x3E0200, ped+0x8D1 = 1, CLocalPlayer+0x1EC = 1
		if(hospitalUnlock)
		{
			*(float *)(localped + 0x4B0) = 100.0f;
			typedef uintptr_t (*GetPad_t)(int);
			uintptr_t pad = ((GetPad_t)(libAddr + 0xB7FF28))(0);
			if(pad > 0x100000)
				*(uint16_t *)(pad + 0x10E) &= ~0x20u;
			*(uint32_t *)(localped + 0x8C) &= ~0x3E0200u;
			*(uint8_t *)(localped + 0x8D1) = 1;
			uintptr_t pool = cheatClient->GetPlayerPool();
			uintptr_t lp = (pool > 0x100000) ? *(uintptr_t *)pool : 0;
			if(lp > 0x100000)
			{
				static int hosDbg = 0;
				if(++hosDbg >= 180)
				{
					hosDbg = 0;
					__android_log_print(ANDROID_LOG_INFO, "TRACE_LAST_FUNC",
						"HOSP lp+1EC=%02x pad+10E=%04x ped+8C=%08x",
						*(uint8_t *)(lp + 0x1EC), *(uint16_t *)(pad + 0x10E),
						*(uint32_t *)(localped + 0x8C));
				}
				*(uint8_t *)(lp + 0x1EC) = 1;
			}
		}

		// arm64: remote slap — remote ped position is rewritten from sync
		// every tick, so re-apply the fling every frame for the window.
		// Local-only visual: server never sees remote ped changes.
		if(slapTargetId >= 0)
		{
			uintptr_t rp = cheatClient->GetAtPlayerPool(slapTargetId);
			uintptr_t rped = (rp > 0x100000) ? *(uintptr_t *)(rp + slapPedOff) : 0;
			if(rped > 0x100000)
			{
				// first tick: dump remote player + ped so we can locate the
				// sync-interp target fields (m_vecOnFootTargetPos etc.)
				if(slapTicks == 120)
				{
					__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
						"SLAP rp=%p rped=%p pos=%.1f,%.1f,%.1f",
						(void *)rp, (void *)rped,
						*(float *)(rped + 0x38), *(float *)(rped + 0x3C), *(float *)(rped + 0x40));
					for(int off = 0; off < 0x400; off += 0x40)
						__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
							"rp+%03X: %.1f %.1f %.1f %.1f | %.1f %.1f %.1f %.1f | %.1f %.1f %.1f %.1f | %.1f %.1f %.1f %.1f",
							off,
							*(float *)(rp+off), *(float *)(rp+off+4), *(float *)(rp+off+8), *(float *)(rp+off+12),
							*(float *)(rp+off+16), *(float *)(rp+off+20), *(float *)(rp+off+24), *(float *)(rp+off+28),
							*(float *)(rp+off+32), *(float *)(rp+off+36), *(float *)(rp+off+40), *(float *)(rp+off+44),
							*(float *)(rp+off+48), *(float *)(rp+off+52), *(float *)(rp+off+56), *(float *)(rp+off+60));
				}
				if(slapTicks == 120)
					slapBaseZ = *(float *)(rp + 0x12C); // server pos Z at slap time
				float z = slapBaseZ + cheatClient->PlayerCheat.Base.Slap.up;
				*(float *)(rped + ENT_POS_Z) = z;
				*(float *)(rped + 0x188) = cheatClient->PlayerCheat.Base.Slap.up;
				// remote ped pos is rebuilt from interp targets inside the
				// remote-player object — sync overwrites them ~10x/sec, so
				// pin absolute lifted Z every frame, not an increment
				*(float *)(rp + 0x5C) = z;   // vecOnFootTargetPos.z
				*(float *)(rp + 0x12C) = z;  // sync pos copy .z
				*(float *)(rp + 0x114) = z;  // second pos triple .z
			}
			else if(slapTicks == 120)
				__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "SLAP rp=%p rped=INVALID", (void *)rp);
			if(--slapTicks <= 0)
				slapTargetId = -1;
		}

		// arm64: Ходьба под водой — pin oxygen +0x880/+0x884
		// (170 land / ~160 water observed; vois writes +0x880 too)
		if(cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater)
		{
			*(float *)(localped + 0x880) = 170.0f;
			*(float *)(localped + 0x884) = 170.0f;
		}

		// arm64: Длинный прыжок — boost forward velocity +0x180/+0x184
		// while falling (vel offset confirmed in vois WalkSpeed)
		if(cheatClient->PlayerCheat.Base.Jumping.long_)
		{
			static float prevZ = 0.f;
			float curZ = *(float *)(localped + ENT_POS_Z);
			if(curZ < prevZ - 0.01f) // falling
			{
				float heading = *(float *)(localped + 0x4E0);
				float boost = 0.5f * cheatClient->PlayerCheat.Base.Jumping.long__;
				*(float *)(localped + 0x180) += -sinf(heading) * boost;
				*(float *)(localped + 0x184) += cosf(heading) * boost;
			}
			prevZ = curZ;
		}
#endif

		// Авто аптечка / авто броня — с порогами и кулдауном 3с
		uint32_t healNow = cheatClient->timer->GetTimeInMilliseconds();
		if(cheatClient->PlayerCheat.Rage.Invulnerability.autoHeal
			&& localplayer.GetHealth() > 0.f
			&& localplayer.GetHealth() < cheatClient->PlayerCheat.Rage.Invulnerability.healBelow
			&& healNow - cheatClient->PlayerCheat.Rage.Invulnerability.lastUse > 3000)
		{
			SendChatCommand(OBFUSCATE("/healme"));
			cheatClient->PlayerCheat.Rage.Invulnerability.lastUse = healNow;
		}
		if(cheatClient->PlayerCheat.Rage.Invulnerability.autoArmour
			&& localplayer.GetHealth() > 0.f
			&& localplayer.GetArmour() < cheatClient->PlayerCheat.Rage.Invulnerability.armourBelow
			&& healNow - cheatClient->PlayerCheat.Rage.Invulnerability.lastUse > 3000)
		{
			SendChatCommand(OBFUSCATE("/armour"));
			cheatClient->PlayerCheat.Rage.Invulnerability.lastUse = healNow;
		}
		
		// Fly Ped
		static bool flyLatchOff = true;
#ifdef __aarch64__
		static bool flyLanding = false;
		if(!cheatClient->PlayerCheat.Base.FlyPed.state)
		{
			if(!flyLatchOff && localped) // just turned off — descend gently
			{
				flyLanding = true;
				*(float *)(localped + 0x180) = 0.f;
				*(float *)(localped + 0x184) = 0.f;
				*(float *)(localped + 0x188) = 0.f;
			}
			flyLatchOff = true;
		}
		if(flyLanding && localped)
		{
			float px = *(float *)(localped + ENT_POS_X);
			float py = *(float *)(localped + ENT_POS_Y);
			float cz = *(float *)(localped + ENT_POS_Z);
			float gz = FindGroundZ ? FindGroundZ(px, py) : cz;
			float target = gz + 0.5f;
			if(cz - target < 0.6f)
			{
				*(float *)(localped + ENT_POS_Z) = target;
				*(float *)(localped + 0x2A8) = target;
				flyLanding = false;
			}
			else
			{
				cz -= 0.25f;
				*(float *)(localped + ENT_POS_Z) = cz;
				*(float *)(localped + 0x2A8) = cz;
			}
		}
#else
		if(!cheatClient->PlayerCheat.Base.FlyPed.state) { flyLatchOff = true; }
#endif
		if(cheatClient->PlayerCheat.Base.FlyPed.state)
		{
			uintptr_t localped = cheatClient->FindLocalPlayer();
			if(localped)
			{
#ifdef __aarch64__
				// arm64: fly via ped velocity +0x180/0x184/0x188 (vois-style):
				// vel.xy = joystick dir * speed, vel.z = 0 (hover); alt-guard via FindGroundZ
				static float flyZ = 0.f;
				if(flyLatchOff) {
					flyZ = *(float *)(localped + ENT_POS_Z) + 1.5f;
					flyLatchOff = false;
				}
				if(*(char *)(localped + 0x540))
				{
					cheatClient->PlayerCheat.Base.FlyPed.state = false;
				}
				else
				{
					float heading = *(float *)(localped + 0x4E0);
					float mx = gJoyX;
					float my = -gJoyY;
					cheatClient->PlayerCheat.Base.FlyPed.speed += 0.05f;
					if(cheatClient->PlayerCheat.Base.FlyPed.speed > 6.f) {
						cheatClient->PlayerCheat.Base.FlyPed.speed = 6.f;
					}
					float step = cheatClient->PlayerCheat.Base.FlyPed.speed * 0.15f;
					float dx = (-sinf(heading) * my + cosf(heading) * mx) * step;
					float dy = (cosf(heading) * my + sinf(heading) * mx) * step;
					// keep at least 1.5m above terrain
					float cx = *(float *)(localped + ENT_POS_X);
					float cy = *(float *)(localped + ENT_POS_Y);
					float gz = FindGroundZ ? FindGroundZ(cx, cy) : flyZ - 2.f;
					float minZ = gz + 1.5f;
					float vz = 0.f;
					if(*(float *)(localped + ENT_POS_Z) < minZ) { vz = 4.0f; }
					*(float *)(localped + 0x180) = dx;
					*(float *)(localped + 0x184) = dy;
					*(float *)(localped + 0x188) = vz;
				}
#else
				if(*(char *)(localped + string2Offset(OBFUSCATE("0x150"))))
				{
					cheatClient->PlayerCheat.Base.FlyPed.speed = 0.f;
				}
				else
				{
					Vec3 localpos = cheatClient->GetPos(localped);
					Vec3 camPos = cheatClient->GetCameraCoors();
					float warpX = localpos.x - camPos.x;
					float warpY = localpos.y - camPos.y;
					float warpZ = localpos.z - camPos.z;
					cheatClient->PlayerCheat.Base.FlyPed.speed += 0.1f;
					if(cheatClient->PlayerCheat.Base.FlyPed.speed > 10.f) {
						cheatClient->PlayerCheat.Base.FlyPed.speed = 10.f;
					}
					*(float *)(localped + string2Offset(OBFUSCATE("0x7C"))) = warpX * (cheatClient->PlayerCheat.Base.FlyPed.speed * 0.01f);
					*(float *)(localped + string2Offset(OBFUSCATE("0x80"))) = warpY * (cheatClient->PlayerCheat.Base.FlyPed.speed * 0.01f);
					*(float *)(localped + string2Offset(OBFUSCATE("0x84"))) = warpZ * (cheatClient->PlayerCheat.Base.FlyPed.speed * 0.01f);
				}
#endif
			}
		}
		
		if(cheatClient->Flooder.working)
		{
			if(cheatClient->timer->GetTimeInMilliseconds() - cheatClient->Flooder.last_sended_message_time > cheatClient->Flooder.delay_after_message)
			{
				if(cheatClient->Flooder.message[0] == '/')
				{
					SendChatCommand(strdup(cheatClient->Flooder.message.c_str()));
				}
				else
				{
					SendChatMessage(strdup(cheatClient->Flooder.message.c_str()));
				}
				cheatClient->Flooder.last_sended_message_time = cheatClient->timer->GetTimeInMilliseconds();
			}
		}
		// NameTags
		if(cheatClient->Wallhack.Player.nametags)
		{
			if(cheatClient->GetNameTagsDrawDistance() != 2000.f)
			{
				cheatClient->SetNameTagsDrawDistance(2000.f);
			}
		}
		else
		{
			cheatClient->SetNameTagsDrawDistance(10.f);
		}
		
		// Вечный двигатель
		if(cheatClient->VehicleCheat.Invulnerability.engine)
		{
			if(localplayer.GetVehicle())
			{
				CVehicle veh = CVehicle(localplayer.GetVehicle());
				if(veh.GetDriver() == localplayer.GetMe())
				{
					veh.SetEngineState(true);
				}
			}
		}
		
#ifdef __aarch64__
		// arm64 NoReload: WEAPONS_UPDATE (id=35) leaks weapon id @ +6 and the
		// tracked ammo u16 @ +7. Locate the CWeapon slot inside the local ped:
		// scan for int == weaponId whose neighbours hold the ammo counter —
		// then pin the adjacent clip field so reload never triggers.
		if(cheatClient->PlayerCheat.Rage.NoReload.state && localped && lastWpnUpdLen >= 8)
		{
			static uintptr_t ammoAddr[16] = {};
			static int ammoCount = -1;
			static int lastAmmo = -1, lastWpnId = -1;
			int ammo = *(uint16_t *)(lastWpnUpd + 7);
			int wpn  = lastWpnUpd[6];
			if(ammo != lastAmmo || wpn != lastWpnId || ammoCount < 0)
			{
				ammoCount = 0;
				int wpnHits = 0, ammoHits = 0;
				// ammo counter may live in the CLocalPlayer object (m_dwLastAmmo
				// vector) rather than inside the ped — scan both
				uintptr_t objs[2] = { localped, 0 };
				uintptr_t pool = cheatClient->GetPlayerPool();
				if(pool > 0x100000) objs[1] = *(uintptr_t *)pool; // CLocalPlayer
				for(int oi = 0; oi < 2 && ammoCount < 16; oi++)
				{
					uintptr_t obj = objs[oi];
					if(obj < 0xb400000000000000ULL || obj > 0xb500000000000000ULL) continue;
					for(int off = 0x100; off < 0x1800 && ammoCount < 16; off += 4)
					{
						int v = *(int *)(obj + off);
						if(v == ammo) { ammoHits++; ammoAddr[ammoCount++] = obj + off; }
						else if(v == wpn) wpnHits++;
					}
					// weapon slot array may be a separate heap object pointed
					// from inside the ped — follow one level of heap pointers
					for(int off = 0x100; off < 0x800 && ammoCount < 16; off += 8)
					{
						uintptr_t sub = *(uintptr_t *)(obj + off);
						if(sub < 0xb400000000000000ULL || sub > 0xb500000000000000ULL) continue;
						if(!RangeResident(sub, 0x800)) continue; // garbage-but-heap-looking qword
						for(int so = 0; so < 0x800 && ammoCount < 16; so += 4)
							if(*(int *)(sub + so) == ammo) { ammoHits++; ammoAddr[ammoCount++] = sub + so; }
					}
					// ammo may also be stored as u16
					for(int off = 0x100; off < 0x1800 && ammoCount < 16; off += 2)
						if(*(uint16_t *)(obj + off) == ammo && *(int *)(obj + off) != ammo)
							{ ammoHits++; ammoAddr[ammoCount++] = obj + off + 0x80000000; } // u16 flag
				}
				lastAmmo = ammo; lastWpnId = wpn;
				__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "NORELOAD wpn=%d ammo=%d wpnHits=%d ammoHits=%d cand=%d", wpn, ammo, wpnHits, ammoHits, ammoCount);
			}
			for(int i = 0; i < ammoCount; i++)
			{
				uintptr_t a = ammoAddr[i] & ~0x80000000ULL;
				__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "NORELOAD cand[%d]=%p%s val=%d",
					i, (void *)a, (ammoAddr[i] & 0x80000000ULL) ? "(u16)" : "",
					(ammoAddr[i] & 0x80000000ULL) ? *(uint16_t *)a : *(int *)a);
				// log-only for now — blind pinning corrupted game objects (crash)
			}
		}
#else
		if(cheatClient->PlayerCheat.Rage.NoReload.state)
		{
			// deagle
			cheatClient->PlayerCheat.Rage.NoReload.deagleClip = *(int *)(localped + string2Offset(OBFUSCATE("0x444")));
			if(cheatClient->PlayerCheat.Rage.NoReload.deagleClip == 1)
			{
				*(int *)(localped + string2Offset(OBFUSCATE("0x444"))) += 1;
				cheatClient->PlayerCheat.Rage.NoReload.prev_deagleClip = cheatClient->PlayerCheat.Rage.NoReload.deagleClip;
			}
			
			// ak47
			cheatClient->PlayerCheat.Rage.NoReload.AK47Clip = *(int *)(localped + string2Offset(OBFUSCATE("0x474")));
			if(cheatClient->PlayerCheat.Rage.NoReload.AK47Clip == 1)
			{
				*(int *)(localped + string2Offset(OBFUSCATE("0x474"))) += 1;
				cheatClient->PlayerCheat.Rage.NoReload.prev_AK47Clip = cheatClient->PlayerCheat.Rage.NoReload.AK47Clip;
			}
			
			// M4
			cheatClient->PlayerCheat.Rage.NoReload.m4Clip = *(int *)(localped + string2Offset(OBFUSCATE("0x48C")));
			
			if(cheatClient->PlayerCheat.Rage.NoReload.m4Clip == 1)
			{
				*(int *)(localped + string2Offset(OBFUSCATE("0x48C"))) += 1;
				cheatClient->PlayerCheat.Rage.NoReload.prev_m4Clip = cheatClient->PlayerCheat.Rage.NoReload.m4Clip;
			}
		}
#endif

		// CarShoot
		if(cheatClient->VehicleCheat.CarShot.state)
		{
			if(localplayer.GetVehicle())
			{
				CVehicle veh = CVehicle(localplayer.GetVehicle());
				if(veh.GetDriver() == localplayer.GetMe())
				{
					uintptr_t vehicle = localplayer.GetVehicle();
					uintptr_t vehicleMatrix = vehicle + 4;
					float cameraAngle = atan2f(*(float *)(cheatClient->GetCameraMatrix() + 4 + 16), *(float *)(cheatClient->GetCameraMatrix() + 4 + 20));
					
					cheatClient->SetRotate(vehicleMatrix, 0, 0, -cameraAngle);
					//cheatClient->SetVehicleSpeed(vehicle, cheatClient->CarShoot.speed);
					
					if(cheatClient->Accelerate.isPressed)
					{
						cheatClient->VehicleCheat.CarShot.speed += cheatClient->VehicleCheat.CarShot.accelerate;
						if(cheatClient->VehicleCheat.CarShot.speed >= 100.f)
						{
							cheatClient->VehicleCheat.CarShot.speed = 100.f;
						}
					}
					else
					{
						cheatClient->VehicleCheat.CarShot.speed = 0;
					}
				}
			}
		}
		
		/*for(int i = 0; i < 1500; i++)
		{
			int playerpool = cheatClient->GetPlayerPool();
			if(!playerpool) { continue; }
			uintptr_t player = CPlayerPool_GetAt(playerpool, i);
			if(player)
			{
				player = *(uintptr_t *)(player + PLAYER_PED_OFF);
				if(player > 0x100000)
				{
					if(player == contact_entity)
					{
						int number = *(int *)(player + 80);
						number &= ~0x100;
						*(int *)(player + 80) = number;
					}
					else
					{
						int number = *(int *)(player + 80);
						number |= 0x100;
						*(int *)(player + 80) = number;
					}
				}
			}
		}*/
		
		// АимБот
		if(cheatClient->Aimbot.state)
		{
#ifdef __aarch64__
			// arm64: aimbot is implemented as silent-aim packet patching in
			// RakLogSend (id=29 retarget) — no per-frame camera writes needed
#else
			Vec3 camera_pos = cheatClient->GetCameraCoors();
			auto bestFov = 180.f;
			auto bestAngle = Vec3(0,0,0);
			// Если игрок атакует
			if(*(int *)(localped + std::stoi(OBFUSCATE("592"))) == PEDSTATE_AIMGUN || *(int *)(localped + 592) == PEDSTATE_FIGHT || *(int *)(localped + 592) == PEDSTATE_ATTACK)
			{
				// Получить текущее оружие
				if(cheatClient->Aimbot.target_id != -1)
				{
					uintptr_t target = cheatClient->GetAtPlayerPool(cheatClient->Aimbot.target_id);
					if(target) {
						target = *(int *)(target + 4);
						if(target) {
							Vec3 target_coors = cheatClient->GetPos(target);
							Vec3 viewangle = Vec3(*(float *)(libAddr + 0x825CFC), *(float *)(libAddr + 0x825CEC), 0);
							float angle = GetAngleBetweenTwoPoints(camera_pos.x, camera_pos.y, target_coors.x, target_coors.y);
							
							float				ax, az, fz, fx;
							Vec3				vect = {0,0,0};
							float				screenAspectRatio = 0.457f;
							float				crosshairOffset[2] = {0.f,0.f};
							
							// calculate distance vector
							vect = local_coors - target_coors;
			
							//if (pCam->Mode == 53 || pCam->Mode == 55) // 3rd person mode
							{
								// weird shit
								float mult = tan(67.f * 0.5f * 0.017453292f);
								fz = ( M_PI * 2.f) - atan2(1.0f, mult * ((0.5f - crosshairOffset[0] + 0.5f - crosshairOffset[0]) * (1.0f / screenAspectRatio)));
								fx = M_PI - atan2(1.0f, mult * (crosshairOffset[1] - 0.5f + crosshairOffset[1] - 0.5f));
							}
							//else
							{
								//fx = fz = M_PI / 2;
							}
							// x angle
							float dist = sqrt(vect.x * vect.x + vect.y * vect.y);
							az = atan2f(dist, vect.z);

							// z angle
							ax = atan2f(vect.y, -vect.x) - M_PI / 2;

#ifndef __aarch64__
							*(float *)(libAddr + 0x825CEC) = az - fz;
#endif
							//*(float *)(libAddr + 0x59D8C4) = -(ax - fx);
							/*pCam->AlphaSpeed = 0.0f;
							pCam->BetaSpeed = 0.0f;*/
			
							/*Vec3 target_on_screen = {0,0,0};
							if(WorldToScreen(&target_coors, &target_on_screen))
							{
								if(target_on_screen.z >= 1.f)
								{
									if(target_on_screen.x >= (cheatClient->screen_width * crosshair_offset_x) - 5 &&
									target_on_screen.x <= (cheatClient->screen_width * crosshair_offset_x) + 5 &&
									target_on_screen.y >= (cheatClient->screen_height * crosshair_offset_y) - 5 &&
									target_on_screen.y <= (cheatClient->screen_height * crosshair_offset_y) + 5)
									{
										
									}
									else if(target_on_screen.x >= (cheatClient->screen_width * crosshair_offset_x) + 10)
									{
										cheatClient->Aimbot.diffX -= 1.f;
									}
									else if(target_on_screen.x <= (cheatClient->screen_width * crosshair_offset_x) - 10)
									{
										cheatClient->Aimbot.diffX += 1.f;
									}
									else if(target_on_screen.y >= (cheatClient->screen_height * crosshair_offset_y) + 10)
									{
										cheatClient->Aimbot.diffY -= 0.5f;
									}
									else if(target_on_screen.y <= (cheatClient->screen_height * crosshair_offset_y) - 10)
									{
										cheatClient->Aimbot.diffY += 0.5f;
									}
								}
								else
								{
									cheatClient->Aimbot.diffX += 1.f;
								}
							}
							
							*(float *)(libAddr + 0x59D8C4) = angle + ((cheatClient->Aimbot.diffX * M_PI) / 180.f);
							*(float *)(libAddr + 0x59D8B4) = (cheatClient->Aimbot.diffY * M_PI) / 180.f;*/
						}
					}
				}
			}
			else
			{
				cheatClient->Aimbot.diffX = 0;
				cheatClient->Aimbot.diffY = 0;
				
				int target_id = cheatClient->FindNearestPlayer();
				if(target_id != -1) {
					uintptr_t target = cheatClient->GetAtPlayerPool(target_id);
					if(target) {
						target = *(int *)(target + 4);
						if(target) {
							cheatClient->Aimbot.target_id = target_id;
							cheatClient->Aimbot.target = target;
						}
						else
						{
							cheatClient->Aimbot.target_id = -1;
							cheatClient->Aimbot.target = 0;
						}
					}
					else
					{
						cheatClient->Aimbot.target_id = -1;
						cheatClient->Aimbot.target = 0;
					}
				} else {
					cheatClient->Aimbot.target_id = target_id;
					//cheatClient->target = cheatClient->GetAtPlayerPool(target_id);
				}
			}
#endif
		}
		
#ifndef __aarch64__
		if(cheatClient->PlayerCheat.Rage.Damager.state)
		{
			int now_time = cheatClient->timer->GetTimeInMilliseconds();
			int weaponId = cheatClient->GetWeaponId();
			
				if(now_time - cheatClient->PlayerCheat.Rage.Damager.last_shot > 500)
				{
					if(gpeer != 0 && ga7 != 0 && ga8 != 0)
					{
						int near_id = cheatClient->FindNearestPlayer();
						if(near_id != -1)
						{
							uintptr_t player = cheatClient->GetAtPlayerPool(near_id);
							if(player) {
								player = *(uintptr_t *)(player + PLAYER_PED_OFF);
								if(player > 0x100000) {
									RakNet::BitStream bitStream;

    								bitStream.Write((bool)false);
    								bitStream.Write((unsigned short)cheatClient->FindNearestPlayer());
    								bitStream.Write((float)1000.f);
    								bitStream.Write((int)9);
    								bitStream.Write((int)3);
									
									int playergivetakedamage = 115;
								
									log_3(gpeer, &playergivetakedamage, &bitStream, 1, 9, 0, ga7, ga8, 0, 0, -1, 0xFFFF, 0xFFFF, 0);
								}
							}
						}
					}
					cheatClient->PlayerCheat.Rage.Damager.last_shot = now_time;
				}
			
		}
#endif

#ifndef __aarch64__
		if(cheatClient->AutoFarm.BotMiner.state)
		{
			// Настройка маршрута
			if(!cheatClient->AutoFarm.BotMiner.haveStone)
			{
				cheatClient->AutoFarm.BotMiner.prev_point = cheatClient->AutoFarm.BotMiner.start_point;
			}
			else
			{
				cheatClient->AutoFarm.BotMiner.prev_point = cheatClient->AutoFarm.BotMiner.end_point;
			}
			
			*(float *)(localped + string2Offset(OBFUSCATE("0x384"))) = (((GetRadianAngleBetweenPoints( local_coors.x, local_coors.y, cheatClient->AutoFarm.BotMiner.prev_point.x, cheatClient->AutoFarm.BotMiner.prev_point.y) * (180.f / M_PI)) - 90.f) * M_PI) / 180.f;
			
			// Настройка анимаций
			if(!cheatClient->AutoFarm.BotMiner.goingToPoint && !cheatClient->AutoFarm.BotMiner.mining && !cheatClient->AutoFarm.BotMiner.haveStone)
			{
				// Сброс анимации
				*(int *)(localped + 592) = 1;
				*(int *)(localped + 596) = 0;
				*(int *)(localped + 600) = 1;
				*(int *)(localped + 612) = 0;
				*(int *)(localped + 616) = 0;
				SetMoveAnim(localped);
				
				// Запуск нужной анимации
				*(int *)(localped + 592) = 31;
				*(int *)(localped + 596) = 0;
				*(int *)(localped + 600) = 5;
				SetMoveAnim(localped);
				cheatClient->AutoFarm.BotMiner.goingToPoint = true;
			} else if(cheatClient->AutoFarm.BotMiner.goingToPoint && cheatClient->AutoFarm.BotMiner.mining) {
				// Сброс анимации
				*(int *)(localped + 592) = 1;
				*(int *)(localped + 596) = 0;
				*(int *)(localped + 600) = 1;
				*(int *)(localped + 612) = 0;
				*(int *)(localped + 616) = 0;
				SetMoveAnim(localped);
				cheatClient->AutoFarm.BotMiner.goingToPoint = false;
			} else if(!cheatClient->AutoFarm.BotMiner.goingToPoint && cheatClient->AutoFarm.BotMiner.haveStone) {
				if(cheatClient->timer->GetTimeInMilliseconds() - cheatClient->AutoFarm.BotMiner.takingStoneLastTime > 3000)
				{
					// Сброс анимации
					*(int *)(localped + 592) = 1;
					*(int *)(localped + 596) = 0;
					*(int *)(localped + 600) = 1;
					*(int *)(localped + 612) = 0;
					*(int *)(localped + 616) = 0;
					SetMoveAnim(localped);
				
					// Запуск нужной анимации
					*(int *)(localped + 592) = 31;
					*(int *)(localped + 596) = 0;
					*(int *)(localped + 600) = 4;
					SetMoveAnim(localped);
					cheatClient->AutoFarm.BotMiner.goingToPoint = true;
				}
			}
			
			// Детект: добывает ли бот руду
			if(localplayer.GetAnimationID() == 249)
			{
				cheatClient->AutoFarm.BotMiner.mining = true;
			}
			else
			{
				cheatClient->AutoFarm.BotMiner.mining = false;
			}
			
			// Детект: бот взял руду
			if(localplayer.GetAnimationID() == 273)
			{
				cheatClient->AutoFarm.BotMiner.takingStoneLastTime = cheatClient->timer->GetTimeInMilliseconds();
				cheatClient->AutoFarm.BotMiner.haveStone = true;
			}
			
			if(localplayer.GetAnimationID() == 274)
			{
				cheatClient->AutoFarm.BotMiner.haveStone = false;
				cheatClient->AutoFarm.BotMiner.goingToPoint = false;
			}
			
		}
#endif

		if(cheatClient->VehicleCheat.CarChanger.entered)
		{
			uintptr_t vehicle = localplayer.GetVehicle();
			if(vehicle)
			{
				CVehicle vehCls = CVehicle(vehicle);
				int vehicle_network_id = 0;
				cheatClient->VehicleCheat.CarChanger.prev_health = vehCls.GetHealth();
				int rw_object = *(int *)(vehicle + string2Offset(OBFUSCATE("0x44")));
				cheatClient->VehicleCheat.CarChanger.prev_angle = atan2f(*(float *)(rw_object + 16), *(float *)(rw_object + 20));
				
				for(int i = 0; i < 1500; i++)
				{
					uintptr_t veh = cheatClient->GetAtVehiclePool(i);
					if(veh == vehicle)
					{
						cheatClient->VehicleCheat.CarChanger.prev_network_id = i;
						break;
					}
				}
				
				// Выкидываем из транспорта
				if(localplayer.GetVehicle())
				{
					cheatClient->VehicleCheat.CarChanger.prev_vehicle = vehicle;
					Vec3 coors = {0,0,0};
					((void (*)(Vec3*))(libAddr + string2Offset(OBFUSCATE("0x2D7B21"))))(&coors);
					((void (*)(int,float,float,float))(libAddr + string2Offset(OBFUSCATE("0x38CA41"))))(localped, coors.x, coors.y, coors.z);
					cheatClient->VehicleCheat.CarChanger.removedFromVehicle = true;
				}
			}
			if(cheatClient->VehicleCheat.CarChanger.removedFromVehicle)
			{
				localplayer.Update();
				// Удаляем транспорт
				if(!localplayer.GetVehicle())
				{
					unsigned short* structure = new unsigned short[13];
					*(int *)(structure + 1) = std::stoi(cheatClient->VehicleCheat.CarChanger.numpad);
					*structure = cheatClient->VehicleCheat.CarChanger.prev_network_id;
					structure[11] = 65535;
					*((float *)structure + 6) = cheatClient->VehicleCheat.CarChanger.prev_health;
						
					*(int *)(structure + 3) = *(int *)(localped + string2Offset(OBFUSCATE("0x30")));
					*((int *)(structure + 3) + 1) = *(int *)(localped + string2Offset(OBFUSCATE("0x34")));
					*((int *)(structure + 3) + 2) = *(int *)(localped + string2Offset(OBFUSCATE("0x38")));
					
					CVehiclePool_New(cheatClient->GetVehiclePool(), structure);
					delete[] structure;
					
					// Выравниваем угол машины как было до этого
				((void (*)(int,float))(libAddr + string2Offset(OBFUSCATE("0x316C37"))))(cheatClient->VehicleCheat.CarChanger.prev_vehicle + 4, cheatClient->VehicleCheat.CarChanger.prev_angle);
						((void (*)(int))(libAddr + string2Offset(OBFUSCATE("0x3169C9"))))(cheatClient->VehicleCheat.CarChanger.prev_vehicle + 4);
						
					cheatClient->VehicleCheat.CarChanger.entered = false;
					cheatClient->VehicleCheat.CarChanger.numpad.clear();
					
					// Помещаем игрока в тачку
					((void (*)(int,int,int))(libAddr + string2Offset(OBFUSCATE("0x391085"))))(localped, cheatClient->GetAtVehiclePool(cheatClient->VehicleCheat.CarChanger.prev_network_id), 0);
					
					// Установка камеры на игрока
	#ifndef __aarch64__
				*(char *)(libAddr + 0x8264BA) = 0;
				((void (*)(int))(libAddr + 0x2A6B66 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x2AA6C8 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x5D9E1C + 1))(libAddr + 0x825AB8);
#endif

					cheatClient->VehicleCheat.CarChanger.removedFromVehicle = false;
				}
				else if(localplayer.GetVehicle())
				{
					CVehicle vehClass = CVehicle(localplayer.GetVehicle());
					if(vehClass.GetDriver() != localped)
					{
						unsigned short* structure = new unsigned short[13];
						*(int *)(structure + 1) = std::stoi(cheatClient->VehicleCheat.CarChanger.numpad);
						*structure = cheatClient->VehicleCheat.CarChanger.prev_network_id;
						structure[11] = 65535;
						*((float *)structure + 6) = cheatClient->VehicleCheat.CarChanger.prev_health;
							
						*(int *)(structure + 3) = *(int *)(localped + string2Offset(OBFUSCATE("0x30")));
						*((int *)(structure + 3) + 1) = *(int *)(localped + string2Offset(OBFUSCATE("0x34")));
						*((int *)(structure + 3) + 2) = *(int *)(localped + string2Offset(OBFUSCATE("0x38")));
						
						CVehiclePool_New(cheatClient->GetVehiclePool(), structure);
						delete[] structure;
						
						// Выравниваем угол машины как было до этого
						((void (*)(int,float))(libAddr + string2Offset(OBFUSCATE("0x316C37"))))(cheatClient->VehicleCheat.CarChanger.prev_vehicle + 4, cheatClient->VehicleCheat.CarChanger.prev_angle);
						((void (*)(int))(libAddr + string2Offset(OBFUSCATE("0x3169C9"))))(cheatClient->VehicleCheat.CarChanger.prev_vehicle + 4);
						
						cheatClient->VehicleCheat.CarChanger.entered = false;
						cheatClient->VehicleCheat.CarChanger.numpad.clear();
						
						// Помещаем игрока в тачку
						((void (*)(int,int,int))(libAddr + string2Offset(OBFUSCATE("0x391085"))))(localped, cheatClient->GetAtVehiclePool(cheatClient->VehicleCheat.CarChanger.prev_network_id), 0);
						
						// Установка камеры на игрока
		#ifndef __aarch64__
				*(char *)(libAddr + 0x8264BA) = 0;
				((void (*)(int))(libAddr + 0x2A6B66 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x2AA6C8 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x5D9E1C + 1))(libAddr + 0x825AB8);
#endif

						cheatClient->VehicleCheat.CarChanger.removedFromVehicle = false;
					}
				}
			}
		}

		if(cheatClient->VehicleCheat.AntiDetachTrailer.state)
		{
			uintptr_t truck = localplayer.GetVehicle();
			if(truck)
			{
				CVehicle _truck = CVehicle(truck);
				if(_truck.GetDriver() == localped)
				{
					if(*(int *)(truck + 696) != TRAILER)
					{
						uintptr_t trailer = *(uintptr_t *)(truck + string2Offset(OBFUSCATE("0x21C")));
						if(trailer)
						{
							cheatClient->VehicleCheat.AntiDetachTrailer.trailer = trailer;
						}
						else if(!trailer && cheatClient->VehicleCheat.AntiDetachTrailer.trailer)
						{
							int rw_object = *(int *)(truck + string2Offset(OBFUSCATE("0x44")));
							float truck_angle = atan2f(*(float *)(rw_object + 16), *(float *)(rw_object + 20));
							
							//((void (*)(int,float))(libAddr + 0x1AA01A + 1))(cheatClient->VehicleCheat.AntiDetachTrailer.trailer + 4, truck_angle);
							//((void (*)(int))(libAddr + 0x1A9EEE + 1))(cheatClient->VehicleCheat.AntiDetachTrailer.trailer + 4);
					
							((void (*)(int,int,int))(libAddr + string2Offset(OBFUSCATE("0x2BA95F")) + 1))(cheatClient->VehicleCheat.AntiDetachTrailer.trailer, truck, 1);
						}
					}
				}
			}
		}
		
		if(cheatClient->Wallhack.Player.skeleton)
		{
#ifdef __aarch64__
			// arm64: no GetBonePosition export — walk the RwFrame (bone) tree.
			// chain: ped -> RpClump -> RpAtomic -> root RwFrame -> child/next
			// all offsets are discovered at runtime on the local ped, then reused.
			static int skClumpOff = -1, skLinkOff = -1, skMatOff = -1;
			static int skChildOff = -1, skNextOff = -1, skDiscN = 0, skDk = 0;
			auto heapq = [](uintptr_t a) -> uintptr_t {
				uintptr_t q = 0;
				if(!SafeRead(a, &q, 8)) return 0;
				q &= 0x00FFFFFFFFFFFFFFULL;
				return (q >= 0x1000000000ULL && q < 0xFF0000000000ULL) ? q : 0;
			};
			if(skClumpOff < 0 && skDiscN++ < 200)
			{
				uintptr_t lp = cheatClient->FindLocalPlayer();
				Vec3 lpPos = lp ? cheatClient->GetPos(lp) : Vec3(0,0,0);
				if(lp && lpPos.x != 0.f)
				{
					// find clump: heap ptr whose atomicList.next (+0x10) reaches a
					// struct that points back to it — that IS the RpClump.
					for(int off = 0x8; off < 0x180 && skClumpOff < 0; off += 8)
					{
						uintptr_t clump = heapq(lp + off);
						if(!clump) continue;
						uintptr_t linkNext = heapq(clump + 0x10);
						if(!linkNext) continue;
						// atomic base = linkNext - inClumpLinkOff; find the backref
						for(int lnk = 0x18; lnk <= 0x50; lnk += 8)
						{
							uintptr_t atomic = linkNext - lnk;
							bool back = false;
							for(int bo = 0x10; bo <= 0x60; bo += 8)
								if(heapq(atomic + bo) == clump) { back = true; break; }
							if(!back) continue;
							// lFrame.next is either the frame or a link node inside it —
							// try it raw and minus small deltas
							uintptr_t fv = heapq(atomic + 0x10);
							if(!fv) continue;
							if(skDiscN < 4)
								__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
									"SKEL cand ped+%x clump=%p link=%p atomic=%p fv=%p",
									off, (void*)clump, (void*)linkNext, (void*)atomic, (void*)fv);
							for(int dk = 0; dk <= 0xC0 && skClumpOff < 0; dk += 0x10)
							{
								uintptr_t frame = fv - dk;
								for(int mo = 0x10; mo <= 0xC0; mo += 8)
								{
									float lt[3];
									if(!SafeRead(frame + mo + 0x40 + 0x30, lt, 12)) continue;
									if(fabsf(lt[0]-lpPos.x) < 3.f && fabsf(lt[1]-lpPos.y) < 3.f && fabsf(lt[2]-lpPos.z) < 3.f)
									{
										// child: c->parent == frame. `next` = sibling link —
										// sibling s has s->parent == f->parent. The root's first
										// child may have no siblings, so descend the first-child
										// chain until a branching node appears.
										int cOff = -1, nOff = -1;
										uintptr_t childF = 0;
										for(int co = 0x80; co <= 0xF0; co += 8)
										{
											uintptr_t c = heapq(frame + co);
											if(!c) continue;
											uintptr_t par = heapq(c + 8);
											if(par == frame) { cOff = co; childF = c; break; }
										}
										if(childF)
										{
											uintptr_t cur = childF, par = frame;
											for(int d = 0; d < 8 && nOff < 0 && cur; d++)
											{
												for(int co2 = 0x80; co2 <= 0xF0; co2 += 8)
												{
													uintptr_t s = heapq(cur + co2);
													if(!s || s == cur) continue;
													uintptr_t sp = heapq(s + 8);
													if(sp == par) { nOff = co2; break; }
												}
												uintptr_t nxt = heapq(cur + cOff);
												par = cur; cur = nxt;
											}
										}
										if(cOff >= 0)
										{
											skClumpOff = off; skLinkOff = lnk; skMatOff = mo;
											skChildOff = cOff; skNextOff = nOff; skDk = dk;
											__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
												"SKEL found clump=ped+%x lnk=%x frame=%p dk=%x mat=%x child=%x next=%x",
												off, lnk, (void*)frame, dk, mo, cOff, nOff);
										}
									}
									if(skClumpOff >= 0) break;
								}
							}
							if(skClumpOff >= 0) break;
						}
					}
					if(skDiscN == 200)
						__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "SKEL discovery failed");
				}
			}
			// frame tree only stores the bind pose — the live animation lives in
			// RpHAnimHierarchy.pMatrixArray. Find a pointer to the hierarchy in
			// clump/atomic/ped fields: u32@+4 = numNodes, +0x10 = matrices,
			// +0x20 = nodeInfo array (node.pFrame -> bone frame).
			static int skHAnimOff = -1, skHAnimHost = -1, skHaN = 0;
			static int skNiOff = -1, skNiStride = 0x18, skNiFrame = 0x10, skMatMode = -1;
			if(skClumpOff >= 0 && skHAnimOff < 0 && skHaN++ < 1200)
			{
				uintptr_t lp = cheatClient->FindLocalPlayer();
				Vec3 lpPos = lp ? cheatClient->GetPos(lp) : Vec3(0,0,0);
				if(lp && lpPos.x != 0.f)
				{
					uintptr_t lclump = heapq(lp + skClumpOff);
					uintptr_t latomic = 0;
					if(lclump) { uintptr_t ln = heapq(lclump + 0x10); if(ln) latomic = ln - skLinkOff; }
					uintptr_t hosts[3] = {lclump, latomic, lp};
					if(skHaN % 300 == 100 && latomic)
					{
						uintptr_t Hp = heapq(latomic + 0x88);
						int Np = 0; if(Hp) SafeRead(Hp + 4, &Np, 4);
						uintptr_t NIp = Hp ? heapq(Hp + 0x20) : 0;
						__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
							"SKEL probe atomic+88 H=%p n=%d ma=%p ni=%p",
							(void*)Hp, Np, (void*)(Hp ? heapq(Hp + 0x10) : 0), (void*)NIp);
						if(Hp)
						{
							uintptr_t hq[12] = {0};
							SafeRead(Hp, hq, 96);
							__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
								"SKEL hraw: %p %p %p %p %p %p %p %p %p %p %p %p",
								(void*)hq[0], (void*)hq[1], (void*)hq[2], (void*)hq[3],
								(void*)hq[4], (void*)hq[5], (void*)hq[6], (void*)hq[7],
								(void*)hq[8], (void*)hq[9], (void*)hq[10], (void*)hq[11]);
						}
						if(NIp)
						{
							for(int di = 0; di < 8; di += 8)
							{
								uintptr_t q[8] = {0};
								SafeRead(NIp + di * 8, q, 64);
								__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
									"SKEL nraw +%02x: %p %p %p %p %p %p %p %p",
									di * 8, (void*)q[0], (void*)q[1], (void*)q[2], (void*)q[3],
									(void*)q[4], (void*)q[5], (void*)q[6], (void*)q[7]);
							}
						}
						uintptr_t N18 = Hp ? heapq(Hp + 0x18) : 0;
						if(N18)
						{
							for(int di = 0; di < 12; di += 8)
							{
								uintptr_t q[8] = {0};
								SafeRead(N18 + di * 8, q, 64);
								__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
									"SKEL ni18 +%02x: %p %p %p %p %p %p %p %p",
									di * 8, (void*)q[0], (void*)q[1], (void*)q[2], (void*)q[3],
									(void*)q[4], (void*)q[5], (void*)q[6], (void*)q[7]);
							}
						}
					}
					int lo[3] = {0x40, 0x40, 0x100}, hi[3] = {0x160, 0x160, 0x400};
					for(int h = 0; h < 3 && skHAnimOff < 0; h++)
					{
						if(!hosts[h]) continue;
						for(int off = lo[h]; off < hi[h] && skHAnimOff < 0; off += 8)
						{
							uintptr_t H = heapq(hosts[h] + off);
							if(!H) continue;
							int N = 0;
							if(!SafeRead(H + 4, &N, 4) || N < 10 || N > 300) continue;
							uintptr_t MA = heapq(H + 0x10);
							if(!MA) continue;
							float m0[3];
							if(!SafeRead(MA + 0x30, m0, 12)) continue;
							float d0 = (m0[0]-lpPos.x)*(m0[0]-lpPos.x) + (m0[1]-lpPos.y)*(m0[1]-lpPos.y) + (m0[2]-lpPos.z)*(m0[2]-lpPos.z);
							int mode = 0;
							if(d0 > 9.f)
							{
								uintptr_t pm = cheatClient->GetMatrix(lp);
								if(!pm) continue;
								float rm[16];
								if(!SafeRead(pm, rm, 64)) continue;
								float wx = rm[0]*m0[0] + rm[4]*m0[1] + rm[8]*m0[2] + rm[12];
								float wy = rm[1]*m0[0] + rm[5]*m0[1] + rm[9]*m0[2] + rm[13];
								float wz = rm[2]*m0[0] + rm[6]*m0[1] + rm[10]*m0[2] + rm[14];
								float d1 = (wx-lpPos.x)*(wx-lpPos.x) + (wy-lpPos.y)*(wy-lpPos.y) + (wz-lpPos.z)*(wz-lpPos.z);
								if(d1 < 9.f) mode = 1;
								else
								{
									__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
										"SKEL hanim reject host=%d off=%x n=%d m0=%.2f,%.2f,%.2f w=%.2f,%.2f,%.2f lp=%.2f,%.2f,%.2f",
										h, off, N, m0[0], m0[1], m0[2], wx, wy, wz, lpPos.x, lpPos.y, lpPos.z);
									continue;
								}
							}
							skHAnimHost = h; skHAnimOff = off; skMatMode = mode;
							__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
								"SKEL hanim host=%d off=%x n=%d ma=%p mode=%d m0=%.2f,%.2f,%.2f",
								h, off, N, (void*)MA, mode, m0[0], m0[1], m0[2]);
						}
					}
					if(skHaN == 1200)
						__android_log_print(ANDROID_LOG_INFO, "PEDDBG", "SKEL hanim not found");
				}
			}
			if(skClumpOff >= 0)
			{
				for(int a = -1; a < 1500; a++)
				{
					uintptr_t player;
					if(a < 0)
					{
						player = cheatClient->FindLocalPlayer();
						if(!player) continue;
					}
					else
					{
						player = cheatClient->GetAtPlayerPool(a);
						if(!player) continue;
						if(!SafeRead(player + PLAYER_PED_OFF, &player, 8) || !(player & 0x00FFFFFFFFFFFFFFULL)) continue;
						player &= 0x00FFFFFFFFFFFFFFULL;
					}
					uintptr_t clump = heapq(player + skClumpOff);
					if(!clump) continue;
					if(skHAnimOff >= 0)
					{
						uintptr_t host = clump;
						if(skHAnimHost == 1) { uintptr_t ln = heapq(clump + 0x10); host = ln ? ln - skLinkOff : 0; }
						else if(skHAnimHost == 2) host = player;
						uintptr_t H = host ? heapq(host + skHAnimOff) : 0;
						int N = 0;
						uintptr_t MA = (H && SafeRead(H + 4, &N, 4) && N > 0 && N <= 128) ? heapq(H + 0x10) : 0;
						if(MA)
						{
							uintptr_t linkNext = heapq(clump + 0x10);
							uintptr_t frame = linkNext ? heapq(linkNext - skLinkOff + 0x10) : 0;
							if(frame)
							{
								frame -= skDk;
								for(int up = 0; up < 64; up++)
								{
									uintptr_t par = heapq(frame + 8);
									if(!par || par == frame) break;
									frame = par;
								}
								float rm[16]; bool haveRm = false;
								if(skMatMode == 1)
								{
									uintptr_t pm = cheatClient->GetMatrix(player);
									if(pm && SafeRead(pm, rm, 64)) haveRm = true;
								}
								Vec3 pp = cheatClient->GetPos(player);
								// DFS pre-order over the frame tree. Only NAMED
								// frames are hierarchy nodes — unnamed attach
								// frames must not consume a matrix index.
								struct SE { uintptr_t f; int parIdx; };
								SE st[256]; int sp = 0, midx = 0;
								st[sp++] = {frame, -1};
								static bool skNamed = false;
								bool dbgNames = (a < 0) && !skNamed;
								static const char* bl[] = {"Finger","Toe","Jaw","Brow","breast","Belly"};
								while(sp > 0 && midx < 200)
								{
									SE cur = st[--sp];
									char nm[24] = {0};
									SafeRead(cur.f + 0xD8, nm, 16);
									bool named = nm[0] != 0;
									int cidx = named ? midx++ : -1;
									bool drawOk = named;
									if(drawOk)
									{
										for(size_t bi = 0; bi < sizeof(bl)/sizeof(bl[0]); bi++)
											if(strstr(nm, bl[bi])) { drawOk = false; break; }
									}
									// children link to nearest drawable ancestor —
									// unnamed/blacklisted frames pass through
									int parForKids = drawOk ? cidx : cur.parIdx;
									if(dbgNames)
									{
										__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
											"SKEL bone idx=%d par=%d f=%p name=%s",
											cidx, cur.parIdx, (void*)cur.f, nm);
									}
									if(drawOk && cur.parIdx >= 0 && cidx < N && cur.parIdx < N)
									{
										float p1[3], p2[3];
										if(SafeRead(MA + cidx * 0x40 + 0x30, p1, 12) &&
										   SafeRead(MA + cur.parIdx * 0x40 + 0x30, p2, 12))
										{
											float x1 = p1[0], y1 = p1[1], z1 = p1[2];
											float x2 = p2[0], y2 = p2[1], z2 = p2[2];
											if(haveRm)
											{
												x1 = rm[0]*p1[0] + rm[4]*p1[1] + rm[8]*p1[2] + rm[12];
												y1 = rm[1]*p1[0] + rm[5]*p1[1] + rm[9]*p1[2] + rm[13];
												z1 = rm[2]*p1[0] + rm[6]*p1[1] + rm[10]*p1[2] + rm[14];
												x2 = rm[0]*p2[0] + rm[4]*p2[1] + rm[8]*p2[2] + rm[12];
												y2 = rm[1]*p2[0] + rm[5]*p2[1] + rm[9]*p2[2] + rm[13];
												z2 = rm[2]*p2[0] + rm[6]*p2[1] + rm[10]*p2[2] + rm[14];
											}
											float dp = (x1-pp.x)*(x1-pp.x) + (y1-pp.y)*(y1-pp.y) + (z1-pp.z)*(z1-pp.z);
											float dd = (x1-x2)*(x1-x2) + (y1-y2)*(y1-y2) + (z1-z2)*(z1-z2);
											if(dp < 9.f && dd > 0.0001f && dd < 2.25f)
											{
												Vec3 w1 = {x1,y1,z1}, w2 = {x2,y2,z2};
												Vec3 s1 = {0,0,0}, s2 = {0,0,0};
												if(WorldToScreen(&w1, &s1) && s1.z >= 1.f &&
												   WorldToScreen(&w2, &s2) && s2.z >= 1.f)
												{
													DrawBuffer de = {ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y)};
													cheatClient->Wallhack.Player.Config.Skeleton.buffer.push_back(de);
												}
											}
										}
									}
									// push sibling first so child pops first (pre-order)
									if(skNextOff >= 0) { uintptr_t n = heapq(cur.f + skNextOff); if(n && n != cur.f && sp < 255) st[sp++] = {n, cur.parIdx}; }
									if(skChildOff >= 0) { uintptr_t c = heapq(cur.f + skChildOff); if(c && c != cur.f && sp < 255) st[sp++] = {c, parForKids}; }
								}
								if(dbgNames) skNamed = true;
							}
							continue;
						}
					}
					uintptr_t linkNext = heapq(clump + 0x10);
					if(!linkNext) continue;
					uintptr_t frame = heapq(linkNext - skLinkOff + 0x10);
					if(!frame) continue;
					frame -= skDk;   // lFrame.next may point inside the frame
					{
						static int skWalkDbg = 0;
						if(skWalkDbg++ < 5)
							__android_log_print(ANDROID_LOG_INFO, "PEDDBG",
								"SKEL walk ped=%p clump=%p frame=%p", (void*)player, (void*)clump, (void*)frame);
					}
					// climb to the root frame, then walk the whole tree
					for(int up = 0; up < 64; up++)
					{
						uintptr_t par = heapq(frame + 8);
						if(!par || par == frame) break;
						frame = par;
					}
					// anchor the tree to the ped's live pos — remote ped bones
					// update on a different path and lag/offset from entity pos
					float anch[3] = {0,0,0};
					{
						float rp[3];
						Vec3 pp = cheatClient->GetPos(player);
						if(SafeRead(frame + skMatOff + 0x40 + 0x30, rp, 12))
						{
							anch[0] = pp.x - rp[0]; anch[1] = pp.y - rp[1]; anch[2] = pp.z - rp[2];
						}
					}
					uintptr_t stack[256]; int sp = 0;
					stack[sp++] = frame;
					int nodes = 0;
					while(sp > 0 && nodes < 200)
					{
						uintptr_t f = stack[--sp]; nodes++;
						uintptr_t par = heapq(f + 8);
						if(par && par != f)
						{
							float p1[3], p2[3];
							if(SafeRead(f + skMatOff + 0x40 + 0x30, p1, 12) &&
							   SafeRead(par + skMatOff + 0x40 + 0x30, p2, 12))
							{
								Vec3 w1 = {p1[0]+anch[0], p1[1]+anch[1], p1[2]+anch[2]};
								Vec3 w2 = {p2[0]+anch[0], p2[1]+anch[1], p2[2]+anch[2]};
								float dd = (w1.x-w2.x)*(w1.x-w2.x) + (w1.y-w2.y)*(w1.y-w2.y) + (w1.z-w2.z)*(w1.z-w2.z);
								if(dd > 0.0001f && dd < 4.f)   // skip degenerate/huge links
								{
									Vec3 s1 = {0,0,0}, s2 = {0,0,0};
									if(WorldToScreen(&w1, &s1) && s1.z >= 1.f &&
									   WorldToScreen(&w2, &s2) && s2.z >= 1.f)
									{
										DrawBuffer de = {ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y)};
										cheatClient->Wallhack.Player.Config.Skeleton.buffer.push_back(de);
									}
								}
							}
						}
						if(skChildOff >= 0) { uintptr_t c = heapq(f + skChildOff); if(c && sp < 255) stack[sp++] = c; }
						if(skNextOff >= 0)  { uintptr_t n = heapq(f + skNextOff);  if(n && sp < 255) stack[sp++] = n; }
					}
				}
			}
#endif
			if(cheatClient->Wallhack.Player.Config.Skeleton.rendered)
			{
				for(int a = 0; a < 1500; a++)
				{
					uintptr_t player = cheatClient->GetAtPlayerPool(a);
					if(player)
					{
						player = *(uintptr_t *)(player + PLAYER_PED_OFF);
						if(player > 0x100000)
						{
							Vec3 worldPos1 = {0,0,0};
							Vec3 worldPos2 = {0,0,0};
					
							Vec3 screenPos1 = {0,0,0};
							Vec3 screenPos2 = {0,0,0};
					
							int t[14] = { 3, 4, 5, 51, 52, 41, 42, 31, 32, 33, 21, 22, 23, 2 };
                    		for (int i = 0; i < 14; i++)
                    		{
                       			int bone1 = GetBonePosition(player, t[i]);
                        		int bone2 = GetBonePosition(player, t[i] + 1);
								
								if(bone1 && bone2)
								{
									worldPos1 = {*(float *)(bone1 + 48), *(float *)(bone1 + 52), *(float *)(bone1 + 56)};
									worldPos2 = {*(float *)(bone2 + 48), *(float *)(bone2 + 52), *(float *)(bone2 + 56)};
									
									if(WorldToScreen(&worldPos1, &screenPos1))
									{
										if(screenPos1.z < 1.0f) continue;
										if(WorldToScreen(&worldPos2, &screenPos2))
										{
											if(screenPos2.z < 1.0f) continue;
											DrawBuffer draw_element = {ImVec2(screenPos1.x, screenPos1.y), ImVec2(screenPos2.x, screenPos2.y)};
											cheatClient->Wallhack.Player.Config.Skeleton.buffer.push_back(draw_element);
											//AddChatMessage("added element");
											//ImGui::GetBackgroundDrawList()->AddLine(ImVec2(screenPos1.x,screenPos1.y), ImVec2(screenPos2.x,screenPos2.y), ImGui::ColorConvertFloat4ToU32(cheatClient->Wallhack.Player.Config.Skeleton.color), cheatClient->Wallhack.Player.Config.Skeleton.tickness);
										}
									}
								}
							}
					
							for (int i = 4; i < 6; i++)
                    		{
                        		int bone = GetBonePosition(player, i * 10 + 1);
								
								if(bone)
								{
									worldPos2 = {*(float *)(bone + 48), *(float *)(bone + 52), *(float *)(bone + 56)};
									if(WorldToScreen(&worldPos2, &screenPos2))
									{
										if(screenPos2.z < 1.0f) continue;
										DrawBuffer draw_element = {ImVec2(screenPos1.x, screenPos1.y), ImVec2(screenPos2.x, screenPos2.y)};
										cheatClient->Wallhack.Player.Config.Skeleton.buffer.push_back(draw_element);
										//AddChatMessage("added element 2");
									}
								}
                        	}
						}
					}
				}
				cheatClient->Wallhack.Player.Config.Skeleton.rendered = false;
			}
		}
		
		if(cheatClient->Collision.ped)
		{
			for(int i = 0; i < 1500; i++)
			{
				uintptr_t player = cheatClient->GetAtPlayerPool(i);
				if(player)
				{
					player = *(uintptr_t *)(player + PLAYER_PED_OFF);
					if(player > 0x100000)
					{
						CPed _player = CPed(player);
						if(_player.InVehicle()) { continue; }
						
						Vec3 local_coors = cheatClient->GetPos(localped);
						Vec3 ped_coors = cheatClient->GetPos(player);
						
						float dist = Get2dDistance(local_coors, ped_coors);
						
						float radius = 0.8f;
						
						if(localplayer.InVehicle())
						{
							int colmodel = *(int *)(*(int *)(libAddr + string2Offset(OBFUSCATE("0x496F61C")) + (4 * *(unsigned short *)(localplayer.GetVehicle() + string2Offset(OBFUSCATE("0x5A"))))) + 28);
							if(colmodel)
							{
								float forward_width = (float)(*(float *)(colmodel + 32) - *(float *)(colmodel + 20));
								dist = Get2dDistance(cheatClient->GetPos(localplayer.GetVehicle()), ped_coors);
								if(dist < forward_width)
								{
									int number = *(int *)(player + 80);
									number &= ~string2Offset(OBFUSCATE("0x100"));
									*(int *)(player + 80) = number;
								}
								else
								{
									int number = *(int *)(player + 80);
									number |= string2Offset(OBFUSCATE("0x100"));
									*(int *)(player + 80) = number;
								}
							}
						}
						else
						{
							if(dist < radius)
							{
								int number = *(int *)(player + 80);
								number &= ~string2Offset(OBFUSCATE("0x100"));
								*(int *)(player + 80) = number;
							}
							else
							{
								int number = *(int *)(player + 80);
								number |= string2Offset(OBFUSCATE("0x100"));
								*(int *)(player + 80) = number;
							}
						}
					}
				}
			}
		}
		else
		{
			for(int i = 0; i < 1500; i++)
			{
				uintptr_t player = cheatClient->GetAtPlayerPool(i);
				if(player)
				{
					player = *(uintptr_t *)(player + PLAYER_PED_OFF);
					if(player > 0x100000)
					{
						CPed _player = CPed(player);
						if(_player.InVehicle()) { 
							int number = *(int *)(player + 80);
							number &= ~string2Offset(OBFUSCATE("0x100"));
							*(int *)(player + 80) = number;
						}
						else
						{
							int number = *(int *)(player + 80);
							number |= string2Offset(OBFUSCATE("0x100"));
							*(int *)(player + 80) = number;
						}
					}
				}
			}
		}
		
		if(cheatClient->Collision.vehicle)
		{
			for(int i = 0; i < 1500; i++)
			{
				uintptr_t vehicle = cheatClient->GetAtVehiclePool(i);
				if(vehicle)
				{
					if(vehicle == localplayer.GetVehicle()) {
						int number = *(int *)(vehicle + 80);
						number |= string2Offset(OBFUSCATE("0x100"));
						*(int *)(vehicle + 80) = number;
						continue;
					}
					
					Vec3 local_coors = cheatClient->GetPos(localped);
					Vec3 veh_coors = cheatClient->GetPos(vehicle);
						
					float dist = Get2dDistance(local_coors, veh_coors);
					
					if(localplayer.InVehicle())
					{
						int local_colmodel = *(int *)(*(int *)(libAddr + string2Offset(OBFUSCATE("0x496F61C")) + (4 * *(unsigned short *)(localplayer.GetVehicle() + string2Offset(OBFUSCATE("0x5A"))))) + 28);
						int target_colmodel = *(int *)(*(int *)(libAddr + string2Offset(OBFUSCATE("0x496F61C")) + (4 * *(unsigned short *)(vehicle + string2Offset(OBFUSCATE("0x5A"))))) + 28);
						
						if(local_colmodel && target_colmodel)
						{
							float my_forward_width = (float)(*(float *)(local_colmodel + 32) - *(float *)(local_colmodel + 20));
							float target_forward_width = (float)(*(float *)(target_colmodel + 32) - *(float *)(target_colmodel + 20));
							
							dist = Get2dDistance(cheatClient->GetPos(localplayer.GetVehicle()), veh_coors);
							if(dist < my_forward_width + target_forward_width)
							{
								int number = *(int *)(vehicle + 80);
								number &= ~string2Offset(OBFUSCATE("0x100"));
								*(int *)(vehicle + 80) = number;
							}
							else
							{
								int number = *(int *)(vehicle + 80);
								number |= string2Offset(OBFUSCATE("0x100"));
								*(int *)(vehicle + 80) = number;
							}
						}
					}
					else
					{
						int target_colmodel = *(int *)(*(int *)(libAddr + string2Offset(OBFUSCATE("0x496F61C")) + (4 * *(unsigned short *)(vehicle + string2Offset(OBFUSCATE("0x5A"))))) + 28);
						
						if(target_colmodel)
						{
							float target_forward_width = (float)(*(float *)(target_colmodel + 32) - *(float *)(target_colmodel + 20));
							
							if(dist < target_forward_width + 0.8f)
							{
								int number = *(int *)(vehicle + 80);
								number &= ~string2Offset(OBFUSCATE("0x100"));
								*(int *)(vehicle + 80) = number;
							}
							else
							{
								int number = *(int *)(vehicle + 80);
								number |= string2Offset(OBFUSCATE("0x100"));
								*(int *)(vehicle + 80) = number;
							}
						}
					}
				}
			}
		}
		else
		{
			for(int i = 0; i < 1500; i++)
			{
				uintptr_t vehicle = cheatClient->GetAtVehiclePool(i);
				if(vehicle)
				{
					int number = *(int *)(vehicle + 80);
					number |= string2Offset(OBFUSCATE("0x100"));
					*(int *)(vehicle + 80) = number;
				}
			}
		}
		
		if(cheatClient->CarSpawn.entered)
		{
			uintptr_t vehpool = cheatClient->GetVehiclePool();
			uintptr_t localped = cheatClient->FindLocalPlayer();
						
			if(vehpool)
			{
					unsigned short* structure = new unsigned short[13];
					*(int *)(structure + 1) = std::stoi(cheatClient->CarSpawn.numpad);
					*structure = 999;
					structure[11] = 65535;
					*((float *)structure + 6) = 1000.f;
								
					*(int *)(structure + 3) = *(int *)(localped + string2Offset(OBFUSCATE("0x30")));
					*((int *)(structure + 3) + 1) = *(int *)(localped + string2Offset(OBFUSCATE("0x34")));
					*((int *)(structure + 3) + 2) = *(int *)(localped + string2Offset(OBFUSCATE("0x38")));
						
					CVehiclePool_New(vehpool, structure);
					delete[] structure;
								
					int vehicle = CVehiclePool_GetAt(cheatClient->GetVehiclePool(), 999);
					if(vehicle)
					{
						// sub_28D808
						/**(char *)(vehicle + 0x7A8) = -1;
						*(int *)(vehicle + 0x7A4) = 0;
						*(int *)(vehicle + 0x794) = -16777216;
						*(int *)(vehicle + 0x798) = 0;
						*(int *)(vehicle + 0x79C) = 0;
						*(int *)(vehicle + 0x7A0) = 0;
						*/
						((void (*) (int,int,int,int,int))(libAddr + string2Offset(OBFUSCATE("0x45D485"))))(vehicle, -16777216, 0, 0, 0);
						// цвет кузова
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x7D8"))) = -1;
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x7D9"))) = 0;
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x7D8"))) = 0;
								
						// цвет дисков
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x7E0"))) = 0;
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x7E1"))) = -1;
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x7E2"))) = 0;
						
						*(float *)(vehicle + string2Offset(OBFUSCATE("0x30"))) = *(float *)(localped + string2Offset(OBFUSCATE("0x30")));
						*(float *)(vehicle + string2Offset(OBFUSCATE("0x34"))) = *(float *)(localped + string2Offset(OBFUSCATE("0x34")));
						*(float *)(vehicle + string2Offset(OBFUSCATE("0x38"))) = *(float *)(localped + string2Offset(OBFUSCATE("0x38")));
						
						// unknown
						/*int v13 = *(int *)(vehicle + 0x12C);
								
						// общий клиренс
						*(float *)(vehicle + 0x778) = 1.f;
								
						// дальний свет
						*(char *)(vehicle + 0x2BF) = 0;
						
						// отдельный клиренс
						*(float *)(vehicle + 0x77C) = 1.f;
								
						// цвет фар
						*(char *)(vehicle + 0x7AC) = 0;
						*(char *)(vehicle + 0x7AD) = -1;
						*(char *)(vehicle + 0x7AE) = 0;
								
						// unused
						*(int *)(vehicle + 0x790) = 0.005f;
								
						*(char *)(vehicle + 0x774) = 0;
						*(char *)(vehicle + 0x78C) = 0;
						*(float *)(vehicle + 0x780) = 0;
						*(float *)(vehicle + 0x784) = 0.0004f;
						*(int *)(vehicle + 0x788) = -16777216;
						*(float *)(vehicle + 0x7C0) = 1.f;
						*(float *)(vehicle + 0x7C4) = 1.f;
						*(int *)(vehicle + 0x7B8) = -16777216;
						*(int *)(vehicle + 0x7BC) = -16777216;
						*(int *)(vehicle + 0x7B0) = 0;
						*(int *)(vehicle + 0x7C8) = 0;
								
						*(int *)(vehicle + 0x7E8) = 0;
						*(int *)(vehicle + 0x7EC) = 0;
						*(int *)(vehicle + 0x7F0) = 0;
								
						*(char *)(vehicle + 0x75C) = 0;
						*(short *)(vehicle + 0x75E) = 0;
								
						*(char *)(vehicle + 0x814) = 0;
						*(char *)(vehicle + 0x7CC) = 0;
						*/
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x20D"))) = 16;
						*(char *)(vehicle + string2Offset(OBFUSCATE("0x212"))) = 1;
						
						cheatClient->CarSpawn.entered = false;
						cheatClient->CarSpawn.numpad.clear();
				}
			}
		}
		
		if(cheatClient->VisualCheat.SkinChanger.entered)
		{
			int v6 = 1001;
			int skinID = std::stoi(cheatClient->VisualCheat.SkinChanger.numpad);
			int localped = cheatClient->FindLocalPlayer();
			
			if(localped)
			{
				if(((int *)(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x496F61C"))))[skinID])
				{
					if(*((unsigned char*)(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x459E298")) + 28 * skinID + 8)) == 1)
					{
						(( void (*)(int))(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x20CDA7"))))(localped);
						*(int *)(localped + 180) = -1;
						(( void (*)(int, int))(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x20CC59"))))(localped, skinID);
						cheatClient->VisualCheat.SkinChanger.entered = false;
						cheatClient->VisualCheat.SkinChanger.numpad.clear();
					}
					else
					{
						// Загружаем модель скина
						// в отдельном потоке
						while(true)
						{
							(( void (*)(int, int))(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x2E712D"))))(skinID, 8);
							(( void (*)(int))(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x2E99FD"))))(1);
							v6 -= 1;
							if(!v6)
							{
								break;
							}
							if(*((unsigned char*)(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x459E298")) + 28 * skinID + 8)) == 1)
							{
								(( void (*)(int))(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x20CDA7"))))(localped);
								*(int *)(localped + 180) = -1;
								(( void (*)(int, int))(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x20CC59"))))(localped, skinID);
								cheatClient->VisualCheat.SkinChanger.entered = false;
								cheatClient->VisualCheat.SkinChanger.numpad.clear();
								break;
							}
						}
					}
				}
				else
				{
					ChatMsg(OBFUSCATE("{ff5252}[ONESHOT] |{ffffff}Скин не найден."));
				}
			}
		}
		
		if(cheatClient->VisualCheat.MoneyChanger.entered)
		{
			int localped;
			int* result;
	
			localped = cheatClient->FindLocalPlayer();
			result = (( int* (*)(int))(cheatClient->GetLibAddr() + 0x3B0A0C + 1))(localped);
			result[20] += std::stoi(cheatClient->VisualCheat.MoneyChanger.numpad);
			char buff[32];
			sprintf(buff, OBFUSCATE("Вы получили %d рублей"), std::stoi(cheatClient->VisualCheat.MoneyChanger.numpad));
			std::string str(buff);
			BrNotification(TYPE_MONEY_GREEN, str.c_str(), 3);
			
			cheatClient->VisualCheat.MoneyChanger.entered = false;
			cheatClient->VisualCheat.MoneyChanger.numpad.clear();
		}
		
		if(cheatClient->Teleport.toMarker)
		{
			// InCar
			if(localplayer.InVehicle())
			{
				CVehicle vehicle(localplayer.GetVehicle());
				if(vehicle.GetDriver() == localped)
				{
					// InCar
					// Установка камеры на игрока
	#ifndef __aarch64__
				*(char *)(libAddr + 0x8264BA) = 0;
				((void (*)(int))(libAddr + 0x2A6B66 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x2AA6C8 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x5D9E1C + 1))(libAddr + 0x825AB8);
#endif

					// Направляем игрока к маркеру
					Vec3 local_coors = cheatClient->GetPos(localplayer.GetVehicle());
					Vec2 marker_coors = cheatClient->GetMarkerCoors();
					*(float *)(localplayer.GetVehicle() + string2Offset(OBFUSCATE("0x38"))) += 0.5f;

					Vec3 cam_coors = cheatClient->GetCameraCoors();
					
					float warpX = (marker_coors.x - local_coors.x);
					float warpY = (marker_coors.y - local_coors.y);
					float length_vector = sqrt(warpX * warpX + warpY * warpY);
					warpX = warpX / length_vector;
					warpY = warpY / length_vector;
					float warpZ = local_coors.z - cam_coors.z;
					cheatClient->Teleport.InCar.speed += 0.1f;
					if(cheatClient->Teleport.InCar.speed > cheatClient->Teleport.InCar.speed_max) {
						cheatClient->Teleport.InCar.speed = cheatClient->Teleport.InCar.speed_max;
					}
					*(float *)(localplayer.GetVehicle() + string2Offset(OBFUSCATE("0x7C"))) = warpX * (cheatClient->Teleport.InCar.speed * 0.01f);
					*(float *)(localplayer.GetVehicle() + string2Offset(OBFUSCATE("0x80"))) = warpY * (cheatClient->Teleport.InCar.speed * 0.01f);
					*(float *)(localplayer.GetVehicle() + string2Offset(OBFUSCATE("0x84"))) = 0.f;
					
					Vec3 marker_coorss = {marker_coors.x,marker_coors.y,20.f};
					
					float distToMarker = Get2dDistance(local_coors, marker_coorss);
					
					if(distToMarker < 40.f)
					{
						cheatClient->Teleport.InCar.speed_max = 100.f;
					}

					if(distToMarker < 5.f)
					{
						while(true)
						{
							marker_coorss.z = FindGroundZ(marker_coors.x, marker_coors.y);
							if(marker_coorss.z != 20.f)
							{
								break;
							}
						}
						marker_coorss.z += 1.5f;
						cheatClient->SetPos(localplayer.GetVehicle(), marker_coorss);
						//((void (*)(int))(libAddr + 0x20DCF4 + 1))(localped);
						cheatClient->Teleport.toMarker = false;
						cheatClient->Teleport.InCar.speed = 0;
						cheatClient->Teleport.InCar.speed_max = 100.f;
					}
				}
				else
				{
					// Stop teleport
					cheatClient->Teleport.toMarker = false;
					BrNotification(TYPE_TEXT_RED, OBFUSCATE("Вы не можете телепортироваться на пассажирском месте!"), 5);
				}
			}
			else
			{
				// OnFoot
				// Установка камеры на игрока
#ifndef __aarch64__
				*(char *)(libAddr + 0x8264BA) = 0;
				((void (*)(int))(libAddr + 0x2A6B66 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x2AA6C8 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x5D9E1C + 1))(libAddr + 0x825AB8);
#endif

				// Направляем игрока к маркеру
				Vec3 local_coors = cheatClient->GetPos(localped);
				Vec2 marker_coors = cheatClient->GetMarkerCoors();
				*(float *)(localped + ENT_POS_Z) += 0.5f;

				Vec3 cam_coors = cheatClient->GetCameraCoors();
				
				float warpX = (marker_coors.x - local_coors.x);
				float warpY = (marker_coors.y - local_coors.y);
				float length_vector = sqrt(warpX * warpX + warpY * warpY);
				warpX = warpX / length_vector;
				warpY = warpY / length_vector;
				float warpZ = local_coors.z - cam_coors.z;
				cheatClient->Teleport.OnFoot.speed += 0.1f;
				if(cheatClient->Teleport.OnFoot.speed > cheatClient->Teleport.OnFoot.speed_max) {
					cheatClient->Teleport.OnFoot.speed = cheatClient->Teleport.OnFoot.speed_max;
				}
				*(float *)(localped + 0x7C) = warpX * (cheatClient->Teleport.OnFoot.speed * 0.01f);
				*(float *)(localped + 0x80) = warpY * (cheatClient->Teleport.OnFoot.speed * 0.01f);
				*(float *)(localped + 0x84) = 0.f;
				
				Vec3 marker_coorss = {marker_coors.x,marker_coors.y,20.f};
				
				float distToMarker = Get2dDistance(local_coors, marker_coorss);
				
				if(distToMarker < 40.f)
				{
					cheatClient->Teleport.OnFoot.speed_max = 35.f;
				}

				if(distToMarker < 5.f)
				{
					while(true)
					{
						marker_coorss.z = FindGroundZ(marker_coors.x, marker_coors.y);
						if(marker_coorss.z != 20.f)
						{
							break;
						}
					}
					marker_coorss.z += 1.5f;
					cheatClient->SetPos(localped, marker_coorss);
					//((void (*)(int))(libAddr + 0x20DCF4 + 1))(localped);
					cheatClient->Teleport.toMarker = false;
					cheatClient->Teleport.OnFoot.speed = 0;
					cheatClient->Teleport.OnFoot.speed_max = 50.f;
				}
			}
		}

		if(cheatClient->Teleport.toCheckpoint)
		{
			// InCar
			if(localplayer.InVehicle())
			{
				CVehicle vehicle(localplayer.GetVehicle());
				if(vehicle.GetDriver() == localped)
				{
					// InCar
					// Установка камеры на игрока
	#ifndef __aarch64__
				*(char *)(libAddr + 0x8264BA) = 0;
				((void (*)(int))(libAddr + 0x2A6B66 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x2AA6C8 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x5D9E1C + 1))(libAddr + 0x825AB8);
#endif

					// Направляем игрока к маркеру
					Vec3 local_coors = cheatClient->GetPos(localplayer.GetVehicle());
					Vec3 marker_coors = cheatClient->GetCheckpointCoors();
					*(float *)(localplayer.GetVehicle() + 0x38) += 0.5f;

					Vec3 cam_coors = cheatClient->GetCameraCoors();
					
					float warpX = (marker_coors.x - local_coors.x);
					float warpY = (marker_coors.y - local_coors.y);
					float length_vector = sqrt(warpX * warpX + warpY * warpY);
					warpX = warpX / length_vector;
					warpY = warpY / length_vector;
					float warpZ = local_coors.z - cam_coors.z;
					cheatClient->Teleport.InCar.speed += 0.1f;
					if(cheatClient->Teleport.InCar.speed > cheatClient->Teleport.InCar.speed_max) {
						cheatClient->Teleport.InCar.speed = cheatClient->Teleport.InCar.speed_max;
					}
					*(float *)(localplayer.GetVehicle() + 0x7C) = warpX * (cheatClient->Teleport.InCar.speed * 0.01f);
					*(float *)(localplayer.GetVehicle() + 0x80) = warpY * (cheatClient->Teleport.InCar.speed * 0.01f);
					*(float *)(localplayer.GetVehicle() + 0x84) = 0.f;
					
					Vec3 marker_coorss = {marker_coors.x,marker_coors.y,20.f};
					
					float distToMarker = Get2dDistance(local_coors, marker_coorss);
					
					if(distToMarker < 40.f)
					{
						cheatClient->Teleport.InCar.speed_max = 100.f;
					}

					if(distToMarker < 5.f)
					{
						while(true)
						{
							marker_coorss.z = FindGroundZ(marker_coors.x, marker_coors.y);
							if(marker_coorss.z != 20.f)
							{
								break;
							}
						}
						marker_coorss.z += 1.5f;
						cheatClient->SetPos(localplayer.GetVehicle(), marker_coorss);
						//((void (*)(int))(libAddr + 0x20DCF4 + 1))(localped);
						cheatClient->Teleport.toCheckpoint = false;
						cheatClient->Teleport.InCar.speed = 0;
						cheatClient->Teleport.InCar.speed_max = 300.f;
					}
				}
				else
				{
					// Stop teleport
					cheatClient->Teleport.toCheckpoint = false;
					BrNotification(TYPE_TEXT_RED, OBFUSCATE("Вы не можете телепортироваться на пассажирском месте!"), 5);
				}
			}
			else
			{
				// OnFoot
				// Установка камеры на игрока
#ifndef __aarch64__
				*(char *)(libAddr + 0x8264BA) = 0;
				((void (*)(int))(libAddr + 0x2A6B66 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x2AA6C8 + 1))(libAddr + 0x825AB8);
				((void (*)(int))(libAddr + 0x5D9E1C + 1))(libAddr + 0x825AB8);
#endif

				// Направляем игрока к маркеру
				Vec3 local_coors = cheatClient->GetPos(localped);
				Vec3 marker_coors = cheatClient->GetCheckpointCoors();
				*(float *)(localped + ENT_POS_Z) += 0.5f;

				Vec3 cam_coors = cheatClient->GetCameraCoors();
				
				float warpX = (marker_coors.x - local_coors.x);
				float warpY = (marker_coors.y - local_coors.y);
				float length_vector = sqrt(warpX * warpX + warpY * warpY);
				warpX = warpX / length_vector;
				warpY = warpY / length_vector;
				float warpZ = local_coors.z - cam_coors.z;
				cheatClient->Teleport.OnFoot.speed += 0.1f;
				if(cheatClient->Teleport.OnFoot.speed > cheatClient->Teleport.OnFoot.speed_max) {
					cheatClient->Teleport.OnFoot.speed = cheatClient->Teleport.OnFoot.speed_max;
				}
				*(float *)(localped + 0x7C) = warpX * (cheatClient->Teleport.OnFoot.speed * 0.01f);
				*(float *)(localped + 0x80) = warpY * (cheatClient->Teleport.OnFoot.speed * 0.01f);
				*(float *)(localped + 0x84) = 0.f;
				
				Vec3 marker_coorss = {marker_coors.x,marker_coors.y,20.f};
				
				float distToMarker = Get2dDistance(local_coors, marker_coorss);
				
				if(distToMarker < 40.f)
				{
					cheatClient->Teleport.OnFoot.speed_max = 35.f;
				}

				if(distToMarker < 5.f)
				{
					while(true)
					{
						marker_coorss.z = FindGroundZ(marker_coors.x, marker_coors.y);
						if(marker_coorss.z != 20.f)
						{
							break;
						}
					}
					marker_coorss.z += 1.5f;
					cheatClient->SetPos(localped, marker_coorss);
					//((void (*)(int))(libAddr + 0x20DCF4 + 1))(localped);
					cheatClient->Teleport.toCheckpoint = false;
					cheatClient->Teleport.OnFoot.speed = 0;
					cheatClient->Teleport.OnFoot.speed_max = 50.f;
				}
			}
		}
	}
}

void CGame_Process()
{
	old_CGame_Process();
	CheatFrameBody();
}

#ifdef __aarch64__
void JNILib_step_hook(JNIEnv* env, jclass cls)
{
	old_JNILib_step(env, cls);
	CheatFrameBody();
}
#endif

void SetVehicleVelocityRPC(int a1)
{
	if(cheatClient != nullptr)
	{
		if(cheatClient->NOPs.vehicleVelocity)
		{
			return;
		}
		else
		{
			old_SetVehicleVelocityRPC(a1);
		}
	}
	else
	{
		old_SetVehicleVelocityRPC(a1);
	}
}

void CPlayerBubbleChatPool_New(int playerbubblepool, int playerId, char* szText, int color, float duration, int drawDistance)
{
	if(cheatClient->Wallhack.Player.nametags)
	{
		old_CPlayerBubbleChatPool_New(playerbubblepool, playerId, szText, color, 2000.f, drawDistance);
		return;
	}
	else
	{
		old_CPlayerBubbleChatPool_New(playerbubblepool, playerId, szText, color, duration, drawDistance);
		return;
	}
}

void ScrServerJoin(int a1)
{
	cheatClient->ServerPlayersCount++;
	old_ScrServerJoin(a1);
}

void ScrServerQuit(int a1)
{
	cheatClient->ServerPlayersCount--;
	old_ScrServerQuit(a1);
}

void ScrConnectionRejected()
{
	cheatClient->ServerPlayersCount = 1;
	for(int i = 0; i < 1500; i++)
	{
		cheatClient->ServerPlayers[i] = {0};
	}
	
	old_ScrConnectionRejected();
}

void ScrServerNewPlayer(int pool, int nickname, int playerID)
{
	ServerPlayer player;
	player.id = playerID;
	strncpy(player.nickname, (const char*)nickname, 25);
	cheatClient->ServerPlayers[playerID] = player;
	old_ScrServerNewPlayer(pool, nickname, playerID);
}
 
void ScrInitGame(RPCParameters* rpcParams)
{
	cheatClient->GameServer.sender = rpcParams->sender;
	cheatClient->GameServer.recipient = rpcParams->recipient;
	cheatClient->GameServer.replyToSender = rpcParams->replyToSender;
	
	for(int i = 0; i < 1500; i++)
	{
		cheatClient->ServerPlayers[i].id = -1;
	}
	
	old_ScrInitGame(rpcParams);
}

void ScrRequestSpawn()
{
	old_ScrRequestSpawn();
}

void ScrSetSpawnInfo(int a1)
{
	old_ScrSetSpawnInfo(a1);
}

void LocalSetSpawnInfo(int *localped, int *spawnInfo)
{
	old_LocalSetSpawnInfo(localped,spawnInfo);
}

void WorldVehicleDelete(RPCParameters* rpcParams)
{
	old_WorldVehicleDelete(rpcParams);
}

int GetIsLineOfSightClear(int origin, int target, int buildings, int vehicles, int peds, int objects, int dummies, int doSeeThroughCheck, int doCameraIgnoreCheck)
{
	if(cheatClient != nullptr)
	{
		if(cheatClient->Wallhack.Player.nametags)
		{
			return old_GetIsLineOfSightClear(origin, target, 0, 0, 0, 0, 0, 0, 0);
		}
		else
		{
			return old_GetIsLineOfSightClear(origin, target, buildings, vehicles, peds, objects, dummies, doSeeThroughCheck, doCameraIgnoreCheck);
		}
	}
	return old_GetIsLineOfSightClear(origin, target, buildings, vehicles, peds, objects, dummies, doSeeThroughCheck, doCameraIgnoreCheck);
}

void CSprintCalculator(int* result)
{
	if(cheatClient != nullptr)
	{
		if(cheatClient->PlayerCheat.Base.Invulnerability.endurance)
		{
			return;
		}
	}
	
	old_CSprintCalculator(result);
}

void CPedDamageCalculator(int a1, int a2, unsigned int a3, float a4, int a5, int a6)
{
	if(cheatClient != nullptr)
	{
		if(cheatClient->PlayerCheat.Base.Invulnerability.health)
		{
			old_CPedDamageCalculator(a1, a2, a3, 0.f, a5, a6);
			return;
		}
	}
	
	old_CPedDamageCalculator(a1, a2, a3, a4, a5, a6);
}

void CVehicleDamageCalculator(int a1, float a2, int a3)
{
	if(cheatClient != nullptr)
	{
		if(cheatClient->VehicleCheat.Invulnerability.health)
		{
			return;
		}
	}
	
	old_CVehicleDamageCalculator(a1, a2, a3);
}

void NoBikeCalculator(int a1, int a2, int a3, int a4, int a5)
{
	if(cheatClient != nullptr)
	{
		if(cheatClient->PlayerCheat.Base.Invulnerability.noBike)
		{
			return;
		}
	}
	
	old_NoBikeCalculator(a1,a2,a3,a4,a5);
}

extern uintptr_t g_camMatrixAddr;

// arm64: locate the camera RwMatrix inside the module's writable segments by
// matching the aim-sync camera position + front vector (captured by the send
// hook into g_camPos/g_camFront). Found once, reused forever.
// scan committed rw-p heap mappings for the camera RwMatrix.
// Mode A (aim-sync seen): pos row (+0x30) matches the packet bit-exact + 'at'
// row ≈ camFront — no false positives.
// Mode B (no aim yet): any matrix whose pos sits 1..25m from the local ped
// and whose 'at' row points at the player — third-person cam property.
// every page of [p, p+n) resident? guards derefs of heap-looking garbage
// maps-based VMA check — mincore is broken on this kernel even untagged.
// MTE: heap pointers carry a tag in bits 56-63; mask it for comparisons.
int g_mapNDebug = -1;   // exported for badlp diag — how many map lines parsed
bool RangeResident(uintptr_t p, size_t n)
{
    // tagged-heap device: VMA bases in /proc/self/maps carry the tag byte too —
    // compare the pointer as-is
    static uintptr_t mapLo[32768], mapHi[32768];
    static int mapN = 0;
    static int mapRefresh = 0;   // refresh maps every ~256 calls (time() is unreliable here)
    if(--mapRefresh <= 0)
    {
        FILE *f = fopen("/proc/self/maps", "r");
        if(f)
        {
            int cnt = 0;
            char line[256];
            uintptr_t b, e;
            while(cnt < 32768 && fgets(line, sizeof(line), f))
            {
                if(sscanf(line, "%llx-%llx", (unsigned long long *)&b, (unsigned long long *)&e) == 2)
                { mapLo[cnt] = b; mapHi[cnt] = e; cnt++; }
            }
            fclose(f);
            mapN = cnt;
            g_mapNDebug = cnt;
        }
        else { g_mapNDebug = -2; __android_log_print(ANDROID_LOG_INFO, "PEDDBG", "MAPS fopen failed errno=%d", errno); }
        static bool loggedMap = false;
        if(!loggedMap)
        {
            loggedMap = true;
            __android_log_print(ANDROID_LOG_INFO, "PEDDBG", "MAPS n=%d first=%llx-%llx", mapN,
                                (unsigned long long)mapLo[0], (unsigned long long)mapHi[0]);
        }
        mapRefresh = 256;
    }
    // probe tagged and untagged forms — maps may carry tag in VMA base or not
    uintptr_t pp[2] = { p, p & 0x00FFFFFFFFFFFFFFULL };
    for(int k = 0; k < 2; k++)
    {
        uintptr_t q = pp[k];
        int lo = 0, hi = mapN - 1;
        while(lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            if(q < mapLo[mid]) hi = mid - 1;
            else if(q >= mapHi[mid]) lo = mid + 1;
            else return q + n <= mapHi[mid];
        }
    }
    return false;
}

uintptr_t g_camObjAddr = 0;   // owning RwCamera (frame->parent) — has viewWindow
bool g_camVerified = false;   // heuristic match until aim-sync proves it

// crash-proof read: VMAs can be unmapped mid-scan (GC, thread teardown) —
// a raw deref raced and SIGSEGV'd @0xa20. process_vm_readv just fails.
bool SafeRead(uintptr_t addr, void *dst, size_t n)
{
    iovec li = { dst, n }, ri = { (void *)addr, n };
    return syscall(__NR_process_vm_readv, getpid(), &li, 1, &ri, 1, 0) == (ssize_t)n;
}

static void CamObjFromMatrix(uintptr_t m)
{
    if(!m || m < 0x40) return;
    uintptr_t cam = 0;
    if(!SafeRead(m - 0x18 + 0x8, &cam, 8)) return; // frame->object.parent
    if(cam && RangeResident(cam, 0x40)) g_camObjAddr = cam;
}

static void HeapScanForCamera()
{
    bool haveAim = g_camValid;
    float px = 0, py = 0, pz = 0;
    // streamed ped positions — used to reject "an NPC/ped facing the player"
    // (its pos is 1..25m away AND its 'at' aims at us, same as the camera)
    float pedPos[1600][3]; int pedN = 0;
    if(!haveAim)
    {
        uintptr_t lp = cheatClient ? cheatClient->FindLocalPlayer() : 0;
        if(!HeapPtr(lp) || !RangeResident(lp, 0x50))
        {
            static int badlpN = 0;
            if(badlpN++ < 20 || badlpN % 500 == 0)
                __android_log_print(ANDROID_LOG_INFO, "PEDDBG", "CAMMATRIX skip badlp=%p mapN=%d", (void *)lp, g_mapNDebug);
            return;
        }
        Vec3 lpv = cheatClient->GetPos(lp);   // live pos from entity matrix — lp+0x38 is stale spawn pos
        if(lpv.x*lpv.x + lpv.y*lpv.y < 4.f) return;   // unreadable/near-origin pos → junk matches at origin
        px = lpv.x; py = lpv.y; pz = lpv.z;
        for(int i = 0; i < 1500; i++)
        {
            uintptr_t rp = cheatClient->GetAtPlayerPool(i);
            // pool mutates under us — validate before every deref (crash @0xa20)
            if(!HeapPtr(rp)) continue;
            if(!RangeResident(rp, 0x20)) continue;
            uintptr_t pd = 0;
            if(!SafeRead(rp + PLAYER_PED_OFF, &pd, 8)) continue;
            if(!HeapPtr(pd)) continue;
            if(!RangeResident(pd, 0x80)) continue;
            if(pedN < 1600)
            {
                Vec3 pp = cheatClient->GetPos(pd);
                pedPos[pedN][0] = pp.x; pedPos[pedN][1] = pp.y; pedPos[pedN][2] = pp.z;
                pedN++;
            }
        }
    }
    const uint32_t sx = *(uint32_t *)&g_camPos[0];
    const uint32_t sy = *(uint32_t *)&g_camPos[1];
    const uint32_t sz = *(uint32_t *)&g_camPos[2];
    const float fx = g_camFront[0], fy = g_camFront[1], fz = g_camFront[2];
    FILE *f = fopen("/proc/self/maps", "r");
    if(!f) return;
    char line[512];
    uintptr_t best = 0; float bestDot = 0.9f;
    static char s_cambuf[0x4000 + 0x40];
    while(fgets(line, sizeof(line), f))
    {
        uintptr_t b, e; char perm[8];
        if(sscanf(line, "%llx-%llx %7s", (unsigned long long *)&b,
                  (unsigned long long *)&e, perm) != 3) continue;
        if(perm[0] != 'r' || perm[1] != 'w') continue;
        if(e - b > 0x20000000) continue;
        // chunked SafeRead over the VMA — races with unmaps can't fault
        uintptr_t cur = b; size_t carry = 0;
        while(cur < e)
        {
            size_t want = e - cur;
            if(want > 0x4000) want = 0x4000;
            if(!SafeRead(cur, s_cambuf + carry, want)) { cur += want; carry = 0; continue; }
            size_t total = carry + want;
            uintptr_t base = cur - carry;                    // buf covers [base, base+total)
            uintptr_t lo = cur > base + 0x40 ? cur : base + 0x40;
            uintptr_t hi = base + total;                     // need a+8 < hi
            for(uintptr_t a = lo; a + 8 < hi; a += 4)
            {
                const char *pa = s_cambuf + (a - base);
                if(haveAim)
                {
                    if(*(const uint32_t *)pa != sx) continue;
                    if(*(const uint32_t *)(pa+4) != sy || *(const uint32_t *)(pa+8) != sz) continue;
                    const char *at = pa - 0x10;
                    if(fabsf(*(const float *)at - fx) < 0.02f &&
                       fabsf(*(const float *)(at+4) - fy) < 0.02f &&
                       fabsf(*(const float *)(at+8) - fz) < 0.02f)
                    {
                        g_camMatrixAddr = a - 0x30;
                        g_camVerified = true;
                        CamObjFromMatrix(g_camMatrixAddr);
                        const float *mm = (const float *)(pa - 0x30);
                        __android_log_print(ANDROID_LOG_INFO, "PEDDBG",
                            "CAMMATRIX found @ %p (aim) camobj=%p libd=0x%llx r=(%.2f,%.2f,%.2f) u=(%.2f,%.2f,%.2f) f=(%.2f,%.2f,%.2f) p=(%.1f,%.1f,%.1f)",
                            (void *)g_camMatrixAddr, (void *)g_camObjAddr,
                            (unsigned long long)(g_camMatrixAddr - libAddr),
                            mm[0],mm[1],mm[2], mm[4],mm[5],mm[6], mm[8],mm[9],mm[10], mm[12],mm[13],mm[14]);
                        fclose(f); return;
                    }
                }
                else
                {
                    // pos must differ from player pos — ped matrices are exact
                    float mx = *(const float *)pa;
                    float dx = mx - px; if(dx > 12.f || dx < -12.f) continue;
                    float dy = *(const float *)(pa+4) - py; if(dy > 12.f || dy < -12.f) continue;
                    float dz = *(const float *)(pa+8) - pz; if(dz > 12.f || dz < -12.f) continue;
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    if(dist < 1.f || dist > 12.f) continue;
                    const char *at = pa - 0x10;
                    float ax = *(const float *)at, ay = *(const float *)(at+4), az = *(const float *)(at+8);
                    float al = sqrtf(ax*ax + ay*ay + az*az);
                    if(al < 0.95f || al > 1.05f) continue; // strict unit forward
                    if(dz < 0.4f || dz > 8.f) continue;    // real cam sits above the ped
                    // orthonormal frame: right/up must be unit and ⊥ at
                    float rx2 = *(const float *)(pa - 0x30), ry2 = *(const float *)(pa - 0x2C), rz2 = *(const float *)(pa - 0x28);
                    float ux2 = *(const float *)(pa - 0x20), uy2 = *(const float *)(pa - 0x1C), uz2 = *(const float *)(pa - 0x18);
                    float rl = rx2*rx2 + ry2*ry2 + rz2*rz2, ul = ux2*ux2 + uy2*uy2 + uz2*uz2;
                    if(rl < 0.7f || rl > 1.3f || ul < 0.7f || ul > 1.3f) continue;
                    if(fabsf(rx2*ax + ry2*ay + rz2*az) > 0.25f) continue;
                    if(fabsf(ux2*ax + uy2*ay + uz2*az) > 0.25f) continue;
                    // reject if pos matches any streamed ped — that IS a ped matrix
                    bool isPed = false;
                    for(int i = 0; i < pedN; i++)
                    {
                        float qx = mx - pedPos[i][0]; if(qx > 1.5f || qx < -1.5f) continue;
                        float qy = *(const float *)(pa+4) - pedPos[i][1]; if(qy > 1.5f || qy < -1.5f) continue;
                        float qz = *(const float *)(pa+8) - pedPos[i][2]; if(qz > 1.5f || qz < -1.5f) continue;
                        isPed = true; break;
                    }
                    if(isPed) continue;
                    float d = (-dx*ax + -dy*ay + -dz*az) / dist;  // at → player
                    if(d < 0.8f) continue;
                    // prefer candidates whose owning camera object resolves —
                    // the real RwCamera does, junk matrices don't
                    float score = d;
                    {
                        uintptr_t cam = 0;
                        if(SafeRead(a - 0x30 - 0x18 + 0x8, &cam, 8) &&
                           (cam & 0x00FFFFFFFFFFFFFFULL) &&
                           RangeResident(cam & 0x00FFFFFFFFFFFFFFULL, 0x40))
                            score += 0.5f;
                    }
                    if(score > bestDot) { bestDot = score; best = a; }
                }
            }
            // keep last 0x40 bytes so -0x30 look-back works across the seam
            if(total >= 0x40) { memmove(s_cambuf, s_cambuf + total - 0x40, 0x40); carry = 0x40; }
            else carry = 0;
            cur += want;
        }
    }
    fclose(f);
    if(!haveAim && best)
    {
        g_camMatrixAddr = best - 0x30;
        g_camVerified = false;
        CamObjFromMatrix(g_camMatrixAddr);
        __android_log_print(ANDROID_LOG_INFO, "PEDDBG",
            "CAMMATRIX found @ %p (ped-heur, dot=%.2f) camobj=%p libd=0x%llx",
            (void *)g_camMatrixAddr, bestDot, (void *)g_camObjAddr,
            (unsigned long long)(g_camMatrixAddr - libAddr));
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "PEDDBG", "CAMMATRIX not found (aim=%d pos %.1f %.1f %.1f)",
        haveAim, g_camPos[0], g_camPos[1], g_camPos[2]);
}

static int camScanStarted = 0;
static void *CamScanThread(void *)
{
    HeapScanForCamera();
    camScanStarted = 0;
    return NULL;
}

void FindCameraMatrix()
{
    static int camDbg = 0;
    if(!libAddr || camScanStarted) { if(camDbg++ == 0) __android_log_print(ANDROID_LOG_INFO,"PEDDBG","CAMSCAN skip lib=%p started=%d",(void*)libAddr,camScanStarted); return; }
    // rescan when: nothing found yet, or an aim-sync arrived and the current
    // matrix is only a heuristic guess — upgrade to the verified one
    if(g_camMatrixAddr && (g_camVerified || !g_camValid)) return;
    uintptr_t lp0 = cheatClient ? cheatClient->FindLocalPlayer() : 0;
    if(!g_camValid && !lp0) { if(camDbg++ == 0) __android_log_print(ANDROID_LOG_INFO,"PEDDBG","CAMSCAN skip nolp"); return; }
    camScanStarted = 1;
    pthread_t t;
    int rc = pthread_create(&t, NULL, CamScanThread, NULL);
    if(camDbg++ < 3) __android_log_print(ANDROID_LOG_INFO,"PEDDBG","CAMSCAN spawn rc=%d lp=%p aim=%d",rc,(void*)lp0,g_camValid);
    if(rc == 0) pthread_detach(t);
    else camScanStarted = 0;
}

int WorldToScreen(Vec3* pos, Vec3* output)
{
#ifdef __aarch64__
    float m[16];
    if(g_camMatrixAddr)
    {
        // stale-heap guard: SafeRead fails on freed/unmapped — invalidate
        errno = 0;
        bool ok = SafeRead(g_camMatrixAddr, m, 0x40);
        int rdErr = errno;
        if(ok)
        {
            float al = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
            if(al < 0.5f || al > 1.5f)
            {
                static int invN = 0;
                if(invN++ < 8) __android_log_print(ANDROID_LOG_INFO, "PEDDBG",
                    "WTS invalidate al=%.3f at=(%.2f,%.2f,%.2f) m=%p", al, m[8], m[9], m[10], (void*)g_camMatrixAddr);
                ok = false;   // 'at' lost unit length
            }
            // wrong-matrix guard: heuristic can latch onto a junk matrix near
            // origin — real 3rd-person cam stays within ~30m of the local ped
            static int camDistChk = 0;
            if(ok && !g_camVerified && ++camDistChk >= 240)
            {
                camDistChk = 0;
                uintptr_t lp = cheatClient ? cheatClient->FindLocalPlayer() : 0;
                if(lp)
                {
                    Vec3 lp2 = cheatClient->GetPos(lp);
                    float ddx = m[12]-lp2.x, ddy = m[13]-lp2.y, ddz = m[14]-lp2.z;
                    if(ddx*ddx + ddy*ddy + ddz*ddz > 225.f)
                    {
                        static int invN3 = 0;
                        if(invN3++ < 8) __android_log_print(ANDROID_LOG_INFO, "PEDDBG",
                            "WTS invalidate far cam=(%.1f,%.1f,%.1f) lp=(%.1f,%.1f,%.1f)",
                            m[12],m[13],m[14], lp2.x,lp2.y,lp2.z);
                        ok = false;
                    }
                }
            }
        }
        else
        {
            static int invN2 = 0;
            if(invN2++ < 8) __android_log_print(ANDROID_LOG_INFO, "PEDDBG",
                "WTS invalidate readfail m=%p errno=%d", (void*)g_camMatrixAddr, rdErr);
        }
        if(!ok) { g_camMatrixAddr = 0; g_camObjAddr = 0; g_camVerified = false; }
    }
    if(!g_camMatrixAddr)
    {
        FindCameraMatrix();
        if(!g_camMatrixAddr) return 0;
        if(!SafeRead(g_camMatrixAddr, m, 0x40)) { g_camMatrixAddr = 0; return 0; }
    }
    float rx = m[0], ry = m[1], rz = m[2];
    float ux = m[4], uy = m[5], uz = m[6];
    float fx = m[8], fy = m[9], fz = m[10];
    float cx = m[12], cy = m[13], cz = m[14];
    float dx = pos->x - cx, dy = pos->y - cy, dz = pos->z - cz;
    float z = dx*fx + dy*fy + dz*fz;
    {
        static int wtsDbg = 0;
        if(++wtsDbg >= 5000)
        {
            wtsDbg = 0;
            __android_log_print(ANDROID_LOG_INFO, "PEDDBG",
                "WTS m=%p cam=(%.1f,%.1f,%.1f) at=(%.2f,%.2f,%.2f) ped=(%.1f,%.1f,%.1f) z=%.1f",
                (void*)g_camMatrixAddr, cx,cy,cz, fx,fy,fz, pos->x,pos->y,pos->z, z);
        }
    }
    if(z <= 0.05f) return 0;
    // real projection: screen = viewspace.xy / viewWindow (RwCamera +0x28/+0x2C)
    float focalX = (cheatClient->screen_width * 0.5f) / 1.25f;
    float focalY = (cheatClient->screen_height * 0.5f) / 0.5206f;
    if(g_camObjAddr)
    {
        float vw[2];
        if(SafeRead(g_camObjAddr + 0x28, vw, 8) &&
           vw[0] > 0.1f && vw[0] < 3.f && vw[1] > 0.1f && vw[1] < 3.f)
        {
            focalX = (cheatClient->screen_width * 0.5f) / vw[0];
            focalY = (cheatClient->screen_height * 0.5f) / vw[1];
        }
    }
    output->x = cheatClient->screen_width * 0.5f - (dx*rx + dy*ry + dz*rz) * focalX / z;
    output->y = cheatClient->screen_height * 0.5f - (dx*ux + dy*uy + dz*uz) * focalY / z;
    output->z = z;
    return 1;
#else
	char temp1[4];
	char temp2[4];
	
	if(((int (*)(uintptr_t, float*, float*, int, char))(cheatClient->GetLibAddr() + string2Offset(OBFUSCATE("0x3F3044")) + 1))((uintptr_t)pos, (float *)output, (float *)temp1, (int)temp2, 1) != 0)
	{
		return 1;
	}
	
	return 0;
#endif
}

void CPed_ProcessBuoyancy(int ped)
{
	if(cheatClient->PlayerCheat.Base.Invulnerability.walkUnderwater) { return; }
	old_CPed_ProcessBuoyancy(ped);
}

void CBike_ProcessBuoyancy(int bike)
{
	if(cheatClient->VehicleCheat.Invulnerability.driveUnderwater) { return; }
	old_CBike_ProcessBuoyancy(bike);
}

void CAutomobile_ProcessBuoyancy(int vehicle)
{
	if(cheatClient->VehicleCheat.Invulnerability.driveUnderwater) { return; }
	old_CAutomobile_ProcessBuoyancy(vehicle);
}

void log_1(int vehicle_pool, unsigned short* structure)
{
	unsigned short *v4 = structure + 3;
	int x = *(int *)(structure + 3);
	
	/**(float *)(structure + 3) = 0;
	*(float *)(structure + 4) = 0;
	*(float *)(structure + 5) = 10.f;
	
	/*int y = *((int *)v4 + 1);
	int z = *((int *)v4 + 2);*/
	//AddChatMessage("MODEL ID: %d, ID: %d, [11]: %u, HP: %f", *(int *)(structure + 1), *structure, structure[11], *((float *)structure + 6));
	//AddChatMessage("X: %f", x);
	old_log1(vehicle_pool, structure);
}

void log_2(int vehicle, int a2, int a3, int a4, int a5)
{
	//AddChatMessage("Vehicle: %d, a2: %d, a3: %d, a4: %d, a5: %d", vehicle, a2, a3, a4, a5);
	old_log2(vehicle, a2, a3, a4, a5);
}

void log_3(int peer, int *a2, RakNet::BitStream* bitStream, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11, int a12, int a13, int a14)
{
	gpeer = peer;
	ga7 = a7;
	ga8 = a8;
	
	if(*a2 == 115)
	{
		bool takeGive;
		unsigned short playerID;
		float damage;
		int weapon;
		int bone;

		bitStream->Read(takeGive);
    	bitStream->Read(playerID);
    	bitStream->Read(damage);
    	bitStream->Read(weapon);
    	bitStream->Read(bone);														
		
		//AddChatMessage("{ff0000}TakeGiveDamage |{ffffff} TakeGive: %d, PlayerID: %u, Damage: %f, Weapon: %d, Bone: %d",
		//takeGive, playerID, damage, weapon, bone);
	}
	old_log3(peer,a2,bitStream,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14);
}

void calculateNewCoordinates(float& x, float& y, float& z, float x_angle, float y_angle, float z_angle, float forward, float height) {
    // Время вперед (в секундах) и вверх (в секундах)
    float time_forward = forward;
    float time_up = height;
	float left = 2.f;
	
    // Углы наклона машины в радианах
    float x_angle_rad = x_angle;
    float y_angle_rad = y_angle;
    float z_angle_rad = z_angle;

    // Вычисляем новые координаты
    // Для упрощения расчетов, предположим, что машина движется с постоянной скоростью
    float new_x = x + time_forward * cos(z_angle_rad) * cos(y_angle_rad);
    float new_y = y + time_forward * sin(z_angle_rad) * cos(y_angle_rad);
    float new_z = z + time_forward * sin(y_angle_rad);

    // Обновляем координаты
    x = new_x - left * cos(x_angle);
    y = new_y - left * sin(x_angle);
    z = new_z + time_up;
}

void Draw3dBox(int object, ImVec4 color, float tickness)
{
	int colmodel = *(int *)(*(int *)(libAddr + 0x46E6700 + (4 * *(unsigned short *)(object + 0x5A))) + 28);
	if(colmodel)
	{
		int rw_object = *(int *)(object + 0x44);
		
		float object_angle_x = atan2f(*(float *)(rw_object + 36), *(float *)(rw_object + 40));
		float object_angle_y = atan2f(-*(float *)(rw_object + 32), sqrt((*(float *)(rw_object + 36) * *(float *)(rw_object + 36)) + (*(float *)(rw_object + 40) * *(float *)(rw_object + 40))));
		float object_angle_z = atan2f(*(float *)(rw_object + 4), *(float *)(rw_object + 20));
		
		float object_forward_width = (float)(*(float *)(colmodel + 28) - *(float *)(colmodel + 16));
		float object_side_width = (float)(*(float *)(colmodel + 32) - *(float *)(colmodel + 20));
		float object_height = (float)(*(float *)(colmodel + 36) - *(float *)(colmodel + 24));
		
		Vec3 screen_object_pos = Vec3(0,0,0);
		Vec3 object_pos = { *(float *)(object + ENT_POS_X) , *(float *)(object + ENT_POS_Y) , *(float *)(object + ENT_POS_Z) };
		Vec3 object_pos_ = object_pos;
		Vec3 forward_up_left_point = Vec3(0,0,0);
		Vec3 forward_up_right_point = Vec3(0,0,0);
		Vec3 forward_down_left_point = Vec3(0,0,0);
		Vec3 forward_down_right_point = Vec3(0,0,0);
		Vec3 back_up_left_point = Vec3(0,0,0);
		Vec3 back_up_right_point = Vec3(0,0,0);
		Vec3 back_down_left_point = Vec3(0,0,0);
		Vec3 back_down_right_point = Vec3(0,0,0);
						
		Vec3 screen_forward_up_left_point = Vec3(0,0,0);
		Vec3 screen_forward_up_right_point = Vec3(0,0,0);
		Vec3 screen_forward_down_left_point = Vec3(0,0,0);
		Vec3 screen_forward_down_right_point = Vec3(0,0,0);
		Vec3 screen_back_up_left_point = Vec3(0,0,0);
		Vec3 screen_back_up_right_point = Vec3(0,0,0);
		Vec3 screen_back_down_left_point = Vec3(0,0,0);
		Vec3 screen_back_down_right_point = Vec3(0,0,0);
		
		float x2 = object_pos.x + (object_forward_width + 0.7f) * cos(object_angle_z);
    	float y2 = object_pos.y + (object_forward_width + 0.7f) * sin(object_angle_z);
		
		calculateNewCoordinates(object_pos.x, object_pos.y, object_pos.z, object_angle_y, object_angle_x, object_angle_z, object_forward_width, object_height);
		//float new_x = object_pos.x + object_forward_width * cos(object_angle_z);
    	//float new_y = object_pos.y + object_forward_width * sin(object_angle_z);
		Vec3 forward_pos = { object_pos.x, object_pos.y, object_pos.z };
		Vec3 screen_forward_pos = {0,0,0};
		
		if(WorldToScreen(&object_pos_, &screen_object_pos))
		{
			if(screen_object_pos.z >= 1.f)
			{
				if(WorldToScreen(&forward_pos, &screen_forward_pos))
				{
					if(screen_forward_pos.z >= 1.f)
					{
						ImGui::GetBackgroundDrawList()->AddLine(ImVec2(screen_object_pos.x, screen_object_pos.y), ImVec2(screen_forward_pos.x, screen_forward_pos.y), IM_COL32(255,0,0,255), 5.f);
					}
				}
			}
		}
	}
}
