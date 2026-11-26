#include "cheat.h"
#include "security/obfuscate.h"
#include <sys/mman.h>

extern bool RangeResident(uintptr_t p, size_t n); // main.cpp — maps-based VMA check
extern bool SafeRead(uintptr_t addr, void *dst, size_t n); // main.cpp — crash-free read
static inline bool HeapPtrA64(uintptr_t p)
{
	p &= 0x00FFFFFFFFFFFFFFULL;                    // strip MTE tag
	return p >= 0x0000000100000000ULL && p < 0x0000FF0000000000ULL;
}

Cheat::Cheat()
{
	/*chams = false;
	flycar = false;
	flyped = false;
	carshot = false;
	airbrake_car = false;
	nopClearAnim = false;
	nopVehicleVelocity = false;
	infinite_engine = false;
	espPedLine = false;
	espPedBox = false;
	espPedSkeleton = false;
	nametags = false;
	no_reload = false;
	aimbot = false;
	gmped = false;
	gmcar = false;
	infiniteRun = false;
	longjump = false;
	highjump = false;
	fastrotate = false;
	tuning = false;
	damager = false;
	nobike = false;
	wallhack = false;
	set_player_skin = false;
	set_player_money = false;
	freecam = false;
	walk_underwater = false;
	drive_underwater = false;
	flooder = false;
	fake_chat = false;
	// боты
	botminer = false;
	botfactory = false;
	botfarm = false;
	botcourier = false;
	botbus = false;
	bottruck = false;
	anti_afk = false;
	auto_heal = false;
	game_speed = false;
	collision = false;
	change_car_model = false;
	change_car_model_entered = false;
	anti_remove_trailer = false;
	tuning_menu = false;
	auto_armour = false;
	set_fov = false;
	set_time = false;
	set_weather = false;
	trigger_gevent = false;*/
}

void Cheat::SetLibAddr(uintptr_t addr)
{
	this->libaddr = addr;
}

uintptr_t Cheat::GetLibAddr()
{
	return this->libaddr;
}

bool Cheat::InitTouchScreen(std::string filename = "")
{
	std::ifstream file(filename);
	
	if(!file.is_open()) {
		SetLastErrorMessage("Не удалось открыть файл touchscreen-custom.json");
		return false;
	}
	
	std::string file_content((std::istreambuf_iterator<char>(file)),
							std::istreambuf_iterator<char>());
	
	file.close();
	
	if(file_content.size() <= 0) {
		SetLastErrorMessage("Не удалось считать текст из touchscreen-custom.json");
		return false;
	}
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "accelerate") {
                // Получить значение "width"
                this->Accelerate.x = item["x"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "accelerate") {
                // Получить значение "width"
                this->Accelerate.y = item["y"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "accelerate") {
                // Получить значение "width"
                this->Accelerate.width = item["width"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "accelerate") {
                // Получить значение "width"
                this->Accelerate.height = item["height"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "brake") {
                // Получить значение "width"
                this->Brake.x = item["x"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "brake") {
                // Получить значение "width"
                this->Brake.y = item["y"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "brake") {
                // Получить значение "width"
                this->Brake.width = item["width"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	try {
        // Распарсить JSON
        nlohmann::json js = nlohmann::json::parse(file_content);

        // Получить конфигурацию
        nlohmann::json config = js["config"];
		
        // Проход по каждому элементу в конфигурации
        for (auto& item : config) {
            // Проверить имя элемента
            if (item["name"] == "brake") {
                // Получить значение "width"
                this->Brake.height = item["height"];
                
				break;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
		std::string error_message("JSON parsing error: ");
		error_message.append(e.what());
        SetLastErrorMessage(error_message);
		return false;
    }
	
	return true;
}

bool Cheat::TouchInJoystickZone(int x, int y)
{
	float y1 = screen_height / 2;
	float width = screen_width / 2;
	
	if(x < width && x > 0 && y > y1) {
		return true;
	}
	
	return false;
}

bool Cheat::TouchInAccelerateZone(int x, int y)
{
	if(x > this->Accelerate.x && x < this->Accelerate.x + this->Accelerate.width)
	{
		if(y > this->Accelerate.y && y < this->Accelerate.y + this->Accelerate.height)
		{
			return true;
		}
	}
	return false;
}

bool Cheat::TouchInBrakeZone(int x, int y)
{
	if(x > this->Brake.x && x < this->Brake.x + this->Brake.width)
	{
		if(y > this->Brake.y && y < this->Brake.y + this->Brake.height)
		{
			return true;
		}
	}
	return false;
}

std::string Cheat::GetLastErrorMessage()
{
	if(last_error_message.size() > 0) {
		return last_error_message;
	} else {
		std::string ret("Ошибки не обнаружены");
		return ret;
	}
}

void Cheat::SetLastErrorMessage(std::string error_message)
{
	last_error_message = error_message;
}

uintptr_t Cheat::GetPlayerPool()
{
#ifdef __aarch64__
	// arm64: pool base directly at global 0x34e7b30 (verified via RequestSpawn/ScrSetPlayerPosFindZ)
	return *(uintptr_t *)(libaddr + 0x34e7b30);
#else
	// g_NetGame object at 0x21c5030, m_pPlayerPool = [obj+0] (verified via RequestSpawn/ServerJoin)
	int *netgame = *(int **)(libaddr + 0x21c5030);
	if(netgame)
	{
		return (uintptr_t)*netgame;
	}
	return 0;
#endif
}

uintptr_t Cheat::GetVehiclePool()
{
#ifdef __aarch64__
	return 0; // TODO remap arm64
#else
	if((int *)(libaddr + 0x4B624F0))
	{
		return *(int *)(libaddr + 0x4B624F0);
	}
	else
	{
		return 0;
	}
#endif
}

uintptr_t Cheat::GetObjectPool()
{
#ifdef __aarch64__
	return 0; // TODO remap arm64
#else
	if((int *)(libaddr + 0x4B62508))
	{
		return *(int *)(libaddr + 0x4B62508);
	}
	else
	{
		return 0;
	}
#endif
}

uintptr_t Cheat::GetAtPlayerPool(int id)
{
	uintptr_t player_pool = GetPlayerPool();
	if(player_pool > 0x100000) {
#ifdef __aarch64__
		return ((uintptr_t (*)(uintptr_t,int))(libaddr + 0x15EE404))(player_pool, id); // CPlayerPool::GetAt arm64 verified
#else
		return ((int (*)(int,int))(libaddr + 0xDB323A + 1))(player_pool, id); // CPlayerPool::GetAt verified
#endif
	} else {
		return 0;
	}
}

uintptr_t Cheat::GetAtVehiclePool(int id)
{
#ifdef __aarch64__
	return 0; // TODO remap arm64
#else
	uintptr_t vehicle_pool = GetVehiclePool();
	if(vehicle_pool) {
		return ((int (*)(int,int))(libaddr + 0x374378 + 1))(vehicle_pool, id);
	} else {
		return 0;
	}
#endif
}

uintptr_t Cheat::GetAtObjectPool(int id)
{
#ifdef __aarch64__
	return 0; // TODO remap arm64
#else
	uintptr_t object_pool = GetObjectPool();
	if(object_pool) {
		return ((int (*)(int,int))(libaddr + 0x3681BA + 1))(object_pool, id);
	} else {
		return 0;
	}
#endif
}

uintptr_t Cheat::FindLocalPlayer()
{
	if(libaddr)
	{
#ifdef __aarch64__
		// from client sources: CPlayerPool::m_pLocalPlayer@+0, CLocalPlayer::m_pPlayerPed@+0x38
		uintptr_t pool = GetPlayerPool();
		if(pool > 0x100000)
		{
			uintptr_t lp = *(uintptr_t *)pool;
			if(lp > 0x100000)
			{
				uintptr_t ped = *(uintptr_t *)(lp + 0x38);
				if(ped) { return ped; }
			}
		}
		return ((uintptr_t (*)())(libaddr + 0xB85690))(); // fallback local-ped accessor
#else
		return ((int (*)())(libaddr + 0x58ded1))(); // local-ped accessor (verified via RPC416 handler)
#endif
	}
	
	return 0;
}

uintptr_t Cheat::FindNearestPlayer()
{
	uintptr_t player_pool = GetPlayerPool();
	if(player_pool) {
		int nearest_player = -1;
		float nearest_distance = std::numeric_limits<float>::max();
		for(int i = 0; i < 1501; i++)
		{
			uintptr_t player = GetAtPlayerPool(i);
			if(player) {
				player = *(uintptr_t *)(player + PLAYER_PED_OFF);
				if(player > 0x100000) {
					uintptr_t localped = FindLocalPlayer();
					if(localped) {
						Vec3 local_coors = { *(float *)(localped + ENT_POS_X) , *(float *)(localped + ENT_POS_Y) , *(float *)(localped + ENT_POS_Z) };
						Vec3 coors = { *(float *)(player + ENT_POS_X) , *(float *)(player + ENT_POS_Y) , *(float *)(player + ENT_POS_Z) };
						float distance = sqrt(pow(local_coors.x - coors.x, 2.f) + pow(local_coors.y - coors.y, 2.f) + pow(local_coors.z - coors.z, 2.f));
						if(distance < nearest_distance) {
							nearest_distance = distance;
							nearest_player = i;
						}
					}
				}
			}
		}
		return nearest_player;
	} else {
		return -1;
	}
}

int Cheat::GetWeaponId()
{
	return *(int *)(libaddr + 0x48E6CC4);
}

void Cheat::SetRotate(uintptr_t matrix, float x, float y, float z)
{
	float cosX;
	float cosY;
	float cosZ;
	float sinX;
	float sinY;
	float sinZ;
	float v14;
	float v15;
	float v16;
		
	cosX = cosf(x);
	sinX = sinf(x);
	cosY = cosf(y);
	sinY = sinf(y);
	cosZ = cosf(z);
	sinZ = sinf(z);
	*((float *)matrix + 6) = sinX;
	//*((int *)matrix + 12) = 0;
	//*((int *)matrix + 13) = 0;
	//*((int *)matrix + 14) = 0;
	v14 = (float)(sinX * cosZ) * sinY;
	v15 = (float)(sinX * cosZ) * cosY;
	v16 = sinX * sinZ;
	*((float *)matrix + 9) = (float)(sinY * sinZ) - v15;
	*((float *)matrix + 8) = (float)(v16 * cosY) + (float)(sinY * cosZ);
	*(float *)matrix = (float)(cosY * cosZ) - (float)(v16 * sinY);
	*((float *)matrix + 1) = v14 + (float)(cosY * sinZ);
	*((float *)matrix + 5) = cosX * cosZ;
	*((float *)matrix + 2) = -(float)(cosX * sinY);
	*((float *)matrix + 4) = -(float)(sinZ * cosX);
	*((float *)matrix + 10) = cosX * cosY;
}
	
void Cheat::SetRotateZOnly(uintptr_t matrix, float z)
{
	float v4;
	float v5;
	float v6;
		
	v4 = sinf(z);
	v5 = -v4;
	*((int *)matrix + 10) = 1065353216;
	*(float *)((char*)matrix + 4) = v4;
	v6 = cosf(z);
	*(float *)matrix = v6;
	*((float *)matrix + 5) = v6;
	*((float *)matrix + 4) = v5;
}

Vec2 Cheat::GetMarkerCoors()
{
	Vec2 coors;
	
	coors.x = *(float *)(libaddr + 0x459C400);
	coors.y = *(float *)(libaddr + 0x459C404);
	
	return coors;
}

Vec3 Cheat::GetCheckpointCoors()
{
	Vec3 coors = Vec3(0,0,0);
	
	coors.x = *(float *)(libaddr + 0x7B7268);
	coors.y = *(float *)(libaddr + 0x7B726C);
	coors.z = *(float *)(libaddr + 0x7B726D);
	
	return coors;
}

Vec3 Cheat::GetCameraCoors()
{
	Vec3 coors = {0,0,0};
	
	uintptr_t cameraMatrix = GetCameraMatrix();
	
	coors.x = *(float *)(cameraMatrix + 0x30);
	coors.y = *(float *)(cameraMatrix + 0x34);
	coors.z = *(float *)(cameraMatrix + 0x38);
	
	return coors;
}

uintptr_t Cheat::GetCameraMatrix()
{
#ifdef __aarch64__
	extern uintptr_t g_camMatrixAddr;
	if(g_camMatrixAddr) return g_camMatrixAddr;
#endif
	return libaddr + 0x825AB8;
}

uintptr_t Cheat::GetMatrix(uintptr_t object)
{
	if(!object) return 0;
#ifdef __aarch64__
	static int matOff = -2;
	static int matRetry = 0;
	if(matOff == -2 && matRetry++ % 120 == 0)
	{
		float pp[3];
		if(!SafeRead(object + 0x38, pp, 12)) return 0;
		for(int off = 0x8; off < 0x80 && matOff < 0; off += 8)
		{
			uintptr_t m = 0;
			if(!SafeRead(object + off, &m, 8)) return 0;
			if(!HeapPtrA64(m)) continue;
			float mr[3];
			if(!SafeRead(m + 0x30, mr, 12)) continue;
			if(fabsf(mr[0] - pp[0]) < 0.5f &&
			   fabsf(mr[1] - pp[1]) < 0.5f &&
			   fabsf(mr[2] - pp[2]) < 0.5f)
				matOff = off;
		}
	}
	if(matOff >= 0)
	{
		uintptr_t m = 0;
		if(SafeRead(object + matOff, &m, 8) && HeapPtrA64(m)) return m;
	}
#endif
	return 0;
}

Vec3 Cheat::GetPos(uintptr_t object)
{
	Vec3 coors = {0,0,0};

	if(!object) { return coors; }

#ifdef __aarch64__
	// entity pos lives in the RwMatrix pointed from the entity — the inline
	// +0x38 vector is only updated for the local ped, remote peds freeze.
	uintptr_t m = GetMatrix(object);
	if(m)
	{
		float mr[3];
		if(!SafeRead(m + 0x30, mr, 12)) return coors;
		coors.x = mr[0];
		coors.y = mr[1];
		coors.z = mr[2];
		return coors;
	}
#endif

	coors.x = *(float *)(object + ENT_POS_X);
	coors.y = *(float *)(object + ENT_POS_Y);
	coors.z = *(float *)(object + ENT_POS_Z);
	
	return coors;
}

void Cheat::SetPos(uintptr_t object, Vec3 coors)
{
	if(!object) { return; }
	
	*(float *)(object + ENT_POS_X) = coors.x;
	*(float *)(object + ENT_POS_Y) = coors.y;
	*(float *)(object + ENT_POS_Z) = coors.z;
}

void Cheat::SetPos(uintptr_t object, float x, float y, float z)
{
	if(!object) { return; }
	
	*(float *)(object + 0x30) = x;
	*(float *)(object + 0x34) = y;
	*(float *)(object + 0x38) = z;
}

void Cheat::SetNameTagsDrawDistance(float m_fDistance)
{
#ifndef __aarch64__
	*(float *)(libaddr + 0x4B62628) = m_fDistance;
#endif
}

float Cheat::GetNameTagsDrawDistance()
{
#ifdef __aarch64__
	return 0.f; // TODO remap arm64
#else
	return *(float *)(libaddr + 0x4B62628);
#endif
}

void Cheat::SetViewRotateX(float x)
{
	*(float *)(libaddr + 0x5EAC80) = x;
}

void Cheat::SetViewRotateY(float y)
{
	*(float *)(libaddr + 0x5EAC70) = y;
}

float Cheat::GetViewRotateX()
{
	return *(float *)(libaddr + 0x5EAC80);
}

float Cheat::GetViewRotateY()
{
	return *(float *)(libaddr + 0x5EAC70);
}

/*int Cheat::GetTimeInMilliseconds()
{
	return *(int *)(libaddr + 0x43BE5CC);
}
*/

#define ONE_SETTINGS_PATH OBFUSCATE("/storage/emulated/0/Android/data/com.br.top/files/oneshot_settings.json")

// UI globals (defined in main.cpp)
extern int icon_size;
extern int alphaWindowBg;
extern int alphaChildBg;
extern bool hideIcon;
extern float crosshair_offset_x;
extern float crosshair_offset_y;

static void jvec3(nlohmann::json& j, const char* k, const Vec3& v)
{
	j[k] = {{"x", v.x}, {"y", v.y}, {"z", v.z}};
}

static void jvec4(nlohmann::json& j, const char* k, const ImVec4& v)
{
	j[k] = {{"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w}};
}

void Cheat::SaveSettings()
{
	nlohmann::json j;
	
	j["player"]["slapUp"] = PlayerCheat.Base.Slap.up;
	j["player"]["slapDown"] = PlayerCheat.Base.Slap.down;
	j["player"]["flyPed"] = PlayerCheat.Base.FlyPed.state;
	j["player"]["flyPedSpeed"] = PlayerCheat.Base.FlyPed.speed;
	j["player"]["invHealth"] = PlayerCheat.Base.Invulnerability.health;
	j["player"]["invEndurance"] = PlayerCheat.Base.Invulnerability.endurance;
	j["player"]["walkUnderwater"] = PlayerCheat.Base.Invulnerability.walkUnderwater;
	j["player"]["noBike"] = PlayerCheat.Base.Invulnerability.noBike;
	j["player"]["jumpHigh"] = PlayerCheat.Base.Jumping.high;
	j["player"]["jumpLong"] = PlayerCheat.Base.Jumping.long_;
	j["player"]["jumpLongVal"] = PlayerCheat.Base.Jumping.long__;
	j["player"]["jumpHighVal"] = PlayerCheat.Base.Jumping.high__;
	j["player"]["fastRotate"] = PlayerCheat.Base.Control.fastRotate;
	j["player"]["rotateRate"] = PlayerCheat.Base.Control.rate;
	
	j["rage"]["noReload"] = PlayerCheat.Rage.NoReload.state;
	j["rage"]["aimBot"] = PlayerCheat.Rage.AimBot.state;
	j["rage"]["autoHeal"] = PlayerCheat.Rage.Invulnerability.autoHeal;
	j["rage"]["autoArmour"] = PlayerCheat.Rage.Invulnerability.autoArmour;
	j["rage"]["damager"] = PlayerCheat.Rage.Damager.state;
	j["rage"]["damagerTarget"] = PlayerCheat.Rage.Damager.targetID;
	
	j["vehicle"]["flyCar"] = VehicleCheat.FlyCar.state;
	j["vehicle"]["flyCarSpeed"] = VehicleCheat.FlyCar.speed;
	j["vehicle"]["carShot"] = VehicleCheat.CarShot.state;
	j["vehicle"]["carShotSpeed"] = VehicleCheat.CarShot.speed;
	j["vehicle"]["carShotAccel"] = VehicleCheat.CarShot.accelerate;
	j["vehicle"]["airBrake"] = VehicleCheat.AirBrake.state;
	j["vehicle"]["invHealth"] = VehicleCheat.Invulnerability.health;
	j["vehicle"]["invEngine"] = VehicleCheat.Invulnerability.engine;
	j["vehicle"]["driveUnderwater"] = VehicleCheat.Invulnerability.driveUnderwater;
	j["vehicle"]["antiDetach"] = VehicleCheat.AntiDetachTrailer.state;
	j["vehicle"]["carChangerModel"] = VehicleCheat.CarChanger.modelID;
	
	j["visual"]["timeFreeze"] = VisualCheat.TimeChanger.freeze;
	j["visual"]["time"] = VisualCheat.TimeChanger.time;
	j["visual"]["fov"] = VisualCheat.Camera.FOV;
	j["visual"]["freeCam"] = VisualCheat.Camera.FreeCam.state;
	j["visual"]["freeCamSpeed"] = VisualCheat.Camera.FreeCam.speed;
	j["visual"]["skinID"] = VisualCheat.SkinChanger.value;
	
	j["flooder"]["delay"] = Flooder.delay_after_message;
	j["flooder"]["message"] = Flooder.message;
	
	j["gameSpeed"]["state"] = GameSpeed.state;
	j["gameSpeed"]["speed"] = GameSpeed.speed;
	
	for(int i = 0; i < 3; i++)
	{
		j["tuning"]["bodyColor"][i] = Tuning.body_color[i];
		j["tuning"]["wheelsColor"][i] = Tuning.wheels_color[i];
		j["tuning"]["lightsColor"][i] = Tuning.lights_color[i];
		j["tuning"]["glowColor"][i] = Tuning.glow_color[i];
		j["tuning"]["rightGlow"][i] = Tuning.right_glow_color[i];
		j["tuning"]["leftGlow"][i] = Tuning.left_glow_color[i];
	}
	j["tuning"]["stroboscopes"] = Tuning.stroboscopes;
	j["tuning"]["stroboscopesType"] = Tuning.stroboscopesType;
	j["tuning"]["longLight"] = Tuning.longLight;
	j["tuning"]["totalClearance"] = Tuning.totalClearance;
	j["tuning"]["separateClearance"] = Tuning.separateClearance;
	j["tuning"]["totalClearanceVal"] = Tuning.total_clearance_val;
	j["tuning"]["separateClearanceVal"] = Tuning.separate_clearance_val;
	j["tuning"]["wheelsRadius"] = Tuning.wheels_radius;
	
	j["teleport"]["toMarker"] = Teleport.toMarker;
	j["teleport"]["toCheckpoint"] = Teleport.toCheckpoint;
	j["teleport"]["toPeds"] = Teleport.toPeds;
	j["teleport"]["toCars"] = Teleport.toCars;
	j["teleport"]["onFootSpeedMax"] = Teleport.OnFoot.speed_max;
	j["teleport"]["inCarSpeedMax"] = Teleport.InCar.speed_max;
	
	j["collision"]["ped"] = Collision.ped;
	j["collision"]["vehicle"] = Collision.vehicle;
	j["collision"]["objects"] = Collision.objects;
	
	j["carSpawn"]["modelID"] = CarSpawn.modelID;
	
	j["wallhack"]["player"]["lines"] = Wallhack.Player.lines;
	j["wallhack"]["player"]["box"] = Wallhack.Player.box;
	j["wallhack"]["player"]["collision"] = Wallhack.Player.collision;
	j["wallhack"]["player"]["skeleton"] = Wallhack.Player.skeleton;
	j["wallhack"]["player"]["modelID"] = Wallhack.Player.model_id;
	j["wallhack"]["player"]["nametags"] = Wallhack.Player.nametags;
	jvec4(j["wallhack"]["player"]["linesCfg"], "color", Wallhack.Player.Config.Lines.color);
	j["wallhack"]["player"]["linesCfg"]["tick"] = Wallhack.Player.Config.Lines.tickness;
	jvec4(j["wallhack"]["player"]["boxCfg"], "color", Wallhack.Player.Config.Box.color);
	j["wallhack"]["player"]["boxCfg"]["tick"] = Wallhack.Player.Config.Box.tickness;
	jvec4(j["wallhack"]["player"]["skelCfg"], "color", Wallhack.Player.Config.Skeleton.color);
	j["wallhack"]["player"]["skelCfg"]["tick"] = Wallhack.Player.Config.Skeleton.tickness;
	
	j["wallhack"]["vehicle"]["lines"] = Wallhack.Vehicle.lines;
	j["wallhack"]["vehicle"]["box"] = Wallhack.Vehicle.box;
	j["wallhack"]["vehicle"]["collision"] = Wallhack.Vehicle.collision;
	j["wallhack"]["vehicle"]["modelID"] = Wallhack.Vehicle.model_id;
	j["wallhack"]["vehicle"]["modelName"] = Wallhack.Vehicle.model_name;
	jvec4(j["wallhack"]["vehicle"]["linesCfg"], "color", Wallhack.Vehicle.Config.Lines.color);
	j["wallhack"]["vehicle"]["linesCfg"]["tick"] = Wallhack.Vehicle.Config.Lines.tickness;
	
	j["wallhack"]["object"]["lines"] = Wallhack.Object.lines;
	j["wallhack"]["object"]["box"] = Wallhack.Object.box;
	j["wallhack"]["object"]["collision"] = Wallhack.Object.collision;
	j["wallhack"]["object"]["modelID"] = Wallhack.Object.model_id;
	j["wallhack"]["object"]["modelName"] = Wallhack.Object.model_name;
	j["wallhack"]["object"]["filter"] = Wallhack.Object.filter;
	jvec4(j["wallhack"]["object"]["linesCfg"], "color", Wallhack.Object.Config.Lines.color);
	j["wallhack"]["object"]["linesCfg"]["tick"] = Wallhack.Object.Config.Lines.tickness;
	
	j["aimbot"]["state"] = Aimbot.state;
	j["aimbot"]["customCrosshair"] = Aimbot.show_custom_crosshair;
	j["aimbot"]["targetBone"] = Aimbot.target_bone_id;
	j["aimbot"]["aimXOffset"] = Aimbot.aimXOffset;
	j["aimbot"]["aimYOffset"] = Aimbot.aimYOffset;
	j["aimbot"]["fov"] = Aimbot.fov;
	j["aimbot"]["fovTick"] = Aimbot.fov_tickness;
	j["aimbot"]["targetTick"] = Aimbot.target_tickness;
	j["aimbot"]["crosshairSize"] = Aimbot.custom_crosshair_size;
	
	j["autofarm"]["antiAfk"] = AutoFarm.AntiAfk.state;
	j["autofarm"]["botMiner"] = AutoFarm.BotMiner.state;
	j["autofarm"]["botTrucker"] = AutoFarm.BotTrucker.state;
	j["autofarm"]["botBus"] = AutoFarm.BotBus.state;
	
	j["nops"]["clearAnimation"] = NOPs.clearAnimation;
	j["nops"]["vehicleVelocity"] = NOPs.vehicleVelocity;
	
	j["ui"]["iconSize"] = ::icon_size;
	j["ui"]["alphaWindowBg"] = ::alphaWindowBg;
	j["ui"]["alphaChildBg"] = ::alphaChildBg;
	j["ui"]["hideIcon"] = ::hideIcon;
	j["ui"]["crosshairX"] = ::crosshair_offset_x;
	j["ui"]["crosshairY"] = ::crosshair_offset_y;
	
	std::ofstream out(std::string(ONE_SETTINGS_PATH));
	if(out.is_open())
	{
		out << j.dump(2);
	}
}

#define LJ(sec, key, var) do { \
		auto it = j[sec].find(key); \
		if(it != j[sec].end() && !it->is_null()) var = it->get<decltype(var)>(); \
	} while(0)

void Cheat::LoadSettings()
{
	std::ifstream file(std::string(ONE_SETTINGS_PATH));
	if(!file.is_open()) { return; }
	
	std::string content((std::istreambuf_iterator<char>(file)),
						std::istreambuf_iterator<char>());
	file.close();
	if(content.empty()) { return; }
	
	try {
		nlohmann::json j = nlohmann::json::parse(content);
		
		LJ("player", "slapUp", PlayerCheat.Base.Slap.up);
		LJ("player", "slapDown", PlayerCheat.Base.Slap.down);
		LJ("player", "flyPed", PlayerCheat.Base.FlyPed.state);
		LJ("player", "flyPedSpeed", PlayerCheat.Base.FlyPed.speed);
		LJ("player", "invHealth", PlayerCheat.Base.Invulnerability.health);
		LJ("player", "invEndurance", PlayerCheat.Base.Invulnerability.endurance);
		LJ("player", "walkUnderwater", PlayerCheat.Base.Invulnerability.walkUnderwater);
		LJ("player", "noBike", PlayerCheat.Base.Invulnerability.noBike);
		LJ("player", "jumpHigh", PlayerCheat.Base.Jumping.high);
		LJ("player", "jumpLong", PlayerCheat.Base.Jumping.long_);
		LJ("player", "jumpLongVal", PlayerCheat.Base.Jumping.long__);
		LJ("player", "jumpHighVal", PlayerCheat.Base.Jumping.high__);
		LJ("player", "fastRotate", PlayerCheat.Base.Control.fastRotate);
		LJ("player", "rotateRate", PlayerCheat.Base.Control.rate);
		
		LJ("rage", "noReload", PlayerCheat.Rage.NoReload.state);
		LJ("rage", "aimBot", PlayerCheat.Rage.AimBot.state);
		LJ("rage", "autoHeal", PlayerCheat.Rage.Invulnerability.autoHeal);
		LJ("rage", "autoArmour", PlayerCheat.Rage.Invulnerability.autoArmour);
		LJ("rage", "damager", PlayerCheat.Rage.Damager.state);
		LJ("rage", "damagerTarget", PlayerCheat.Rage.Damager.targetID);
		
		LJ("vehicle", "flyCar", VehicleCheat.FlyCar.state);
		LJ("vehicle", "flyCarSpeed", VehicleCheat.FlyCar.speed);
		LJ("vehicle", "carShot", VehicleCheat.CarShot.state);
		LJ("vehicle", "carShotSpeed", VehicleCheat.CarShot.speed);
		LJ("vehicle", "carShotAccel", VehicleCheat.CarShot.accelerate);
		LJ("vehicle", "airBrake", VehicleCheat.AirBrake.state);
		LJ("vehicle", "invHealth", VehicleCheat.Invulnerability.health);
		LJ("vehicle", "invEngine", VehicleCheat.Invulnerability.engine);
		LJ("vehicle", "driveUnderwater", VehicleCheat.Invulnerability.driveUnderwater);
		LJ("vehicle", "antiDetach", VehicleCheat.AntiDetachTrailer.state);
		LJ("vehicle", "carChangerModel", VehicleCheat.CarChanger.modelID);
		
		LJ("visual", "timeFreeze", VisualCheat.TimeChanger.freeze);
		LJ("visual", "time", VisualCheat.TimeChanger.time);
		LJ("visual", "fov", VisualCheat.Camera.FOV);
		LJ("visual", "freeCam", VisualCheat.Camera.FreeCam.state);
		LJ("visual", "freeCamSpeed", VisualCheat.Camera.FreeCam.speed);
		LJ("visual", "skinID", VisualCheat.SkinChanger.value);
		
		LJ("flooder", "delay", Flooder.delay_after_message);
		LJ("flooder", "message", Flooder.message);
		
		LJ("gameSpeed", "state", GameSpeed.state);
		LJ("gameSpeed", "speed", GameSpeed.speed);
		
		if(j.contains("tuning"))
		{
			auto& t = j["tuning"];
			for(int i = 0; i < 3; i++)
			{
				if(t.contains("bodyColor"))    Tuning.body_color[i] = t["bodyColor"][i];
				if(t.contains("wheelsColor"))  Tuning.wheels_color[i] = t["wheelsColor"][i];
				if(t.contains("lightsColor"))  Tuning.lights_color[i] = t["lightsColor"][i];
				if(t.contains("glowColor"))    Tuning.glow_color[i] = t["glowColor"][i];
				if(t.contains("rightGlow"))    Tuning.right_glow_color[i] = t["rightGlow"][i];
				if(t.contains("leftGlow"))     Tuning.left_glow_color[i] = t["leftGlow"][i];
			}
			LJ("tuning", "stroboscopes", Tuning.stroboscopes);
			LJ("tuning", "stroboscopesType", Tuning.stroboscopesType);
			LJ("tuning", "longLight", Tuning.longLight);
			LJ("tuning", "totalClearance", Tuning.totalClearance);
			LJ("tuning", "separateClearance", Tuning.separateClearance);
			LJ("tuning", "totalClearanceVal", Tuning.total_clearance_val);
			LJ("tuning", "separateClearanceVal", Tuning.separate_clearance_val);
			LJ("tuning", "wheelsRadius", Tuning.wheels_radius);
		}
		
		LJ("teleport", "toMarker", Teleport.toMarker);
		LJ("teleport", "toCheckpoint", Teleport.toCheckpoint);
		LJ("teleport", "toPeds", Teleport.toPeds);
		LJ("teleport", "toCars", Teleport.toCars);
		LJ("teleport", "onFootSpeedMax", Teleport.OnFoot.speed_max);
		LJ("teleport", "inCarSpeedMax", Teleport.InCar.speed_max);
		
		LJ("collision", "ped", Collision.ped);
		LJ("collision", "vehicle", Collision.vehicle);
		LJ("collision", "objects", Collision.objects);
		
		LJ("carSpawn", "modelID", CarSpawn.modelID);
		
		auto lvec4 = [](nlohmann::json& c, ImVec4& v) {
			if(c.contains("color"))
			{
				v.x = c["color"].value("x", v.x);
				v.y = c["color"].value("y", v.y);
				v.z = c["color"].value("z", v.z);
				v.w = c["color"].value("w", v.w);
			}
		};
		
		if(j.contains("wallhack"))
		{
			auto& w = j["wallhack"];
			if(w.contains("player"))
			{
				auto& p = w["player"];
				if(p.contains("lines")) Wallhack.Player.lines = p["lines"];
				if(p.contains("box")) Wallhack.Player.box = p["box"];
				if(p.contains("collision")) Wallhack.Player.collision = p["collision"];
				if(p.contains("skeleton")) Wallhack.Player.skeleton = p["skeleton"];
				if(p.contains("modelID")) Wallhack.Player.model_id = p["modelID"];
				if(p.contains("nametags")) Wallhack.Player.nametags = p["nametags"];
				if(p.contains("linesCfg"))
				{
					lvec4(p["linesCfg"], Wallhack.Player.Config.Lines.color);
					Wallhack.Player.Config.Lines.tickness = p["linesCfg"].value("tick", Wallhack.Player.Config.Lines.tickness);
				}
				if(p.contains("boxCfg"))
				{
					lvec4(p["boxCfg"], Wallhack.Player.Config.Box.color);
					Wallhack.Player.Config.Box.tickness = p["boxCfg"].value("tick", Wallhack.Player.Config.Box.tickness);
				}
				if(p.contains("skelCfg"))
				{
					lvec4(p["skelCfg"], Wallhack.Player.Config.Skeleton.color);
					Wallhack.Player.Config.Skeleton.tickness = p["skelCfg"].value("tick", Wallhack.Player.Config.Skeleton.tickness);
				}
			}
			if(w.contains("vehicle"))
			{
				auto& v = w["vehicle"];
				if(v.contains("lines")) Wallhack.Vehicle.lines = v["lines"];
				if(v.contains("box")) Wallhack.Vehicle.box = v["box"];
				if(v.contains("collision")) Wallhack.Vehicle.collision = v["collision"];
				if(v.contains("modelID")) Wallhack.Vehicle.model_id = v["modelID"];
				if(v.contains("modelName")) Wallhack.Vehicle.model_name = v["modelName"];
				if(v.contains("linesCfg"))
				{
					lvec4(v["linesCfg"], Wallhack.Vehicle.Config.Lines.color);
					Wallhack.Vehicle.Config.Lines.tickness = v["linesCfg"].value("tick", Wallhack.Vehicle.Config.Lines.tickness);
				}
			}
			if(w.contains("object"))
			{
				auto& o = w["object"];
				if(o.contains("lines")) Wallhack.Object.lines = o["lines"];
				if(o.contains("box")) Wallhack.Object.box = o["box"];
				if(o.contains("collision")) Wallhack.Object.collision = o["collision"];
				if(o.contains("modelID")) Wallhack.Object.model_id = o["modelID"];
				if(o.contains("modelName")) Wallhack.Object.model_name = o["modelName"];
				if(o.contains("filter")) Wallhack.Object.filter = o["filter"].get<std::vector<unsigned short>>();
				if(o.contains("linesCfg"))
				{
					lvec4(o["linesCfg"], Wallhack.Object.Config.Lines.color);
					Wallhack.Object.Config.Lines.tickness = o["linesCfg"].value("tick", Wallhack.Object.Config.Lines.tickness);
				}
			}
		}
		
		LJ("aimbot", "state", Aimbot.state);
		LJ("aimbot", "customCrosshair", Aimbot.show_custom_crosshair);
		LJ("aimbot", "targetBone", Aimbot.target_bone_id);
		LJ("aimbot", "aimXOffset", Aimbot.aimXOffset);
		LJ("aimbot", "aimYOffset", Aimbot.aimYOffset);
		LJ("aimbot", "fov", Aimbot.fov);
		LJ("aimbot", "fovTick", Aimbot.fov_tickness);
		LJ("aimbot", "targetTick", Aimbot.target_tickness);
		LJ("aimbot", "crosshairSize", Aimbot.custom_crosshair_size);
		
		LJ("autofarm", "antiAfk", AutoFarm.AntiAfk.state);
		LJ("autofarm", "botMiner", AutoFarm.BotMiner.state);
		LJ("autofarm", "botTrucker", AutoFarm.BotTrucker.state);
		LJ("autofarm", "botBus", AutoFarm.BotBus.state);
		
		LJ("nops", "clearAnimation", NOPs.clearAnimation);
		LJ("nops", "vehicleVelocity", NOPs.vehicleVelocity);
		
		LJ("ui", "iconSize", ::icon_size);
		LJ("ui", "alphaWindowBg", ::alphaWindowBg);
		LJ("ui", "alphaChildBg", ::alphaChildBg);
		LJ("ui", "hideIcon", ::hideIcon);
		LJ("ui", "crosshairX", ::crosshair_offset_x);
		LJ("ui", "crosshairY", ::crosshair_offset_y);

		// all cheat toggles start OFF after (re)launch — only config values persist
		PlayerCheat.Base.FlyPed.state = false;
		PlayerCheat.Base.Invulnerability.health = false;
		PlayerCheat.Base.Invulnerability.endurance = false;
		PlayerCheat.Base.Invulnerability.walkUnderwater = false;
		PlayerCheat.Base.Invulnerability.noBike = false;
		PlayerCheat.Base.Jumping.high = false;
		PlayerCheat.Base.Jumping.long_ = false;
		PlayerCheat.Base.Control.fastRotate = false;
		PlayerCheat.Rage.NoReload.state = false;
		PlayerCheat.Rage.AimBot.state = false;
		PlayerCheat.Rage.Invulnerability.autoHeal = false;
		PlayerCheat.Rage.Invulnerability.autoArmour = false;
		PlayerCheat.Rage.Damager.state = false;
		VehicleCheat.FlyCar.state = false;
		VehicleCheat.CarShot.state = false;
		VehicleCheat.AirBrake.state = false;
		VehicleCheat.Invulnerability.health = false;
		VehicleCheat.Invulnerability.engine = false;
		VehicleCheat.Invulnerability.driveUnderwater = false;
		VehicleCheat.AntiDetachTrailer.state = false;
		VisualCheat.TimeChanger.freeze = false;
		VisualCheat.Camera.FreeCam.state = false;
		GameSpeed.state = false;
		Teleport.toMarker = false;
		Teleport.toCheckpoint = false;
		Teleport.toPeds = false;
		Teleport.toCars = false;
		Collision.ped = false;
		Collision.vehicle = false;
		Collision.objects = false;
		Wallhack.Player.lines = false;
		Wallhack.Player.box = false;
		Wallhack.Player.collision = false;
		Wallhack.Player.skeleton = false;
		Wallhack.Player.model_id = false;
		Wallhack.Player.nametags = false;
		Wallhack.Vehicle.lines = false;
		Wallhack.Vehicle.box = false;
		Wallhack.Vehicle.collision = false;
		Wallhack.Vehicle.model_id = false;
		Wallhack.Object.lines = false;
		Wallhack.Object.box = false;
		Wallhack.Object.collision = false;
		Wallhack.Object.model_id = false;
		Aimbot.state = false;
		AutoFarm.AntiAfk.state = false;
		AutoFarm.BotMiner.state = false;
		AutoFarm.BotTrucker.state = false;
		AutoFarm.BotBus.state = false;
		AutoFarm.BotCourier.state = false;
		AutoFarm.BotFarm.state = false;
		AutoFarm.BotFactory.state = false;
		NOPs.clearAnimation = false;
		NOPs.vehicleVelocity = false;

		settings_loaded = true;
	} catch (...) {}
}
