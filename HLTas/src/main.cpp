// HLTas — этап 2: оверлей + управление временем + свои бинды для Half-Life 1 (GoldSrc, OpenGL)
//  * цепляется к SwapBuffers и рисует поверх игры меню и HUD на ImGui (неоновая тема)
//  * получает данные игрока через хук HUD_PlayerMove из client.dll
//  * замедление — на стороне движка: подменяем источники времени Windows для кода hw.dll
//  * пауза/шаг — через встроенную паузу движка (команда pause через pfnClientCmd)
//  * движение: автобанихоп, автострафе (CL_CreateMove), дак-спам на земле
//  * клавиши назначаются в меню и сохраняются в hltas.ini рядом с hl.exe
//
// Только для одиночной игры. В онлайн с этим не заходить.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <functional>
#include <string>
#include <algorithm>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#define CALLER() _ReturnAddress()
#define NOINLINE __declspec(noinline)
#else
#define CALLER() __builtin_return_address(0)
#define NOINLINE __attribute__((noinline))
#endif

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_win32.h"
#include "neon_theme.h"

#ifdef _MSC_VER
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

// объявлена в imgui_impl_win32.cpp, но закрыта #if 0 в заголовке
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------- лог
static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "hltas.log", "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- playermove_t (начало структуры из HLSDK)
struct Vec3 { float x, y, z; };

struct PlayerMove
{
    int   player_index;
    int   server;
    int   multiplayer;
    float time;
    float frametime;
    Vec3  forward, right, up;
    Vec3  origin;
    Vec3  angles;
    Vec3  oldangles;
    Vec3  velocity;
    Vec3  movedir;
    Vec3  basevelocity;
    Vec3  view_ofs;
    float flDuckTime;
    int   bInDuck;
    int   flTimeStepSound;
    int   iStepLeft;
    float flFallVelocity;
    Vec3  punchangle;
    float flSwimTime;
    float flNextPrimaryAttack;
    int   effects;
    int   flags;
    int   usehull;
    float gravity;
    float friction;
    int   oldbuttons;
    float waterjumptime;
    int   dead;
    int   deadflag;
    int   spectator;
    int   movetype;
    int   onground;
    int   waterlevel;
};

// страховка: если смещения не совпали, сборка остановится, а не выдаст мусор в игре
static_assert(offsetof(PlayerMove, origin) == 56, "origin offset");
static_assert(offsetof(PlayerMove, velocity) == 92, "velocity offset");
static_assert(offsetof(PlayerMove, flags) == 184, "flags offset");
static_assert(offsetof(PlayerMove, onground) == 224, "onground offset");

static const int FL_ONGROUND = (1 << 9);
static const int FL_DUCKING = (1 << 14);

// usercmd_t из HLSDK (то, что клиент отправляет движку каждый кадр)
struct UserCmd
{
    short          lerp_msec;
    unsigned char  msec;
    Vec3           viewangles;
    float          forwardmove;
    float          sidemove;
    float          upmove;
    unsigned char  lightlevel;
    unsigned short buttons;
    unsigned char  impulse;
    unsigned char  weaponselect;
    int            impact_index;
    Vec3           impact_position;
};
static_assert(offsetof(UserCmd, viewangles) == 4, "viewangles offset");
static_assert(offsetof(UserCmd, forwardmove) == 16, "forwardmove offset");
static_assert(offsetof(UserCmd, sidemove) == 20, "sidemove offset");
static_assert(offsetof(UserCmd, buttons) == 30, "buttons offset");

static const unsigned short IN_JUMP = (1 << 1);
static const unsigned short IN_DUCK = (1 << 2);
static const unsigned short IN_FORWARD = (1 << 3);
static const unsigned short IN_BACK = (1 << 4);
static const unsigned short IN_MOVELEFT = (1 << 9);
static const unsigned short IN_MOVERIGHT = (1 << 10);

// ===== RECORDER-DATA-BEGIN ===========================================================
// Один кадр записи: достаточно, чтобы нарисовать путь и потом воспроизвести ввод.
struct Frame
{
    float t;                    // время от начала записи, сек
    Vec3  pos, vel;
    float yaw, pitch;           // взгляд из usercmd
    float fwd, side;            // forwardmove / sidemove
    unsigned short buttons;
    unsigned char  msec;
    unsigned char  flags;       // FR_GROUND | FR_DUCK
};
static_assert(sizeof(Frame) == 48, "Frame must be 48 bytes");
static const unsigned char FR_GROUND = 1;
static const unsigned char FR_DUCK = 2;

struct RunStats { float time = 0; int frames = 0; float distance = 0, top = 0, avg = 0; int jumps = 0; };
struct Run { std::string name; std::string map; std::vector<Frame> frames; RunStats st; };
struct RunInfo { std::string name, path, map; int frames = 0; float time = 0; };

static bool   g_rec = false;                 // идёт запись
static Run    g_recRun;                      // записываемый сейчас забег
static float  g_recClock = 0.0f;
static Run    g_cur;                         // открытый в просмотрщике забег
static bool   g_curLoaded = false;
static bool   g_curDirty = false;
static int    g_curVer = 0;                  // растёт при смене забега (сброс камеры)
static std::string g_curPath, g_selectPath;
static std::vector<RunInfo> g_runs;
static char   g_nextName[48] = "";
static char   g_runFilter[48] = "";
static int    g_runSort = 0;                 // 0 = по имени, 1 = по длине
static int    g_viewMode = 0;                // 0 = Orbit, 1 = Top, 2 = POV
static int    g_playhead = 0;
static bool   g_play = false;
static float  g_playAccum = 0.0f;
static float  g_orbYaw = 40.0f, g_orbPitch = 32.0f, g_orbZoom = 2.2f;
static bool   g_trailShow = true;            // рисовать путь в игровом мире
static bool   g_trailThrough = false;        // ...сквозь стены
static bool   g_jumpStatsOn = false;

// воспроизведение: повторяем записанный ввод кадр за кадром
static bool   g_replayActive = false;
static int    g_replayIdx = 0;
static Run    g_replayRun;
static int    g_replayStartMode = 0;         // 0 = дойти до старта пешком, 1 = телепорт, 2 = стоять на старте самому
static float  g_replayMaxDist = 300.0f;      // «рядом» — не дальше этого расстояния
static volatile double g_scale = 1.0;       // скорость времени (замедление)
static bool   g_replayLockstep = false;      // экспериментально: время игры идёт ровно по длине кадров записи
static bool   g_leadIn = true;               // в запись входит разбег с последней остановки
static std::vector<Frame> g_lead;            // кадры с момента, когда игрок последний раз стоял на месте
static int    g_recLead = 0;                 // сколько кадров разбега попало в текущую запись
static bool   g_leadTrunc = false;           // разбег был длиннее буфера и обрезан
static int    g_replayPhase = 0;             // 0 = нет, 1 = идём к старту, 2 = воспроизведение
static float  g_walkDist = 0.0f;
static int    g_waitFrames = 0;
static bool   g_replayFpsChanged = false;
static int    g_lsWatchdog = 0;
static int    g_replayPace = 0;              // 0 = в реальном времени, 1 = как можно быстрее
static float  g_replaySpeed = 0.0f;          // скорость повтора: время записи / реальное время
static unsigned long long g_spdRealT0 = 0, g_spdStartReal = 0;
static int    g_spdGameMs = 0, g_spdTotalGameMs = 0;
static std::string g_vsyncSaved;
static bool   g_vsyncChanged = false;
static unsigned long long g_walkStartTick = 0, g_walkBestTick = 0;
static float  g_walkBest = 1e9f;
static float  g_driftNow = 0.0f, g_driftMax = 0.0f;
static int    g_driftMaxIdx = 0, g_driftFirstIdx = -1;
static bool   g_stopOnDrift = false;
static float  g_driftLimit = 3.0f;
static bool   g_paused = false;
static bool   g_lsOn = false;                // пошаговое время воспроизведения включено
static int    g_normalFps = 100;             // обычный fps_max, к которому возвращаемся
static int    g_lastFps = 0;
struct VisCfg { bool on; int style; float col[3]; float bright; bool through; };   // style: 0 = оттенок, 1 = плоский цвет, 2 = свечение
static VisCfg g_chams = { false, 0,{ 1.0f, 0.15f, 0.85f }, 1.5f, false };
static VisCfg g_weap = { false, 0,{ 0.0f, 0.93f, 1.0f }, 1.5f, false };
static int    g_visHits[2] = { 0, 0 };           // сколько вызовов цвета подменено (модели / оружие)
static int    g_visHitsShown[2] = { 0, 0 };
static unsigned long long g_visTick = 0;
static bool   g_glHooksOk = false;
static int    g_glColorHooks = 0;
static bool   g_hookGdiSwap = true;
static std::string g_hookedMap;              // имя карты, замеченное при открытии файла .bsp
static char   g_replayMsg[120] = "";
static float  g_replayMsgT = 0.0f;
static bool   g_teleportOk = false;          // найдены серверные функции для переноса
static bool   g_viewAnglesOk = false;        // можно ставить взгляд движку

struct JumpInfo { bool valid = false; float dist = 0, pre = 0, top = 0, height = 0; };
static JumpInfo g_jump;

// реализации с WinAPI — ниже по файлу
static bool LoadRun(const std::string& path, Run& r);
static bool SaveRun(const Run& r, std::string& path);
static void DeleteRunFile(const std::string& path);
static void RefreshRuns();
static void ToggleRecord();
static void EnsureMapFor(const std::string& name);
static void LoadMapByName(const std::string& name);
static bool StartReplay();
static void StopReplay(const char* why);

static float Speed2D(const Frame& f) { return sqrtf(f.vel.x * f.vel.x + f.vel.y * f.vel.y); }

static void ComputeStats(Run& r)
{
    RunStats st;
    st.frames = (int)r.frames.size();
    float t = 0.0f;
    for (size_t i = 0; i < r.frames.size(); ++i) {
        Frame& f = r.frames[i];
        f.t = t;
        t += f.msec * 0.001f;
        float sp = Speed2D(f);
        if (sp > st.top) st.top = sp;
        if (i > 0) {
            const Frame& q = r.frames[i - 1];
            float dx = f.pos.x - q.pos.x, dy = f.pos.y - q.pos.y, dz = f.pos.z - q.pos.z;
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < 200.0f) st.distance += d;                         // отсекаем телепорты
            if ((q.flags & FR_GROUND) && !(f.flags & FR_GROUND) && f.vel.z > 200.0f) ++st.jumps;
        }
    }
    st.time = t;
    st.avg = t > 0.0f ? st.distance / t : 0.0f;
    r.st = st;
}

// тепловая шкала скорости: синий -> зелёный -> жёлтый -> красный
static ImVec4 HeatColor(float speed)
{
    float t = speed / 600.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float r, g, b;
    if (t < 0.33f) { float k = t / 0.33f;          r = 0.15f;              g = 0.45f + 0.45f * k; b = 1.00f - 0.70f * k; }
    else if (t < 0.66f) { float k = (t - 0.33f) / 0.33f; r = 0.15f + 0.85f * k;  g = 0.90f;             b = 0.30f * (1.0f - k); }
    else { float k = (t - 0.66f) / 0.34f; r = 1.00f;              g = 0.90f - 0.65f * k; b = 0.05f; }
    return ImVec4(r, g, b, 1.0f);
}

// ---- карта: чтение BSP30 (Half-Life) и построение сетки для окна рекордера
struct MapMesh
{
    std::string name;
    std::vector<float> tri;       // по 6 float на вершину: x y z r g b
    std::vector<float> lines;     // по 3 float на вершину, пары = отрезки
    Vec3 mn = { 0, 0, 0 }, mx = { 0, 0, 0 };
    unsigned int listTri = 0, listLine = 0;   // дисплей-листы OpenGL (создаются при первой отрисовке)
    int faces = 0;
    bool ok = false;
};

static MapMesh g_map;
static std::vector<unsigned int> g_staleLists;   // старые дисплей-листы, которые надо удалить в потоке рендера
static bool   g_mapShow = true;                  // показывать карту в рекордере
static bool   g_mapCut = true;                   // срезать потолки над путём
static float  g_mapCutOff = 64.0f;               // на сколько выше пути срезать
static char   g_mapMsg[140] = "";
typedef const char* (*GetCvarString_t)(const char*);
static GetCvarString_t g_getCvarString = nullptr;

static bool ParseBsp(const std::vector<unsigned char>& d, MapMesh& out, std::string& err)
{
    if (d.size() < 4 + 15 * 8) { err = "file is too small"; return false; }
    auto rd32 = [&](size_t o) -> int { int v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; };
    int ver = rd32(0);
    if (ver != 30) { err = "unsupported BSP version " + std::to_string(ver); return false; }

    int lo[15], ln[15];
    for (int i = 0; i < 15; ++i) {
        lo[i] = rd32(4 + i * 8);
        ln[i] = rd32(4 + i * 8 + 4);
        if (lo[i] < 0 || ln[i] < 0 || (size_t)lo[i] + (size_t)ln[i] > d.size()) { err = "corrupted lump table"; return false; }
    }
    const int L_PLANES = 1, L_TEXTURES = 2, L_VERTICES = 3, L_TEXINFO = 6, L_FACES = 7, L_EDGES = 12, L_SURFEDGES = 13, L_MODELS = 14;
    size_t nv = ln[L_VERTICES] / 12, np = ln[L_PLANES] / 20, ne = ln[L_EDGES] / 4, ns = ln[L_SURFEDGES] / 4;
    size_t nf = ln[L_FACES] / 20, nti = ln[L_TEXINFO] / 40, nm = ln[L_MODELS] / 64;
    if (nm < 1) { err = "no world model"; return false; }

    // имена текстур
    std::vector<std::string> texNames;
    if (ln[L_TEXTURES] >= 4) {
        int cnt = rd32(lo[L_TEXTURES]);
        if (cnt > 0 && (size_t)cnt < 100000 && 4 + (size_t)cnt * 4 <= (size_t)ln[L_TEXTURES]) {
            for (int i = 0; i < cnt; ++i) {
                int off = rd32(lo[L_TEXTURES] + 4 + i * 4);
                std::string nm2;
                if (off > 0 && (size_t)off + 16 <= (size_t)ln[L_TEXTURES]) {
                    const char* c = (const char*)&d[lo[L_TEXTURES] + off];
                    for (int k = 0; k < 16 && c[k]; ++k) nm2 += (char)tolower((unsigned char)c[k]);
                }
                texNames.push_back(nm2);
            }
        }
    }

    size_t m0 = lo[L_MODELS];
    int firstFace = rd32(m0 + 56), numFaces = rd32(m0 + 60);
    if (firstFace < 0 || numFaces < 0 || (size_t)firstFace + (size_t)numFaces > nf) { err = "bad world model"; return false; }

    const unsigned char* base = d.data();
    const float* verts = (const float*)(base + lo[L_VERTICES]);
    const unsigned char* planes = base + lo[L_PLANES];
    const unsigned short* edges = (const unsigned short*)(base + lo[L_EDGES]);
    const int* surfedges = (const int*)(base + lo[L_SURFEDGES]);
    const unsigned char* faces = base + lo[L_FACES];
    const unsigned char* texinfo = base + lo[L_TEXINFO];

    out = MapMesh();
    bool first = true;
    const float lx = 0.35f, ly = 0.50f, lz = 0.80f;
    const float ll = sqrtf(lx * lx + ly * ly + lz * lz);
    std::vector<Vec3> poly;

    for (int fi = firstFace; fi < firstFace + numFaces; ++fi) {
        const unsigned char* f = faces + (size_t)fi * 20;
        unsigned short planenum; short side; int firstedge; short numedges; short ti;
        memcpy(&planenum, f + 0, 2); memcpy(&side, f + 2, 2); memcpy(&firstedge, f + 4, 4);
        memcpy(&numedges, f + 8, 2); memcpy(&ti, f + 10, 2);
        if (numedges < 3 || firstedge < 0 || (size_t)firstedge + (size_t)numedges > ns) continue;
        if (planenum >= np || ti < 0 || (size_t)ti >= nti) continue;

        int mip = 0;
        memcpy(&mip, texinfo + (size_t)ti * 40 + 32, 4);
        std::string tn = (mip >= 0 && (size_t)mip < texNames.size()) ? texNames[mip] : std::string();
        if (tn.compare(0, 3, "sky") == 0 || tn == "aaatrigger" || tn == "clip" || tn == "null" || tn == "origin" ||
            tn == "hint" || tn == "skip" || tn == "bevel" || tn == "black_hidden") continue;

        poly.clear();
        bool bad = false;
        for (int j = 0; j < numedges; ++j) {
            int se = surfedges[firstedge + j];
            size_t ei = (size_t)(se >= 0 ? se : -se);
            if (ei >= ne) { bad = true; break; }
            unsigned short vi = edges[ei * 2 + (se >= 0 ? 0 : 1)];
            if (vi >= nv) { bad = true; break; }
            Vec3 v = { verts[vi * 3], verts[vi * 3 + 1], verts[vi * 3 + 2] };
            poly.push_back(v);
            if (first) { out.mn = v; out.mx = v; first = false; }
            out.mn.x = fminf(out.mn.x, v.x); out.mn.y = fminf(out.mn.y, v.y); out.mn.z = fminf(out.mn.z, v.z);
            out.mx.x = fmaxf(out.mx.x, v.x); out.mx.y = fmaxf(out.mx.y, v.y); out.mx.z = fmaxf(out.mx.z, v.z);
        }
        if (bad) continue;

        float nx, ny, nz;
        memcpy(&nx, planes + (size_t)planenum * 20 + 0, 4);
        memcpy(&ny, planes + (size_t)planenum * 20 + 4, 4);
        memcpy(&nz, planes + (size_t)planenum * 20 + 8, 4);
        if (side) { nx = -nx; ny = -ny; nz = -nz; }
        float dl = (nx * lx + ny * ly + nz * lz) / ll;
        if (dl < 0.0f) dl = 0.0f;
        float shade = 0.30f + 0.62f * dl;
        float r = 0.64f, g = 0.68f, b = 0.76f;
        if (!tn.empty() && (tn[0] == '!' || tn.compare(0, 5, "water") == 0)) { r = 0.30f; g = 0.50f; b = 0.90f; }
        r *= shade; g *= shade; b *= shade;

        for (size_t j = 1; j + 1 < poly.size(); ++j) {
            const Vec3* t3[3] = { &poly[0], &poly[j], &poly[j + 1] };
            for (int k = 0; k < 3; ++k) {
                out.tri.push_back(t3[k]->x); out.tri.push_back(t3[k]->y); out.tri.push_back(t3[k]->z);
                out.tri.push_back(r); out.tri.push_back(g); out.tri.push_back(b);
            }
        }
        for (size_t j = 0; j < poly.size(); ++j) {
            const Vec3& a = poly[j];
            const Vec3& c = poly[(j + 1) % poly.size()];
            out.lines.push_back(a.x); out.lines.push_back(a.y); out.lines.push_back(a.z);
            out.lines.push_back(c.x); out.lines.push_back(c.y); out.lines.push_back(c.z);
        }
        ++out.faces;
    }
    if (out.tri.empty()) { err = "map has no drawable faces"; return false; }
    out.ok = true;
    return true;
}

// имя текущей карты: сначала из cvar (host_map, mapname), потом из замеченного открытия файла .bsp
static std::string SanitizeMapName(const char* v)
{
    if (!v) return std::string();
    std::string s;
    for (int i = 0; i < 120 && v[i]; ++i) s += v[i];
    size_t sl = s.find_last_of("/\\");
    if (sl != std::string::npos) s = s.substr(sl + 1);
    size_t dot = s.rfind('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    std::string o;
    for (char c : s) if (isalnum((unsigned char)c) || c == '_' || c == '-') o += c;
    return o.size() > 31 ? o.substr(0, 31) : o;
}

static std::string CurrentMapName()
{
    if (g_getCvarString) {
        static const char* names[2] = { "host_map", "mapname" };
        for (int i = 0; i < 2; ++i) {
            std::string r = SanitizeMapName(g_getCvarString(names[i]));
            if (!r.empty()) return r;
        }
    }
    return g_hookedMap;
}
// ===== RECORDER-DATA-END =============================================================


// ---------------------------------------------------------------- состояние
static volatile PlayerMove* g_pm = nullptr;      // последний указатель из HUD_PlayerMove
static volatile ULONGLONG   g_pmTick = 0;        // когда он обновлялся в последний раз

// данные игрока «свежие»: при сильном замедлении кадры идут редко, поэтому допуск растёт
static inline bool PmFresh()
{
    double sc = g_scale > 0.05 ? g_scale : 0.05;
    return (double)(GetTickCount64() - g_pmTick) < 500.0 / sc;
}

static bool    g_init = false;
static bool    g_menu = false;
static bool    g_inSwap = false;
static HWND    g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static POINT   g_center = { 0, 0 };
static bool    g_passthru = false;               // true — GetCursorPos отдаёт настоящую позицию (для ImGui)
static bool    g_hookPlayerMove = false;

// ---------------------------------------------------------------- оригиналы функций
typedef void (*PlayerMove_t)(PlayerMove*, int);
typedef BOOL(WINAPI* Swap_t)(HDC);
typedef BOOL(WINAPI* SetCursorPos_t)(int, int);
typedef BOOL(WINAPI* GetCursorPos_t)(LPPOINT);

static PlayerMove_t   oPlayerMove = nullptr;
static Swap_t         oWglSwap = nullptr;
static Swap_t         oGdiSwap = nullptr;
static SetCursorPos_t oSetCursorPos = nullptr;
static GetCursorPos_t oGetCursorPos = nullptr;

// ---------------------------------------------------------------- рекордер: файлы и запись
struct RunHeader { char magic[4]; unsigned int frames; float time; unsigned int reserved; char map[32]; };   // AHR2, 48 байт

static std::string ExeDir()
{
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    char* sl = strrchr(p, '\\');
    if (sl) sl[1] = 0;
    return std::string(p);
}

static std::string RunDir()
{
    std::string d = ExeDir() + "archangel_runs\\";
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

static std::string SafeName(const std::string& n)
{
    std::string o;
    for (char c : n) o += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    return o.empty() ? std::string("run") : o;
}

static bool SaveRun(const Run& r, std::string& path)
{
    path = RunDir() + SafeName(r.name) + ".run";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    RunHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "AHR2", 4);
    h.frames = (unsigned int)r.frames.size();
    h.time = r.st.time;
    strncpy(h.map, r.map.c_str(), sizeof(h.map) - 1);
    fwrite(&h, sizeof(h), 1, f);
    if (!r.frames.empty()) fwrite(r.frames.data(), sizeof(Frame), r.frames.size(), f);
    fclose(f);
    return true;
}

static bool ReadHeader(FILE* f, RunHeader& h)
{
    memset(&h, 0, sizeof(h));
    if (fread(&h, 16, 1, f) != 1) return false;                         // общая часть: magic, frames, time, reserved
    if (memcmp(h.magic, "AHR2", 4) == 0) { if (fread(h.map, 32, 1, f) != 1) return false; h.map[31] = 0; }
    else if (memcmp(h.magic, "AHR1", 4) != 0) return false;           // старый формат без карты
    return h.frames < 2000000u;
}

static std::string NameFromPath(const std::string& path)
{
    size_t a = path.find_last_of("\\/");
    std::string n = path.substr(a == std::string::npos ? 0 : a + 1);
    size_t d = n.rfind('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

static bool LoadRun(const std::string& path, Run& r)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    RunHeader h;
    bool ok = ReadHeader(f, h);
    if (ok) {
        r.frames.resize(h.frames);
        ok = h.frames == 0 || fread(r.frames.data(), sizeof(Frame), h.frames, f) == h.frames;
    }
    fclose(f);
    if (!ok) { r.frames.clear(); return false; }
    r.name = NameFromPath(path);
    r.map = h.map;
    ComputeStats(r);
    return true;
}

static void DeleteRunFile(const std::string& path) { DeleteFileA(path.c_str()); }

static void RefreshRuns()
{
    g_runs.clear();
    std::string dir = RunDir();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.run").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            RunInfo ri;
            ri.path = dir + fd.cFileName;
            ri.name = NameFromPath(ri.path);
            FILE* f = nullptr;
            if (fopen_s(&f, ri.path.c_str(), "rb") == 0 && f) {
                RunHeader hd;
                if (ReadHeader(f, hd)) { ri.frames = (int)hd.frames; ri.time = hd.time; ri.map = hd.map; g_runs.push_back(ri); }
                fclose(f);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}


// ---------------------------------------------------------------- карта: перехват открытия .bsp
typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t oCreateFileA = nullptr;
static CreateFileW_t oCreateFileW = nullptr;

static void NoteBspPath(const char* p)
{
    size_t n = strlen(p);
    if (n > 4 && _stricmp(p + n - 4, ".bsp") == 0) {
        std::string nm = SanitizeMapName(p);
        if (!nm.empty() && nm != g_hookedMap) { g_hookedMap = nm; Log("map file opened: %s -> %s", p, nm.c_str()); }
    }
}
static HANDLE WINAPI hkCreateFileA(LPCSTR f, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    if (f) NoteBspPath(f);
    return oCreateFileA(f, a, sh, sa, cd, fl, t);
}
static HANDLE WINAPI hkCreateFileW(LPCWSTR f, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    if (f) {
        size_t n = wcslen(f);
        if (n > 4 && _wcsicmp(f + n - 4, L".bsp") == 0) {
            char b[300];
            if (WideCharToMultiByte(CP_ACP, 0, f, -1, b, sizeof(b), nullptr, nullptr) > 0) NoteBspPath(b);
        }
    }
    return oCreateFileW(f, a, sh, sa, cd, fl, t);
}

static void LogMapDetect()
{
    const char* a = g_getCvarString ? g_getCvarString("host_map") : nullptr;
    const char* b = g_getCvarString ? g_getCvarString("mapname") : nullptr;
    Log("map detect: host_map='%s' mapname='%s' file='%s' -> '%s'", a ? a : "(null)", b ? b : "(null)",
        g_hookedMap.c_str(), CurrentMapName().c_str());
}

// ---------------------------------------------------------------- карта: поиск файла и загрузка
static std::string FindMapFile(const std::string& name)
{
    std::string root = ExeDir();
    auto tryDir = [&](const std::string& d) -> std::string {
        std::string p = root + d + "\\maps\\" + name + ".bsp";
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : std::string();
        };
    std::string r = tryDir("valve");
    if (!r.empty()) return r;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                r = tryDir(fd.cFileName);
                if (!r.empty()) break;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return r;
}

static void LoadMapByName(const std::string& nameIn)
{
    std::string name = nameIn;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    if (name.empty()) { snprintf(g_mapMsg, sizeof(g_mapMsg), "Enter a map name"); return; }
    std::string path = FindMapFile(name);
    if (path.empty()) { snprintf(g_mapMsg, sizeof(g_mapMsg), "Map file not found: %s.bsp", name.c_str()); return; }
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) { snprintf(g_mapMsg, sizeof(g_mapMsg), "Cannot open %s", path.c_str()); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> data;
    if (sz > 0 && sz < 200 * 1024 * 1024) { data.resize((size_t)sz); if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear(); }
    fclose(f);
    MapMesh m;
    std::string err;
    if (data.empty() || !ParseBsp(data, m, err)) {
        snprintf(g_mapMsg, sizeof(g_mapMsg), "Map %s: %s", name.c_str(), err.empty() ? "read error" : err.c_str());
        return;
    }
    m.name = name;
    if (g_map.listTri) g_staleLists.push_back(g_map.listTri);
    g_map = std::move(m);
    snprintf(g_mapMsg, sizeof(g_mapMsg), "Map %s loaded: %d faces", name.c_str(), g_map.faces);
}

static void EnsureMapFor(const std::string& name)
{
    if (name.empty()) return;
    if (g_map.ok && _stricmp(g_map.name.c_str(), name.c_str()) == 0) return;
    LoadMapByName(name);
}

static void SelectRun(const RunInfo& ri)
{
    Run r;
    if (!LoadRun(ri.path, r)) return;
    g_cur = std::move(r);
    g_curLoaded = true;
    g_curDirty = false;
    g_curPath = ri.path;
    g_selectPath = ri.path;
    g_curVer++;
    g_playhead = 0;
    g_play = false;
    g_playAccum = 0.0f;
    LogMapDetect();
    EnsureMapFor(!g_cur.map.empty() ? g_cur.map : CurrentMapName());
}

static Frame MakeFrame(const PlayerMove* pm, const UserCmd* cmd, float t)
{
    Frame f;
    memset(&f, 0, sizeof(f));
    f.t = t;
    f.pos = pm->origin;
    f.vel = pm->velocity;
    f.yaw = cmd->viewangles.y;
    f.pitch = cmd->viewangles.x;
    f.fwd = cmd->forwardmove;
    f.side = cmd->sidemove;
    f.buttons = cmd->buttons;
    f.msec = cmd->msec;
    f.flags = (unsigned char)(((pm->flags & FL_ONGROUND) != 0 || pm->onground != -1 ? FR_GROUND : 0) |
        ((pm->flags & FL_DUCKING) != 0 ? FR_DUCK : 0));
    return f;
}

// «разбег»: кадры с момента, когда игрок последний раз стоял на земле без движения.
// При старте записи они становятся её началом, и повтор начинается с места, где скорость нулевая.
static void LeadInPush(const PlayerMove* pm, const UserCmd* cmd)
{
    if (!g_leadIn || !pm || !PmFresh() || g_paused || g_replayActive) return;
    if (pm->dead || pm->deadflag) { g_lead.clear(); g_leadTrunc = false; return; }
    bool ground = (pm->flags & FL_ONGROUND) != 0 || pm->onground != -1;
    float sp = sqrtf(pm->velocity.x * pm->velocity.x + pm->velocity.y * pm->velocity.y + pm->velocity.z * pm->velocity.z);
    if (ground && sp < 0.5f) { g_lead.clear(); g_leadTrunc = false; }
    if (g_lead.size() >= 6000) {                           // слишком долго в движении: отбрасываем самое старое
        g_lead.erase(g_lead.begin(), g_lead.begin() + 1000);
        g_leadTrunc = true;
    }
    g_lead.push_back(MakeFrame(pm, cmd, 0.0f));
}

static void StartRecording()
{
    g_recRun = Run();
    g_recRun.frames.reserve(8192);
    LogMapDetect();
    g_recRun.map = CurrentMapName();
    g_recClock = 0.0f;
    g_recLead = 0;
    if (g_leadIn && !g_lead.empty()) {
        g_recRun.frames = g_lead;
        for (Frame& f : g_recRun.frames) { f.t = g_recClock; g_recClock += f.msec * 0.001f; }
        g_recLead = (int)g_lead.size();
        Log("recording: run-up of %d frames from the last standstill%s", g_recLead, g_leadTrunc ? " (truncated)" : "");
    }
    g_rec = true;
}

static void StopRecording()
{
    g_rec = false;
    if (g_recRun.frames.size() < 2) { g_recRun = Run(); return; }
    Run r = std::move(g_recRun);
    g_recRun = Run();

    std::string name = g_nextName;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    RefreshRuns();
    auto exists = [](const std::string& n) {
        for (auto& ri : g_runs) if (_stricmp(ri.name.c_str(), n.c_str()) == 0) return true;
        return false;
        };
    if (name.empty()) {
        for (int i = 1; i < 10000; ++i) {
            char b[32];
            snprintf(b, sizeof(b), "run_%03d", i);
            if (!exists(b)) { name = b; break; }
        }
    }
    else if (exists(name)) {
        std::string base = name;
        for (int i = 2; i < 10000; ++i) {
            name = base + "_" + std::to_string(i);
            if (!exists(name)) break;
        }
    }
    r.name = name;
    ComputeStats(r);
    std::string path;
    if (SaveRun(r, path)) {
        g_nextName[0] = 0;
        RefreshRuns();
        g_cur = std::move(r);
        g_curLoaded = true;
        g_curDirty = false;
        g_curPath = path;
        g_selectPath = path;
        g_curVer++;
        g_playhead = 0;
        g_play = false;
        EnsureMapFor(!g_cur.map.empty() ? g_cur.map : CurrentMapName());
    }
}

static void ToggleRecord() { if (g_rec) StopRecording(); else StartRecording(); }

static void RecordFrame(const PlayerMove* pm, const UserCmd* cmd)
{
    if (!g_rec || !pm || !PmFresh()) return;
    if (g_paused) return;                                    // на паузе игра кадры не выполняет: не пишем их
    if (pm->dead || pm->deadflag) { StopRecording(); return; }
    if (g_recRun.frames.size() >= 300000) { StopRecording(); return; }
    Frame f = MakeFrame(pm, cmd, g_recClock);
    g_recClock += cmd->msec * 0.001f;
    g_recRun.frames.push_back(f);
}

// статистика прыжка: дистанция, скорость на отрыве, максимум, перепад высоты
static void UpdateJumpStats(const PlayerMove* pm)
{
    if (!g_jumpStatsOn || !pm || !PmFresh()) return;
    static bool prevGround = true, inAir = false;
    static Vec3 t0;
    static float pre = 0, top = 0;
    bool ground = (pm->flags & FL_ONGROUND) != 0 || pm->onground != -1;
    float sp = sqrtf(pm->velocity.x * pm->velocity.x + pm->velocity.y * pm->velocity.y);
    if (prevGround && !ground) { inAir = true; t0 = pm->origin; pre = sp; top = sp; }
    else if (inAir && !ground) { if (sp > top) top = sp; }
    else if (inAir && ground) {
        inAir = false;
        float dx = pm->origin.x - t0.x, dy = pm->origin.y - t0.y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d >= 30.0f) {
            g_jump.valid = true; g_jump.dist = d; g_jump.pre = pre; g_jump.top = top; g_jump.height = pm->origin.z - t0.z;
        }
    }
    prevGround = ground;
}

// путь в игровом мире: рисуем в HUD_DrawTransparentTriangles, когда движок уже настроил 3D-матрицы
typedef void (*DrawTrans_t)(void);
static DrawTrans_t oDrawTrans = nullptr;
static bool g_hookDrawTrans = false;

static void DrawTrailLines(const std::vector<Frame>& fr, float alpha)
{
    if (fr.size() < 2) return;
    size_t step = fr.size() > 6000 ? fr.size() / 6000 : 1;
    glBegin(GL_LINE_STRIP);
    for (size_t i = 0; i < fr.size(); i += step) {
        const Frame& f = fr[i];
        ImVec4 c = HeatColor(Speed2D(f));
        glColor4f(c.x, c.y, c.z, alpha);
        float feet = (f.flags & FR_DUCK) ? 18.0f : 36.0f;
        glVertex3f(f.pos.x, f.pos.y, f.pos.z - feet + 1.0f);
    }
    const Frame& l = fr.back();
    ImVec4 c = HeatColor(Speed2D(l));
    glColor4f(c.x, c.y, c.z, alpha);
    glVertex3f(l.pos.x, l.pos.y, l.pos.z - ((l.flags & FR_DUCK) ? 18.0f : 36.0f) + 1.0f);
    glEnd();
}

static void DrawTrail3D()
{
    bool haveCur = g_curLoaded && g_cur.frames.size() >= 2;
    bool haveRec = g_rec && g_recRun.frames.size() >= 2;
    if (!g_trailShow || (!haveCur && !haveRec)) return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (g_trailThrough) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glLineWidth(3.0f);
    if (haveCur) DrawTrailLines(g_cur.frames, g_rec ? 0.45f : 0.95f);
    if (haveRec) DrawTrailLines(g_recRun.frames, 1.0f);
    glPopAttrib();
}

static void hkDrawTrans(void)
{
    oDrawTrans();
    DrawTrail3D();
}

// ---------------------------------------------------------------- доступ к движку
// client.dll хранит копию таблицы функций движка (cl_enginefunc_t). Это длинный блок
// указателей, все из которых ведут внутрь hw.dll. Ищем такой блок в записываемых секциях client.dll.
// pfnClientCmd — 21-я функция таблицы (индекс 20), как в HLSDK.
typedef int (*ClientCmd_t)(char*);
static ClientCmd_t g_clientCmd = nullptr;

static float g_lastCmdYaw = 0.0f;
typedef void  (*GetViewAngles_t)(float*);
typedef void  (*SetViewAngles_t)(float*);
typedef void* (*FindEntByString_t)(void*, const char*, const char*);
typedef void  (*SetOrigin_t)(void*, const float*);
static GetViewAngles_t  g_getViewAngles = nullptr;
static SetViewAngles_t  g_setViewAngles = nullptr;
static FindEntByString_t g_findEnt = nullptr;
static SetOrigin_t      g_setOrigin = nullptr;

// ищем в записываемых секциях модуля длинный блок указателей, все из которых ведут внутрь движка
static uintptr_t* FindFuncTable(HMODULE mod, uintptr_t lo, uintptr_t hi, size_t minRun)
{
    auto nt = (PIMAGE_NT_HEADERS)((BYTE*)mod + ((PIMAGE_DOS_HEADER)mod)->e_lfanew);
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uintptr_t* p = (uintptr_t*)((BYTE*)mod + sec[i].VirtualAddress);
        size_t n = sec[i].Misc.VirtualSize / sizeof(uintptr_t);
        size_t run = 0;
        for (size_t k = 0; k < n; ++k) {
            if (p[k] >= lo && p[k] < hi) {
                if (++run == minRun) return &p[k - minRun + 1];
            }
            else {
                run = 0;
            }
        }
    }
    return nullptr;
}

// серверная таблица (enginefuncs_t в hl.dll): 14 = FindEntityByString, 28 = SetOrigin.
// hl.dll загружается вместе с картой, поэтому поиск повторяем, пока не получится.
static void TryFindServerFuncs()
{
    if (g_teleportOk) return;
    HMODULE hw = GetModuleHandleA("hw.dll");
    if (!hw) hw = GetModuleHandleA("sw.dll");
    HMODULE sv = GetModuleHandleA("hl.dll");
    if (!hw || !sv) return;
    auto hwNt = (PIMAGE_NT_HEADERS)((BYTE*)hw + ((PIMAGE_DOS_HEADER)hw)->e_lfanew);
    uintptr_t lo = (uintptr_t)hw, hi = lo + hwNt->OptionalHeader.SizeOfImage;
    uintptr_t* st = FindFuncTable(sv, lo, hi, 60);
    if (st) {
        g_findEnt = (FindEntByString_t)st[14];
        g_setOrigin = (SetOrigin_t)st[28];
        g_teleportOk = true;
        Log("server func table at %p", (void*)st);
    }
}

static bool FindEngineFuncs()
{
    HMODULE hw = GetModuleHandleA("hw.dll");
    if (!hw) hw = GetModuleHandleA("sw.dll");
    HMODULE cl = GetModuleHandleA("client.dll");
    if (!hw || !cl) { Log("engine dll or client.dll not found"); return false; }

    auto hwNt = (PIMAGE_NT_HEADERS)((BYTE*)hw + ((PIMAGE_DOS_HEADER)hw)->e_lfanew);
    uintptr_t lo = (uintptr_t)hw, hi = lo + hwNt->OptionalHeader.SizeOfImage;

    // клиентская таблица (cl_enginefunc_t): 20 = ClientCmd, 34 = GetViewAngles, 35 = SetViewAngles
    uintptr_t* table = FindFuncTable(cl, lo, hi, 50);
    if (table) {
        g_clientCmd = (ClientCmd_t)table[20];
        g_getCvarString = (GetCvarString_t)table[16];
        g_getViewAngles = (GetViewAngles_t)table[34];
        g_setViewAngles = (SetViewAngles_t)table[35];
        Log("engine func table at %p, pfnClientCmd = %p", (void*)table, (void*)g_clientCmd);
    }
    else {
        Log("engine func table not found");
    }

    TryFindServerFuncs();
    return table != nullptr;
}

static void Cmd(const char* fmt, ...)
{
    if (!g_clientCmd) return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_clientCmd(buf);
}

// ---------------------------------------------------------------- воспроизведение забега
static void ReplayMsg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_replayMsg, sizeof(g_replayMsg), fmt, ap);
    va_end(ap);
    g_replayMsgT = 4.0f;
}

static float DegDiff(float a)
{
    while (a > 180.0f)  a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

// проверяем, что GetViewAngles действительно возвращает текущий взгляд (индекс таблицы верный)
static bool VerifyViewAngles()
{
    if (!g_getViewAngles || !g_setViewAngles) return false;
    float a[3] = { 0, 0, 0 };
    g_getViewAngles(a);
    if (!(fabsf(a[0]) <= 90.5f) || !(fabsf(a[1]) <= 360.0f)) return false;
    return fabsf(DegDiff(a[1] - g_lastCmdYaw)) < 45.0f;
}

// перенос игрока в стартовую точку забега (через серверную функцию SetOrigin)
static bool TeleportToFrame(const Frame& f, const PlayerMove* pm)
{
    if (!g_teleportOk || !g_findEnt || !g_setOrigin || !pm) return false;
    void* ed = g_findEnt(nullptr, "classname", "player");
    if (!ed) return false;
    // проверка: в edict (смещение entvars = 64, origin = +8) должна лежать позиция игрока
    const float* eo = (const float*)((const char*)ed + 64 + 8);
    float err = fabsf(eo[0] - pm->origin.x) + fabsf(eo[1] - pm->origin.y) + fabsf(eo[2] - pm->origin.z);
    if (!(err < 64.0f)) { Log("teleport: edict check failed (err=%.1f)", err); return false; }
    g_setOrigin(ed, &f.pos.x);
    float* ev = (float*)((char*)ed + 64 + 32);     // pev->velocity
    ev[0] = f.vel.x; ev[1] = f.vel.y; ev[2] = f.vel.z;
    return true;
}

static void SendFps(int fps);
static void ApplySlow();
static void LsBegin(int firstMs);
static void LsEnd();
static void LsNextFrame(int ms);
static void StopReplay(const char* why)
{
    if (g_replayActive) {
        if (g_replayPhase == 2)
            ReplayMsg("Replay %s (max drift %.1f u at frame %d)", why, g_driftMax, g_driftMaxIdx);
        else
            ReplayMsg("Replay stopped: %s", why);
        {
            unsigned long long nowMs = GetTickCount64();
            float avg = (g_spdStartReal && nowMs > g_spdStartReal) ? (float)g_spdTotalGameMs / (float)(nowMs - g_spdStartReal) : 0.0f;
            Log("replay ended: %s; phase=%d frame=%d/%d max drift %.2f at %d; first drift>1u at %d; average speed x%.2f (lockstep=%d pace=%d)",
                why, g_replayPhase, g_replayIdx, (int)g_replayRun.frames.size(), g_driftMax, g_driftMaxIdx, g_driftFirstIdx,
                avg, (int)g_replayLockstep, g_replayPace);
        }
    }
    if (g_lsOn) { LsEnd(); }
    g_replayActive = false;
    g_replayPhase = 0;
    g_replayRun = Run();
    if (g_replayFpsChanged) { SendFps(g_normalFps); g_replayFpsChanged = false; }
    if (g_vsyncChanged) { Cmd("gl_vsync %s", g_vsyncSaved.c_str()); g_vsyncChanged = false; }
    ApplySlow();
}

// начало самого воспроизведения (после того как игрок встал на старт)
static void BeginPlayback()
{
    g_driftNow = 0.0f; g_driftMax = 0.0f; g_driftMaxIdx = 0; g_driftFirstIdx = -1;
    g_lsWatchdog = 0;
    g_replaySpeed = 0.0f; g_spdRealT0 = 0; g_spdGameMs = 0; g_spdTotalGameMs = 0; g_spdStartReal = 0;
    if (g_replayLockstep) {
        // вертикальная синхронизация ограничивала бы скорость кадров, пока повтор идёт в реальном времени
        const char* vs = g_getCvarString ? g_getCvarString("gl_vsync") : nullptr;
        g_vsyncSaved = (vs && vs[0]) ? vs : "1";
        Cmd("gl_vsync 0");
        g_vsyncChanged = true;
        // движок склеивает кадры короче 1/fps_max: поднимаем лимит и ждём, пока команда применится
        SendFps(1000);
        g_replayFpsChanged = true;
        g_replayPhase = 3;
        g_waitFrames = 0;
        ReplayMsg("Preparing the replay...");
    }
    else {
        g_replayPhase = 2;
        g_replayIdx = 0;
        ReplayMsg("Replay started");
    }
}

static bool FpsMaxApplied()
{
    if (!g_getCvarString) return true;
    const char* v = g_getCvarString("fps_max");
    return v && atof(v) >= 999.0;
}

static bool StartReplay()
{
    if (g_replayActive) return false;
    if (!g_curLoaded || g_cur.frames.size() < 2) { ReplayMsg("Select a run first"); return false; }
    const PlayerMove* pm = (const PlayerMove*)g_pm;
    if (!pm || !PmFresh()) { ReplayMsg("No player data: load a map and start playing"); return false; }
    if (g_rec) StopRecording();

    {
        std::string cm = CurrentMapName();
        if (!g_cur.map.empty() && !cm.empty() && _stricmp(g_cur.map.c_str(), cm.c_str()) != 0) {
            ReplayMsg("This run was recorded on map \"%s\", now \"%s\". Load that map first.", g_cur.map.c_str(), cm.c_str());
            return false;
        }
    }

    const Frame& f0 = g_cur.frames[0];
    float dx = pm->origin.x - f0.pos.x, dy = pm->origin.y - f0.pos.y, dz = pm->origin.z - f0.pos.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float dist2 = sqrtf(dx * dx + dy * dy);
    float spd = sqrtf(pm->velocity.x * pm->velocity.x + pm->velocity.y * pm->velocity.y);

    g_lead.clear();
    g_leadTrunc = false;
    g_viewAnglesOk = VerifyViewAngles();
    g_replayRun = g_cur;
    g_replayIdx = 0;
    g_replayActive = true;
    g_driftNow = 0.0f; g_driftMax = 0.0f; g_driftMaxIdx = 0; g_driftFirstIdx = -1;

    int mode = g_replayStartMode;
    if (mode == 1) {
        TryFindServerFuncs();
        if (dist > g_replayMaxDist) { ReplayMsg("Too far from the start: %.0f u (limit %.0f).", dist, g_replayMaxDist); g_replayActive = false; return false; }
        if (dist > 0.5f && !TeleportToFrame(f0, pm)) mode = 0;     // телепорт недоступен: идём пешком
        else { BeginPlayback(); return true; }
    }
    if (mode == 2) {
        if (dist > 1.0f) { ReplayMsg("Stand exactly on the start point (%.1f u away).", dist); g_replayActive = false; return false; }
        BeginPlayback();
        return true;
    }
    // mode 0: идём к старту сами
    if (dist2 > g_replayMaxDist) { ReplayMsg("Too far from the start: %.0f u (limit %.0f).", dist2, g_replayMaxDist); g_replayActive = false; return false; }
    if (fabsf(dz) > 20.0f) { ReplayMsg("The start is %.0f u above or below you. Go to the same floor first.", fabsf(dz)); g_replayActive = false; return false; }
    if (dist2 < 0.6f && spd < 10.0f) { BeginPlayback(); return true; }
    g_replayPhase = 1;
    g_walkStartTick = GetTickCount64();
    g_walkBestTick = g_walkStartTick;
    g_walkBest = 1e9f;
    g_walkDist = dist2;
    {
        float fs = sqrtf(f0.vel.x * f0.vel.x + f0.vel.y * f0.vel.y);
        if (fs > 10.0f) ReplayMsg("Walking to the start (%.0f u). Note: this run starts in motion (%.0f u/s), the replay will differ.", dist2, fs);
        else            ReplayMsg("Walking to the start (%.0f u)", dist2);
    }
    return true;
}

// подмена usercmd: сначала подход к старту, потом кадры записи
static void ApplyReplay(UserCmd* cmd)
{
    const PlayerMove* pm = (const PlayerMove*)g_pm;
    if (!pm) return;
    if (pm->dead || pm->deadflag) { StopReplay("player died"); return; }
    const Frame& f0 = g_replayRun.frames[0];

    // ---- фаза 1: идём к стартовой точке
    if (g_replayPhase == 1) {
        unsigned long long now = GetTickCount64();
        float dx = f0.pos.x - pm->origin.x, dy = f0.pos.y - pm->origin.y;
        float dist = sqrtf(dx * dx + dy * dy);
        float vx = pm->velocity.x, vy = pm->velocity.y;
        float speed = sqrtf(vx * vx + vy * vy);
        g_walkDist = dist;
        if (now - g_walkStartTick > 25000ULL) { StopReplay("could not reach the start in time"); return; }
        if (dist < g_walkBest - 2.0f) { g_walkBest = dist; g_walkBestTick = now; }
        else if (now - g_walkBestTick > 2500ULL && dist > 3.0f) { StopReplay("something blocks the way to the start"); return; }

        const float kPiL = 3.14159265f;
        float yaw = atan2f(dy, dx) * (180.0f / kPiL);
        cmd->viewangles.x = 0.0f; cmd->viewangles.y = yaw; cmd->viewangles.z = 0.0f;
        cmd->upmove = 0.0f; cmd->impulse = 0;
        cmd->buttons = (f0.flags & FR_DUCK) ? IN_DUCK : 0;

        if (dist < 0.6f && speed < 10.0f) {       // пришли: один холостой кадр и начинаем
            cmd->forwardmove = 0.0f; cmd->sidemove = 0.0f;
            if (g_viewAnglesOk) { float a[3] = { f0.pitch, f0.yaw, 0.0f }; g_setViewAngles(a); }
            BeginPlayback();
            return;
        }
        float inv = dist > 1e-3f ? 1.0f / dist : 0.0f;
        float vAlong = vx * dx * inv + vy * dy * inv;
        float vdes = fminf(300.0f, dist * 10.0f);
        float fwd = vdes + 3.0f * (vdes - vAlong);          // разгон и торможение: подбирал по модели трения/ускорения HL
        if (fwd > 400.0f) fwd = 400.0f;
        if (fwd < -400.0f) fwd = -400.0f;
        cmd->forwardmove = fwd;
        cmd->sidemove = 0.0f;
        if (g_viewAnglesOk) { float a[3] = { 0.0f, yaw, 0.0f }; g_setViewAngles(a); }
        return;
    }

    // ---- фаза 3: ждём, пока движок примет новый лимит FPS (стоим на месте)
    if (g_replayPhase == 3) {
        ++g_waitFrames;
        cmd->viewangles.x = f0.pitch; cmd->viewangles.y = f0.yaw; cmd->viewangles.z = 0.0f;
        cmd->forwardmove = 0.0f; cmd->sidemove = 0.0f; cmd->upmove = 0.0f; cmd->impulse = 0;
        cmd->buttons = (f0.flags & FR_DUCK) ? IN_DUCK : 0;
        if (g_viewAnglesOk) { float a[3] = { f0.pitch, f0.yaw, 0.0f }; g_setViewAngles(a); }
        bool ready = (g_waitFrames > 3 && FpsMaxApplied()) || g_waitFrames > 60;
        if (ready) {
            if (g_waitFrames > 60) Log("replay: fps_max did not reach 1000 in time, continuing anyway");
            g_replayPhase = 2;
            g_replayIdx = 0;
            LsBegin(f0.msec);
            ReplayMsg("Replay started");
        }
        return;
    }

    // ---- фаза 2: повторяем записанный ввод
    if (g_paused) return;                                    // на паузе кадры записи не тратим
    if (g_replayIdx >= (int)g_replayRun.frames.size()) { StopReplay("finished"); return; }
    const Frame& f = g_replayRun.frames[g_replayIdx];

    // расхождение: где игрок сейчас и где он был в записи в этот же момент
    {
        float ex = pm->origin.x - f.pos.x, ey = pm->origin.y - f.pos.y, ez = pm->origin.z - f.pos.z;
        float drift = sqrtf(ex * ex + ey * ey + ez * ez);
        g_driftNow = drift;
        if (drift > g_driftMax) { g_driftMax = drift; g_driftMaxIdx = g_replayIdx; }
        if (g_driftFirstIdx < 0 && drift > 1.0f) {
            g_driftFirstIdx = g_replayIdx;
            Log("replay: first drift > 1 u at frame %d: now pos(%.2f %.2f %.2f) vel(%.1f %.1f %.1f) / rec pos(%.2f %.2f %.2f) vel(%.1f %.1f %.1f) msec=%d buttons=%d",
                g_replayIdx, pm->origin.x, pm->origin.y, pm->origin.z, pm->velocity.x, pm->velocity.y, pm->velocity.z,
                f.pos.x, f.pos.y, f.pos.z, f.vel.x, f.vel.y, f.vel.z, (int)f.msec, (int)f.buttons);
        }
        if (g_stopOnDrift && g_replayIdx > 0 && drift > g_driftLimit) { StopReplay("lost the recording (drift)"); return; }
    }

    {
        unsigned long long nowMs = GetTickCount64();
        if (g_spdStartReal == 0) g_spdStartReal = nowMs;
        if (g_spdRealT0 == 0) g_spdRealT0 = nowMs;
        g_spdGameMs += f.msec;
        g_spdTotalGameMs += f.msec;
        if (nowMs - g_spdRealT0 >= 500ULL) {
            g_replaySpeed = (float)g_spdGameMs / (float)(nowMs - g_spdRealT0);
            g_spdGameMs = 0;
            g_spdRealT0 = nowMs;
        }
    }
    ++g_replayIdx;
    cmd->viewangles.x = f.pitch;
    cmd->viewangles.y = f.yaw;
    cmd->viewangles.z = 0.0f;
    cmd->forwardmove = f.fwd;
    cmd->sidemove = f.side;
    cmd->upmove = 0.0f;
    cmd->buttons = f.buttons;
    cmd->msec = f.msec;
    cmd->impulse = 0;
    if (g_viewAnglesOk) {
        float a[3] = { f.pitch, f.yaw, 0.0f };
        g_setViewAngles(a);
    }
    if (g_lsOn && g_lsWatchdog > 3) {
        Log("replay: lock-step watchdog fired %d times, switching lock-step off", g_lsWatchdog);
        LsEnd();
        ReplayMsg("Lock-step time switched off: the engine merges short frames");
    }
    if (g_lsOn && g_replayIdx < (int)g_replayRun.frames.size()) LsNextFrame(g_replayRun.frames[g_replayIdx].msec);
}

// ---------------------------------------------------------------- время игры (на стороне движка)
// Движок берёт время у Windows: QueryPerformanceCounter / timeGetTime / GetTickCount.
// Мы подменяем эти вызовы, но ТОЛЬКО когда их вызывает код из hw.dll — игра сама
// «живёт» медленнее, без консольных команд. Физические кадры остаются теми же
// (обычно по 10 мс), просто идут реже, как в настоящих TAS-инструментах.
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
typedef DWORD(WINAPI* U32Time_t)(void);
static QPC_t     oQPC = nullptr;
static U32Time_t oTimeGetTime = nullptr;
static U32Time_t oGetTickCount = nullptr;

struct Clock { double baseReal = 0, baseVirt = 0; };
static Clock g_clkQpc, g_clkMm, g_clkTick;
static bool g_clocksOk = false;
static uintptr_t g_engLo = 0, g_engHi = 0;

static inline bool FromEngine(void* ret)
{
    uintptr_t a = (uintptr_t)ret;
    return a >= g_engLo && a < g_engHi;
}
static inline double Virt(const Clock& c, double real)
{
    return c.baseVirt + (real - c.baseReal) * g_scale;
}

// «Пошаговое» время для воспроизведения: движок получает время, которое растёт ровно
// на длину очередного кадра записи (cmd.msec), независимо от реального FPS и замедления.
static double g_lsT = 0.0;            // игровое время воспроизведения, сек
static int    g_lsPendingMs = 10;     // длина следующего кадра, мс
static bool   g_lsAdvanced = false;   // время на этот кадр уже выдано движку
static double g_lsLastReal = 0.0;     // реальный момент последней выдачи (счётчики QPC)
static double g_lsBaseQpc = 0, g_lsBaseMm = 0, g_lsBaseTick = 0, g_qpcFreq = 1.0;

static void LsTick()
{
    LARGE_INTEGER q;
    oQPC(&q);
    double now = (double)q.QuadPart;
    if (g_paused) {                     // на паузе время идёт как обычно, кадры записи не тратятся
        g_lsT += (now - g_lsLastReal) / g_qpcFreq;
        g_lsLastReal = now;
        return;
    }
    if (g_lsAdvanced && now - g_lsLastReal > 0.25 * g_qpcFreq) {
        // кадр так и не начался (движок склеивает короткие кадры): страховка от зависания
        g_lsT += 0.010;
        g_lsLastReal = now;
        ++g_lsWatchdog;
        return;
    }
    if (!g_lsAdvanced) {
        if (g_replayPace == 1) {                               // как можно быстрее: без ожидания
            g_lsT += g_lsPendingMs * 0.001;
            g_lsAdvanced = true;
            g_lsLastReal = now;
        }
        else {
            double sc = g_scale > 0.001 ? g_scale : 0.001;
            double need = (g_lsPendingMs * 0.001) / sc * g_qpcFreq;
            if (now - g_lsLastReal >= need) {
                g_lsT += g_lsPendingMs * 0.001;
                g_lsAdvanced = true;
                g_lsLastReal += need;                          // держим расписание, а не «сейчас»: опоздания не копятся
                if (now - g_lsLastReal > 0.1 * g_qpcFreq) g_lsLastReal = now;
            }
        }
    }
}

static NOINLINE BOOL WINAPI hkQPC(LARGE_INTEGER* out)
{
    void* ret = CALLER();
    BOOL r = oQPC(out);
    if (r && out && g_clocksOk && FromEngine(ret)) {
        if (g_lsOn) { LsTick(); out->QuadPart = (LONGLONG)(g_lsBaseQpc + g_lsT * g_qpcFreq); }
        else out->QuadPart = (LONGLONG)Virt(g_clkQpc, (double)out->QuadPart);
    }
    return r;
}
static NOINLINE DWORD WINAPI hkTimeGetTime(void)
{
    void* ret = CALLER();
    DWORD r = oTimeGetTime();
    if (g_clocksOk && FromEngine(ret)) {
        if (g_lsOn) { LsTick(); return (DWORD)(ULONGLONG)(g_lsBaseMm + g_lsT * 1000.0); }
        return (DWORD)(ULONGLONG)Virt(g_clkMm, (double)r);
    }
    return r;
}
static NOINLINE DWORD WINAPI hkGetTickCount(void)
{
    void* ret = CALLER();
    DWORD r = oGetTickCount();
    if (g_clocksOk && FromEngine(ret)) {
        if (g_lsOn) { LsTick(); return (DWORD)(ULONGLONG)(g_lsBaseTick + g_lsT * 1000.0); }
        return (DWORD)(ULONGLONG)Virt(g_clkTick, (double)r);
    }
    return r;
}

static void EngineRange()
{
    HMODULE hw = GetModuleHandleA("hw.dll");
    if (!hw) hw = GetModuleHandleA("sw.dll");
    if (!hw) return;
    auto nt = (PIMAGE_NT_HEADERS)((BYTE*)hw + ((PIMAGE_DOS_HEADER)hw)->e_lfanew);
    g_engLo = (uintptr_t)hw;
    g_engHi = g_engLo + nt->OptionalHeader.SizeOfImage;
}

static void InitClocks()
{
    LARGE_INTEGER q;
    if (oQPC) { oQPC(&q); g_clkQpc.baseReal = g_clkQpc.baseVirt = (double)q.QuadPart; }
    if (oTimeGetTime) { g_clkMm.baseReal = g_clkMm.baseVirt = (double)oTimeGetTime(); }
    if (oGetTickCount) { g_clkTick.baseReal = g_clkTick.baseVirt = (double)oGetTickCount(); }
    g_clocksOk = g_engHi != 0 && (oQPC || oTimeGetTime || oGetTickCount);
}

// сменить скорость времени: фиксируем текущее «игровое» время и дальше идём с новым множителем
static void SetScale(double ns)
{
    if (!g_clocksOk) return;
    LARGE_INTEGER q;
    if (oQPC) {
        oQPC(&q);
        double r = (double)q.QuadPart;
        g_clkQpc.baseVirt = Virt(g_clkQpc, r); g_clkQpc.baseReal = r;
    }
    if (oTimeGetTime) {
        double r = (double)oTimeGetTime();
        g_clkMm.baseVirt = Virt(g_clkMm, r); g_clkMm.baseReal = r;
    }
    if (oGetTickCount) {
        double r = (double)oGetTickCount();
        g_clkTick.baseVirt = Virt(g_clkTick, r); g_clkTick.baseReal = r;
    }
    g_scale = ns;
}

static void LsBegin(int firstMs)
{
    if (!g_clocksOk || !oQPC || !oTimeGetTime || !oGetTickCount) { g_lsOn = false; return; }
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;
    oQPC(&q);
    double rq = (double)q.QuadPart;
    g_lsBaseQpc = Virt(g_clkQpc, rq);
    g_lsBaseMm = Virt(g_clkMm, (double)oTimeGetTime());
    g_lsBaseTick = Virt(g_clkTick, (double)oGetTickCount());
    g_lsT = 0.0;
    g_lsLastReal = rq;
    g_lsPendingMs = firstMs > 0 ? firstMs : 1;
    g_lsAdvanced = false;
    g_lsOn = true;
}

static void LsNextFrame(int ms)
{
    g_lsPendingMs = ms > 0 ? ms : 1;
    g_lsAdvanced = false;
}

// вернуться к обычному времени без скачка
static void LsEnd()
{
    if (!g_lsOn) return;
    LARGE_INTEGER q;
    oQPC(&q);
    g_clkQpc.baseReal = (double)q.QuadPart;      g_clkQpc.baseVirt = g_lsBaseQpc + g_lsT * g_qpcFreq;
    g_clkMm.baseReal = (double)oTimeGetTime();   g_clkMm.baseVirt = g_lsBaseMm + g_lsT * 1000.0;
    g_clkTick.baseReal = (double)oGetTickCount();  g_clkTick.baseVirt = g_lsBaseTick + g_lsT * 1000.0;
    g_lsOn = false;
}

// ---------------------------------------------------------------- модели: цвет и яркость (chams) через перехват OpenGL
// Модели рисуются полосами/веерами треугольников. Подменяем цвет только когда эти вызовы идут из движка (hw.dll).
// Модель оружия движок рисует в суженном диапазоне глубины (0 .. 0.3), по этому признаку её и отличаем.
typedef void (WINAPI* glBegin_t)(GLenum);
typedef void (WINAPI* glEnd_t)(void);
typedef void (WINAPI* glDepthRange_t)(GLclampd, GLclampd);
typedef void (WINAPI* glColor3f_t)(GLfloat, GLfloat, GLfloat);
typedef void (WINAPI* glColor3fv_t)(const GLfloat*);
typedef void (WINAPI* glColor4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (WINAPI* glColor4fv_t)(const GLfloat*);
typedef void (WINAPI* glColor3ub_t)(GLubyte, GLubyte, GLubyte);
typedef void (WINAPI* glColor4ub_t)(GLubyte, GLubyte, GLubyte, GLubyte);
static glBegin_t      oGlBegin = nullptr;
static glEnd_t        oGlEnd = nullptr;
static glDepthRange_t oGlDepthRange = nullptr;
static glColor3f_t    oGlColor3f = nullptr;
static glColor3fv_t   oGlColor3fv = nullptr;
static glColor4f_t    oGlColor4f = nullptr;
static glColor4fv_t   oGlColor4fv = nullptr;
static glColor3ub_t   oGlColor3ub = nullptr;
static glColor4ub_t   oGlColor4ub = nullptr;

static bool g_inView = false;               // сейчас рисуется модель оружия
static bool g_visPushed = false;            // состояние OpenGL сохранено, идёт подменённая модель
static const VisCfg* g_visCfg = nullptr;

static NOINLINE void WINAPI hkGlBegin(GLenum mode)
{
    void* ret = CALLER();
    if ((mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) && !g_visPushed && FromEngine(ret)) {
        const VisCfg* c = g_inView ? &g_weap : &g_chams;
        if (c->on) {
            glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_TEXTURE_BIT);
            g_visPushed = true;
            g_visCfg = c;
            if (c->style != 0) glDisable(GL_TEXTURE_2D);
            if (c->style == 2) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); glDepthMask(GL_FALSE); }
            if (c->through && !g_inView) glDepthRange(0.0, 0.02);        // «поверх стен»: модель ближе всего остального
        }
    }
    oGlBegin(mode);
}

static NOINLINE void WINAPI hkGlEnd(void)
{
    oGlEnd();
    if (g_visPushed) { glPopAttrib(); g_visPushed = false; g_visCfg = nullptr; }
}

static NOINLINE void WINAPI hkGlDepthRange(GLclampd n, GLclampd f)
{
    if (FromEngine(CALLER())) g_inView = (f < 0.5 && n < 0.001);
    oGlDepthRange(n, f);
}

// подмена цвета вершины; возвращает true, если цвет заменён
static inline bool VisColor(float& r, float& g, float& b, float& a)
{
    if (!g_visPushed || !g_visCfg) return false;
    const VisCfg& c = *g_visCfg;
    if (c.style == 0) { r *= c.col[0] * c.bright; g *= c.col[1] * c.bright; b *= c.col[2] * c.bright; }
    else {
        r = c.col[0] * c.bright; g = c.col[1] * c.bright; b = c.col[2] * c.bright;
        if (c.style == 2) a *= 0.55f;
    }
    ++g_visHits[g_inView ? 1 : 0];
    return true;
}

static NOINLINE void WINAPI hkGlColor3f(GLfloat r, GLfloat g, GLfloat b)
{
    float a = 1.0f;
    if (g_visPushed && oGlColor4f && FromEngine(CALLER()) && VisColor(r, g, b, a)) { oGlColor4f(r, g, b, a); return; }
    oGlColor3f(r, g, b);
}
static NOINLINE void WINAPI hkGlColor3fv(const GLfloat* v)
{
    float r = v[0], g = v[1], b = v[2], a = 1.0f;
    if (g_visPushed && oGlColor4f && FromEngine(CALLER()) && VisColor(r, g, b, a)) { oGlColor4f(r, g, b, a); return; }
    oGlColor3fv(v);
}
static NOINLINE void WINAPI hkGlColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    if (g_visPushed && FromEngine(CALLER()) && VisColor(r, g, b, a)) { oGlColor4f(r, g, b, a); return; }
    oGlColor4f(r, g, b, a);
}
static NOINLINE void WINAPI hkGlColor4fv(const GLfloat* v)
{
    float r = v[0], g = v[1], b = v[2], a = v[3];
    if (g_visPushed && oGlColor4f && FromEngine(CALLER()) && VisColor(r, g, b, a)) { oGlColor4f(r, g, b, a); return; }
    oGlColor4fv(v);
}
static NOINLINE void WINAPI hkGlColor3ub(GLubyte r8, GLubyte g8, GLubyte b8)
{
    float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f, a = 1.0f;
    if (g_visPushed && oGlColor4f && FromEngine(CALLER()) && VisColor(r, g, b, a)) { oGlColor4f(r, g, b, a); return; }
    oGlColor3ub(r8, g8, b8);
}
static NOINLINE void WINAPI hkGlColor4ub(GLubyte r8, GLubyte g8, GLubyte b8, GLubyte a8)
{
    float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f, a = a8 / 255.0f;
    if (g_visPushed && oGlColor4f && FromEngine(CALLER()) && VisColor(r, g, b, a)) { oGlColor4f(r, g, b, a); return; }
    oGlColor4ub(r8, g8, b8, a8);
}

// ---------------------------------------------------------------- пауза / шаг / замедление
static bool  g_slowOn = false;
static float g_slowScale = 0.20f;
static bool  g_stepActive = false;
static float g_stepTime = 0.0f;
static int   g_stepSwaps = 0;

// «Плавный» режим: при замедлении поднимаем лимит FPS игры, чтобы кадров рисовалось больше.
// Минус: игровые кадры становятся короче 10 мс, а значит физика уже не такая, как на 100 FPS.
static bool g_smooth = false;

static void SendFps(int fps)
{
    if (fps == g_lastFps) return;
    g_lastFps = fps;
    Cmd("fps_override 1");
    Cmd("fps_max %d", fps);
}

static void ApplySlow()
{
    SetScale(g_slowOn ? (double)g_slowScale : 1.0);
    if (!g_smooth && g_lastFps == 0) return;            // ничего не трогали, ничего не возвращаем
    if (g_smooth && g_slowOn) {
        int fps = (int)(g_normalFps / g_slowScale + 0.5f);
        if (fps < g_normalFps) fps = g_normalFps;
        if (fps > 1000) fps = 1000;
        SendFps(fps);
    }
    else {
        SendFps(g_normalFps);                           // вернуть обычный лимит
    }
}

static void TogglePause()
{
    Cmd("pause");
    g_paused = !g_paused;
    g_stepActive = false;
}

static void ToggleSlow()
{
    g_slowOn = !g_slowOn;
    ApplySlow();
}

// Шаг: снимаем паузу, дожидаемся одного игрового кадра и ставим паузу обратно.
// Если игра не на паузе, первое нажатие просто ставит паузу.
static void StepFrame()
{
    if (!g_paused) { TogglePause(); return; }
    if (g_stepActive) return;
    const PlayerMove* pm = (const PlayerMove*)g_pm;
    g_stepTime = pm ? pm->time : 0.0f;
    g_stepSwaps = 0;
    g_stepActive = true;
    Cmd("pause");
}

static void UpdateStep()
{
    if (!g_stepActive) return;
    ++g_stepSwaps;
    const PlayerMove* pm = (const PlayerMove*)g_pm;
    bool advanced = pm && pm->time != g_stepTime;
    if (advanced || g_stepSwaps >= 4 || (!pm && g_stepSwaps >= 1)) {
        Cmd("pause");
        g_stepActive = false;
    }
}

// ---------------------------------------------------------------- движение
static bool g_bhop = false;
static bool g_strafe = false;
static int  g_strafeMode = 0;           // 0 = авто (направление задаёт взгляд), 1 = помощник (A/D по мыши)
static int  g_strafeDirMode = 0;         // авто-режим: 0 = вперёд, 1 = влево, 2 = вправо, 3 = назад, 4 = по зажатым W/A/S/D
static bool g_strafeJumpOnly = true;    // страфить, только пока зажат прыжок
static bool g_gstrafe = false;          // граунд-страйф (работает, пока зажата клавиша из KEYS)
static bool g_hookCreateMove = false;
static int  g_theme = 0;                // 0 = Neon, 1 = Mono

typedef void (*CreateMove_t)(float, UserCmd*, int);
static CreateMove_t oCreateMove = nullptr;

static const float kPi = 3.14159265358979f;

static float NormAngle(float a)             // градусы -> [-180, 180]
{
    while (a > 180.0f)  a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
static float NormRad(float a)               // радианы -> [-pi, pi]
{
    while (a > kPi)  a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

static void SetMoveButtons(UserCmd* cmd, float fwd, float side)
{
    cmd->forwardmove = fwd;
    cmd->sidemove = side;
    cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
    if (fwd > 1.0f) cmd->buttons |= IN_FORWARD;
    if (fwd < -1.0f) cmd->buttons |= IN_BACK;
    if (side > 1.0f) cmd->buttons |= IN_MOVERIGHT;
    if (side < -1.0f) cmd->buttons |= IN_MOVELEFT;
}

static bool g_keys_gs_down();

static void DoMovement(UserCmd* cmd)
{
    // поворот мыши считаем каждый кадр, даже если дальше выйдем
    static float prevYaw = 0.0f;
    float yaw = cmd->viewangles.y;
    float dyaw = NormAngle(yaw - prevYaw);
    prevYaw = yaw;

    static bool gsDucked = false;

    const PlayerMove* pm = (const PlayerMove*)g_pm;
    if (!pm || !PmFresh()) return;
    if (pm->dead || pm->deadflag || pm->spectator) return;
    if (pm->movetype != 3 /*MOVETYPE_WALK*/ || pm->waterlevel > 1) return;

    const bool onGround = (pm->flags & FL_ONGROUND) != 0 || pm->onground != -1;
    const bool jumpHeld = (cmd->buttons & IN_JUMP) != 0;

    // ---- автострафе
    if (g_strafe && !onGround && (jumpHeld || !g_strafeJumpOnly)) {
        if (g_strafeMode == 0) {
            // Авто: ставим направление «хотения» перпендикулярно скорости, в ту сторону,
            // куда нужно довернуть скорость, чтобы она совпала с направлением взгляда.
            // Камеру не трогаем: поворачиваешь мышью ты, страфы подстраиваются под неё.
            float vx = pm->velocity.x, vy = pm->velocity.y;
            if (vx * vx + vy * vy > 1.0f) {
                float offset = 0.0f;                     // куда хотим лететь относительно взгляда
                switch (g_strafeDirMode) {
                case 1: offset = kPi * 0.5f; break;     // влево
                case 2: offset = -kPi * 0.5f; break;     // вправо
                case 3: offset = kPi;        break;     // назад
                case 4:                                  // «авто»: куда нажаты клавиши
                    if ((cmd->buttons & IN_MOVELEFT) && !(cmd->buttons & IN_MOVERIGHT)) offset = kPi * 0.5f;
                    else if ((cmd->buttons & IN_MOVERIGHT) && !(cmd->buttons & IN_MOVELEFT)) offset = -kPi * 0.5f;
                    else if (cmd->buttons & IN_BACK) offset = kPi;
                    break;
                default: break;
                }
                float velAng = atan2f(vy, vx);
                float viewAng = yaw * (kPi / 180.0f);
                float delta = NormRad(velAng - viewAng - offset);

                const float K = 400.0f;
                float sgn = delta > 0.0f ? 1.0f : -1.0f;
                float fwd = sgn * K * sinf(delta);
                float side = sgn * K * cosf(delta);
                SetMoveButtons(cmd, fwd, side);
            }
        }
        else {
            // Помощник: ты задаёшь поворот мышью, а A/D выбираются сами по направлению поворота
            if (fabsf(dyaw) < 0.0001f) SetMoveButtons(cmd, 0.0f, 0.0f);
            else if (dyaw > 0.0f)      SetMoveButtons(cmd, 0.0f, -250.0f);   // поворот влево -> A
            else                       SetMoveButtons(cmd, 0.0f, 250.0f);   // поворот вправо -> D
        }
    }

    // ---- автобанихоп: на земле держим прыжок, в воздухе отпускаем, чтобы на приземлении нажать заново
    if (g_bhop && jumpHeld && !onGround)
        cmd->buttons &= ~IN_JUMP;

    // ---- граунд-страйф: пока зажата клавиша и мы на земле, приседаем через кадр
    bool gsHeld = g_gstrafe && g_keys_gs_down();
    if (gsHeld && onGround) {
        if (!gsDucked) { cmd->buttons |= IN_DUCK; gsDucked = true; }
        else { cmd->buttons &= ~IN_DUCK; gsDucked = false; }
    }
    else {
        gsDucked = false;
    }
}

static void hkCreateMove(float frametime, UserCmd* cmd, int active)
{
    oCreateMove(frametime, cmd, active);
    if (cmd) {
        g_lastCmdYaw = cmd->viewangles.y;
        const PlayerMove* pm = (const PlayerMove*)g_pm;
        UpdateJumpStats(pm);
        if (g_replayActive) { ApplyReplay(cmd); return; }
        DoMovement(cmd);
        LeadInPush(pm, cmd);
        RecordFrame(pm, cmd);
    }
}

// ---------------------------------------------------------------- свои клавиши
enum Action { ACT_MENU, ACT_PAUSE, ACT_STEP, ACT_SLOW, ACT_BHOP, ACT_STRAFE, ACT_GSTRAFE, ACT_RECORD, ACT_REPLAY, ACT_COUNT };
static const char* kActionName[ACT_COUNT] = { "Open / close menu", "Pause / resume", "Step frame", "Slow motion on / off", "Auto bhop on / off", "Auto strafe on / off", "Ground strafe (hold)", "Record run on / off", "Play / stop replay" };
static const char* kActionKey[ACT_COUNT] = { "menu", "pause", "step", "slow", "bhop", "strafe", "gstrafe", "record", "replay" };
static int  g_keys[ACT_COUNT] = { VK_INSERT, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, 'G', VK_F11, VK_F12 };
static int  g_capture = -1;                 // какое действие сейчас «слушает» клавишу
static char g_iniPath[MAX_PATH] = "";

static bool g_keys_gs_down()
{
    int vk = g_keys[ACT_GSTRAFE];
    if (!vk || g_menu || g_capture >= 0) return false;
    if (GetForegroundWindow() != g_hwnd) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void InitIniPath()
{
    GetModuleFileNameA(nullptr, g_iniPath, MAX_PATH);
    char* slash = strrchr(g_iniPath, '\\');
    if (slash) slash[1] = 0;
    strncat(g_iniPath, "hltas.ini", MAX_PATH - strlen(g_iniPath) - 1);
}

static void SaveConfig()
{
    char v[32];
    for (int i = 0; i < ACT_COUNT; ++i) {
        snprintf(v, sizeof(v), "%d", g_keys[i]);
        WritePrivateProfileStringA("binds", kActionKey[i], v, g_iniPath);
    }
    snprintf(v, sizeof(v), "%.3f", g_slowScale);
    WritePrivateProfileStringA("time", "slow_factor", v, g_iniPath);
    WritePrivateProfileStringA("time", "smooth", g_smooth ? "1" : "0", g_iniPath);
    snprintf(v, sizeof(v), "%d", g_theme);
    WritePrivateProfileStringA("ui", "theme", v, g_iniPath);
    WritePrivateProfileStringA("rec", "trail", g_trailShow ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("rec", "trail_through", g_trailThrough ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("rec", "jump_stats", g_jumpStatsOn ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("misc", "hook_gdi_swap", g_hookGdiSwap ? "1" : "0", g_iniPath);
    {
        const char* sec[2] = { "vis_models", "vis_weapon" };
        const VisCfg* vc[2] = { &g_chams, &g_weap };
        for (int k = 0; k < 2; ++k) {
            WritePrivateProfileStringA(sec[k], "on", vc[k]->on ? "1" : "0", g_iniPath);
            snprintf(v, sizeof(v), "%d", vc[k]->style);  WritePrivateProfileStringA(sec[k], "style", v, g_iniPath);
            snprintf(v, sizeof(v), "%.3f", vc[k]->col[0]); WritePrivateProfileStringA(sec[k], "r", v, g_iniPath);
            snprintf(v, sizeof(v), "%.3f", vc[k]->col[1]); WritePrivateProfileStringA(sec[k], "g", v, g_iniPath);
            snprintf(v, sizeof(v), "%.3f", vc[k]->col[2]); WritePrivateProfileStringA(sec[k], "b", v, g_iniPath);
            snprintf(v, sizeof(v), "%.2f", vc[k]->bright); WritePrivateProfileStringA(sec[k], "bright", v, g_iniPath);
            WritePrivateProfileStringA(sec[k], "through", vc[k]->through ? "1" : "0", g_iniPath);
        }
    }
    WritePrivateProfileStringA("rec", "map_show", g_mapShow ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("rec", "map_cut", g_mapCut ? "1" : "0", g_iniPath);
    snprintf(v, sizeof(v), "%.0f", g_mapCutOff);
    WritePrivateProfileStringA("rec", "map_cut_off", v, g_iniPath);
    snprintf(v, sizeof(v), "%d", g_replayStartMode);
    WritePrivateProfileStringA("rec", "replay_start_mode", v, g_iniPath);
    WritePrivateProfileStringA("rec", "replay_lockstep", g_replayLockstep ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("rec", "lead_in", g_leadIn ? "1" : "0", g_iniPath);
    snprintf(v, sizeof(v), "%d", g_replayPace);
    WritePrivateProfileStringA("rec", "replay_pace", v, g_iniPath);
    WritePrivateProfileStringA("rec", "stop_on_drift", g_stopOnDrift ? "1" : "0", g_iniPath);
    snprintf(v, sizeof(v), "%.1f", g_driftLimit);
    WritePrivateProfileStringA("rec", "drift_limit", v, g_iniPath);
    snprintf(v, sizeof(v), "%.0f", g_replayMaxDist);
    WritePrivateProfileStringA("rec", "replay_max_dist", v, g_iniPath);
    WritePrivateProfileStringA("move", "bhop", g_bhop ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("move", "strafe", g_strafe ? "1" : "0", g_iniPath);
    snprintf(v, sizeof(v), "%d", g_strafeMode);
    WritePrivateProfileStringA("move", "strafe_mode", v, g_iniPath);
    snprintf(v, sizeof(v), "%d", g_strafeDirMode);
    WritePrivateProfileStringA("move", "strafe_dir_mode", v, g_iniPath);
    WritePrivateProfileStringA("move", "strafe_jump_only", g_strafeJumpOnly ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("move", "gstrafe", g_gstrafe ? "1" : "0", g_iniPath);
    snprintf(v, sizeof(v), "%d", g_normalFps);
    WritePrivateProfileStringA("time", "normal_fps", v, g_iniPath);
}

static void LoadConfig()
{
    InitIniPath();
    for (int i = 0; i < ACT_COUNT; ++i)
        g_keys[i] = (int)GetPrivateProfileIntA("binds", kActionKey[i], g_keys[i], g_iniPath);
    char v[32];
    GetPrivateProfileStringA("time", "slow_factor", "0.200", v, sizeof(v), g_iniPath);
    float f = (float)atof(v);
    if (f >= 0.01f && f <= 2.0f) g_slowScale = f;
    g_smooth = GetPrivateProfileIntA("time", "smooth", 0, g_iniPath) != 0;
    g_theme = (int)GetPrivateProfileIntA("ui", "theme", 0, g_iniPath);
    if (g_theme < 0 || g_theme > 1) g_theme = 0;
    g_trailShow = GetPrivateProfileIntA("rec", "trail", 1, g_iniPath) != 0;
    g_trailThrough = GetPrivateProfileIntA("rec", "trail_through", 0, g_iniPath) != 0;
    g_jumpStatsOn = GetPrivateProfileIntA("rec", "jump_stats", 0, g_iniPath) != 0;
    g_hookGdiSwap = GetPrivateProfileIntA("misc", "hook_gdi_swap", 1, g_iniPath) != 0;
    {
        const char* sec[2] = { "vis_models", "vis_weapon" };
        VisCfg* vc[2] = { &g_chams, &g_weap };
        for (int k = 0; k < 2; ++k) {
            char b[32];
            vc[k]->on = GetPrivateProfileIntA(sec[k], "on", 0, g_iniPath) != 0;
            int st = (int)GetPrivateProfileIntA(sec[k], "style", 0, g_iniPath);
            vc[k]->style = (st >= 0 && st <= 2) ? st : 0;
            const char* keys[3] = { "r", "g", "b" };
            for (int c = 0; c < 3; ++c) {
                GetPrivateProfileStringA(sec[k], keys[c], "", b, sizeof(b), g_iniPath);
                if (b[0]) { float f = (float)atof(b); if (f >= 0.0f && f <= 1.0f) vc[k]->col[c] = f; }
            }
            GetPrivateProfileStringA(sec[k], "bright", "", b, sizeof(b), g_iniPath);
            if (b[0]) { float f = (float)atof(b); if (f >= 0.3f && f <= 6.0f) vc[k]->bright = f; }
            vc[k]->through = GetPrivateProfileIntA(sec[k], "through", 0, g_iniPath) != 0;
        }
    }
    g_mapShow = GetPrivateProfileIntA("rec", "map_show", 1, g_iniPath) != 0;
    g_mapCut = GetPrivateProfileIntA("rec", "map_cut", 1, g_iniPath) != 0;
    int mco = (int)GetPrivateProfileIntA("rec", "map_cut_off", 64, g_iniPath);
    if (mco >= 0 && mco <= 1000) g_mapCutOff = (float)mco;
    g_replayStartMode = (int)GetPrivateProfileIntA("rec", "replay_start_mode", 0, g_iniPath);
    if (g_replayStartMode < 0 || g_replayStartMode > 2) g_replayStartMode = 0;
    g_replayLockstep = GetPrivateProfileIntA("rec", "replay_lockstep", 0, g_iniPath) != 0;
    g_leadIn = GetPrivateProfileIntA("rec", "lead_in", 1, g_iniPath) != 0;
    g_replayPace = (int)GetPrivateProfileIntA("rec", "replay_pace", 0, g_iniPath);
    if (g_replayPace < 0 || g_replayPace > 1) g_replayPace = 0;
    g_stopOnDrift = GetPrivateProfileIntA("rec", "stop_on_drift", 0, g_iniPath) != 0;
    {
        char dv[32];
        GetPrivateProfileStringA("rec", "drift_limit", "3.0", dv, sizeof(dv), g_iniPath);
        float dl2 = (float)atof(dv);
        if (dl2 >= 0.5f && dl2 <= 50.0f) g_driftLimit = dl2;
    }
    int rmd = (int)GetPrivateProfileIntA("rec", "replay_max_dist", 300, g_iniPath);
    if (rmd >= 20 && rmd <= 2000) g_replayMaxDist = (float)rmd;
    g_bhop = GetPrivateProfileIntA("move", "bhop", 0, g_iniPath) != 0;
    g_strafe = GetPrivateProfileIntA("move", "strafe", 0, g_iniPath) != 0;
    g_strafeMode = (int)GetPrivateProfileIntA("move", "strafe_mode", 0, g_iniPath);
    if (g_strafeMode < 0 || g_strafeMode > 1) g_strafeMode = 0;
    g_strafeDirMode = (int)GetPrivateProfileIntA("move", "strafe_dir_mode", GetPrivateProfileIntA("move", "strafe_key_dir", 0, g_iniPath) ? 4 : 0, g_iniPath);
    if (g_strafeDirMode < 0 || g_strafeDirMode > 4) g_strafeDirMode = 0;
    g_strafeJumpOnly = GetPrivateProfileIntA("move", "strafe_jump_only", 1, g_iniPath) != 0;
    g_gstrafe = GetPrivateProfileIntA("move", "gstrafe", 0, g_iniPath) != 0;
    int nf = (int)GetPrivateProfileIntA("time", "normal_fps", 100, g_iniPath);
    if (nf >= 20 && nf <= 1000) g_normalFps = nf;
}

static void KeyName(int vk, char* out, int n)
{
    if (!vk) { snprintf(out, n, "-"); return; }
    LONG lp = (LONG)(MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16);
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE:
        lp |= (1 << 24);
    }
    if (!GetKeyNameTextA(lp, out, n)) snprintf(out, n, "VK %d", vk);
}

// ---------------------------------------------------------------- меню/курсор
static void SetMenu(bool open)
{
    if (open == g_menu) return;
    if (open) {
        // игра каждый кадр возвращает курсор в центр, так что сейчас он именно там
        if (oGetCursorPos) oGetCursorPos(&g_center);
    }
    else {
        // вернуть курсор в центр, чтобы камеру не дёрнуло
        if (oSetCursorPos) oSetCursorPos(g_center.x, g_center.y);
    }
    g_menu = open;
    if (!open) g_capture = -1;
    if (g_init) ImGui::GetIO().MouseDrawCursor = open;
}

static LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_init && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        bool repeat = (lp & (1 << 30)) != 0;

        if (g_capture >= 0) {                       // ждём клавишу для назначения
            if (!repeat && wp != VK_SHIFT && wp != VK_CONTROL && wp != VK_MENU) {
                if (wp == VK_BACK) {
                    if (g_capture != ACT_MENU) { g_keys[g_capture] = 0; SaveConfig(); }
                }
                else if (wp != VK_ESCAPE) {
                    g_keys[g_capture] = (int)wp;
                    SaveConfig();
                }
                g_capture = -1;
            }
            return 0;
        }

        for (int a = 0; a < ACT_COUNT; ++a) {
            if (g_keys[a] == 0 || g_keys[a] != (int)wp) continue;
            switch (a) {
            case ACT_MENU:  if (!repeat) SetMenu(!g_menu); break;
            case ACT_PAUSE: if (!repeat) TogglePause();    break;
            case ACT_STEP:  StepFrame();                   break;
            case ACT_SLOW:  if (!repeat) ToggleSlow();     break;
            case ACT_BHOP:  if (!repeat) { g_bhop = !g_bhop;     SaveConfig(); } break;
            case ACT_STRAFE:if (!repeat) { g_strafe = !g_strafe; SaveConfig(); } break;
            case ACT_GSTRAFE: break;               // удержание: обрабатывается в DoMovement
            case ACT_RECORD: if (!repeat) ToggleRecord(); break;
            case ACT_REPLAY: if (!repeat) { if (g_replayActive) StopReplay("stopped"); else StartReplay(); } break;
            }
            return 0;
        }
    }

    if (g_init && g_menu) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_INPUT:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
        case WM_CHAR:
            return 0; // игра этого не видит, пока открыто меню
        }
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

static BOOL WINAPI hkSetCursorPos(int x, int y)
{
    if (g_menu) return TRUE;           // пока меню открыто, игра не может утащить курсор в центр
    return oSetCursorPos(x, y);
}

static BOOL WINAPI hkGetCursorPos(LPPOINT p)
{
    BOOL r = oGetCursorPos(p);
    if (r && g_menu && !g_passthru) *p = g_center;   // игре говорим, что мышь стоит на месте
    return r;
}

// ---------------------------------------------------------------- данные игрока
static void hkPlayerMove(PlayerMove* pm, int server)
{
    oPlayerMove(pm, server);
    if (!server) {                       // нас интересует клиентская копия
        g_pm = pm;
        g_pmTick = GetTickCount64();
    }
}

struct Snapshot
{
    bool  valid = false;
    Vec3  pos{}, vel{};
    float speed2d = 0, speed3d = 0;
    bool  onGround = false, ducking = false;
    float frametime = 0;
    int   movetype = 0;
};

static Snapshot ReadPlayer()
{
    Snapshot s;
    const PlayerMove* pm = (const PlayerMove*)g_pm;
    if (!pm || !PmFresh()) return s;   // данных нет или они устарели
    s.valid = true;
    s.pos = pm->origin;
    s.vel = pm->velocity;
    s.speed2d = sqrtf(s.vel.x * s.vel.x + s.vel.y * s.vel.y);
    s.speed3d = sqrtf(s.vel.x * s.vel.x + s.vel.y * s.vel.y + s.vel.z * s.vel.z);
    s.onGround = (pm->flags & FL_ONGROUND) != 0 || pm->onground != -1;
    s.ducking = pm->bInDuck != 0 || (pm->flags & (1 << 14)) != 0; // FL_DUCKING
    s.frametime = pm->frametime;
    s.movetype = pm->movetype;
    return s;
}

// ---------------------------------------------------------------- рисование
static void DrawHud(const Snapshot& s)
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();

    const float cx = io.DisplaySize.x * 0.5f;
    const float cy = io.DisplaySize.y * 0.74f;

    char num[32];
    if (s.valid) snprintf(num, sizeof(num), "%.0f", s.speed2d);
    else         snprintf(num, sizeof(num), "--");

    const float big = 44.0f;
    ImVec2 sz = font->CalcTextSizeA(big, FLT_MAX, 0.0f, num);
    ImVec2 pos(cx - sz.x * 0.5f, cy);
    ImU32 col = ImGui::ColorConvertFloat4ToU32(s.valid ? Neon::SpeedColor(s.speed2d) : Neon::Palette::TextDim);

    // «свечение»: смещённая копия маджентой под основным текстом
    dl->AddText(font, big, ImVec2(pos.x + 2, pos.y + 2),
        ImGui::ColorConvertFloat4ToU32(Neon::WithAlpha(Neon::Palette::Magenta, 0.55f)), num);
    dl->AddText(font, big, pos, col, num);

    const char* label = "UPS";
    ImVec2 lsz = font->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, label);
    dl->AddText(font, 16.0f, ImVec2(cx - lsz.x * 0.5f, pos.y + sz.y + 2),
        ImGui::ColorConvertFloat4ToU32(Neon::Palette::CyanDim), label);

    if (g_paused || g_slowOn) {
        char st[48];
        if (g_paused) snprintf(st, sizeof(st), "PAUSED");
        else          snprintf(st, sizeof(st), "SLOW  x%.2f", g_slowScale);
        ImVec2 ssz = font->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, st);
        dl->AddText(font, 22.0f, ImVec2(cx - ssz.x * 0.5f, cy - 34.0f),
            ImGui::ColorConvertFloat4ToU32(g_paused ? Neon::Palette::Magenta : Neon::Palette::Cyan), st);
    }

    if (s.valid) {
        // индикатор земли: циан — на земле, маджента — в воздухе
        ImVec4 st = s.onGround ? Neon::Palette::Cyan : Neon::Palette::Magenta;
        const char* txt = s.onGround ? "GROUND" : "AIR";
        ImVec2 tsz = font->CalcTextSizeA(14.0f, FLT_MAX, 0.0f, txt);
        dl->AddText(font, 14.0f, ImVec2(cx - tsz.x * 0.5f, pos.y + sz.y + 22),
            ImGui::ColorConvertFloat4ToU32(st), txt);
    }

    if (g_replayActive) {
        char rp[96];
        if (g_replayPhase == 1) snprintf(rp, sizeof(rp), "WALKING TO START  %.0f u", g_walkDist);
        else if (g_replayPhase == 3) snprintf(rp, sizeof(rp), "PREPARING REPLAY...");
        else                    snprintf(rp, sizeof(rp), "REPLAY  %d / %d     drift %.1f u     speed x%.2f", g_replayIdx, (int)g_replayRun.frames.size(), g_driftNow, g_replaySpeed);
        ImVec2 psz = font->CalcTextSizeA(18.0f, FLT_MAX, 0.0f, rp);
        dl->AddText(font, 18.0f, ImVec2(cx - psz.x * 0.5f, 36.0f), ImGui::ColorConvertFloat4ToU32(Neon::Palette::Cyan), rp);
    }
    else if (g_replayMsgT > 0.0f) {
        g_replayMsgT -= io.DeltaTime;
        ImVec2 msz = font->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, g_replayMsg);
        dl->AddText(font, 16.0f, ImVec2(cx - msz.x * 0.5f, 36.0f), ImGui::ColorConvertFloat4ToU32(Neon::Palette::Magenta), g_replayMsg);
    }

    if (g_rec) {
        char rc[48];
        if (g_recLead > 0) snprintf(rc, sizeof(rc), "REC  %.1f s   (run-up %d fr.)", g_recClock, g_recLead);
        else               snprintf(rc, sizeof(rc), "REC  %.1f s", g_recClock);
        ImVec2 rsz = font->CalcTextSizeA(18.0f, FLT_MAX, 0.0f, rc);
        dl->AddCircleFilled(ImVec2(cx - rsz.x * 0.5f - 12.0f, 46.0f), 5.0f, ImGui::ColorConvertFloat4ToU32(Neon::Palette::Magenta));
        dl->AddText(font, 18.0f, ImVec2(cx - rsz.x * 0.5f, 36.0f), ImGui::ColorConvertFloat4ToU32(Neon::Palette::Magenta), rc);
    }

    if (g_jumpStatsOn && g_jump.valid) {
        float jx = 28.0f, jy = io.DisplaySize.y * 0.62f;
        ImU32 hd = ImGui::ColorConvertFloat4ToU32(Neon::Palette::Cyan);
        ImU32 tx = ImGui::ColorConvertFloat4ToU32(Neon::Palette::Text);
        char jb[64];
        dl->AddText(font, 14.0f, ImVec2(jx, jy), hd, "LAST JUMP");
        snprintf(jb, sizeof(jb), "distance  %.1f", g_jump.dist);  dl->AddText(font, 16.0f, ImVec2(jx, jy + 18.0f), tx, jb);
        snprintf(jb, sizeof(jb), "pre       %.0f", g_jump.pre);  dl->AddText(font, 16.0f, ImVec2(jx, jy + 38.0f), tx, jb);
        snprintf(jb, sizeof(jb), "max       %.0f", g_jump.top);  dl->AddText(font, 16.0f, ImVec2(jx, jy + 58.0f), tx, jb);
        snprintf(jb, sizeof(jb), "height    %+.1f", g_jump.height); dl->AddText(font, 16.0f, ImVec2(jx, jy + 78.0f), tx, jb);
    }

    if (g_bhop || g_strafe || g_gstrafe) {
        char mv[64];
        snprintf(mv, sizeof(mv), "%s%s%s%s%s", g_bhop ? "BHOP" : "", (g_bhop && (g_strafe || g_gstrafe)) ? "   " : "",
            g_strafe ? "STRAFE" : "", (g_strafe && g_gstrafe) ? "   " : "", g_gstrafe ? "GSTRAFE" : "");
        ImVec2 msz = font->CalcTextSizeA(14.0f, FLT_MAX, 0.0f, mv);
        dl->AddText(font, 14.0f, ImVec2(cx - msz.x * 0.5f, pos.y + sz.y + 42),
            ImGui::ColorConvertFloat4ToU32(Neon::Palette::CyanDim), mv);
    }
}

// ================================================================ интерфейс меню
static ImFont* g_fontMain = nullptr;
static ImFont* g_fontSmall = nullptr;
static ImFont* g_fontBig = nullptr;

static int   g_page = 0;
static char  g_search[64] = "";
static bool  g_dry = false;          // «сухой» прогон: только считаем, сколько строк подходит под поиск
static int   g_dryHits = 0;
static bool  g_uiDone = false;       // последний слайдер только что отпустили
static float g_toast = 0.0f;

static const float kRowH = 34.0f;

static ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

static bool UiMatch(const char* label)
{
    if (!g_search[0]) return true;
    char a[96], b[96];
    size_t i = 0;
    for (; label[i] && i < sizeof(a) - 1; ++i) a[i] = (char)tolower((unsigned char)label[i]);
    a[i] = 0;
    for (i = 0; g_search[i] && i < sizeof(b) - 1; ++i) b[i] = (char)tolower((unsigned char)g_search[i]);
    b[i] = 0;
    return strstr(a, b) != nullptr;
}

static bool UiRow(const char* label)
{
    if (!UiMatch(label)) return false;
    if (g_dry) { ++g_dryHits; return false; }
    return true;
}

struct RowCtx { ImVec2 p, after; float w; };

static RowCtx UiRowBegin(const char* label)
{
    RowCtx r;
    r.p = ImGui::GetCursorScreenPos();
    r.w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(r.w, kRowH));
    r.after = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImDrawList* rdl = ImGui::GetWindowDrawList();
    rdl->AddText(ImVec2(r.p.x, r.p.y + (kRowH - ts.y) * 0.5f), Col(Neon::Palette::Text), label);
    rdl->AddLine(ImVec2(r.p.x, r.p.y + kRowH + 1.0f), ImVec2(r.p.x + r.w, r.p.y + kRowH + 1.0f),
        Col(Neon::WithAlpha(Neon::Palette::Text, 0.06f)));
    return r;
}
static void UiPlaceRight(const RowCtx& r, float cw)
{
    ImGui::SetCursorScreenPos(ImVec2(r.p.x + r.w - cw, r.p.y + (kRowH - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::SetNextItemWidth(cw);
}
static void UiRowEnd(const RowCtx& r) { ImGui::SetCursorScreenPos(r.after); }

static bool UiToggle(const char* label, bool* v)
{
    if (!UiRow(label)) return false;
    ImGui::PushID(label);
    RowCtx r = UiRowBegin(label);
    ImGui::SetCursorScreenPos(r.p);
    bool clicked = ImGui::InvisibleButton("##t", ImVec2(r.w, kRowH));
    if (clicked) *v = !*v;

    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID id = ImGui::GetID("anim");
    float target = *v ? 1.0f : 0.0f;
    float t = st->GetFloat(id, target);
    t += (target - t) * fminf(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    st->SetFloat(id, t);

    const float pw = 44.0f, ph = 22.0f;
    float x0 = r.p.x + r.w - pw, y0 = r.p.y + (kRowH - ph) * 0.5f;
    ImVec4 off = ImVec4(0.20f, 0.20f, 0.24f, 1.0f);
    ImVec4 on = Neon::Palette::Cyan;
    ImVec4 bg = ImVec4(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), Col(bg), ph * 0.5f);
    float kx = x0 + ph * 0.5f + t * (pw - ph);
    dl->AddCircleFilled(ImVec2(kx, y0 + ph * 0.5f), ph * 0.5f - 3.5f,
        Col(t > 0.5f ? ImVec4(0.04f, 0.04f, 0.06f, 1.0f) : ImVec4(0.75f, 0.75f, 0.80f, 1.0f)));
    UiRowEnd(r);
    ImGui::PopID();
    return clicked;
}

static bool UiCombo(const char* label, int* v, const char* items)
{
    if (!UiRow(label)) return false;
    ImGui::PushID(label);
    RowCtx r = UiRowBegin(label);
    UiPlaceRight(r, 190.0f);
    bool ch = ImGui::Combo("##c", v, items);
    UiRowEnd(r);
    ImGui::PopID();
    return ch;
}

static bool UiSlider(const char* label, float* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags = 0)
{
    if (!UiRow(label)) return false;
    ImGui::PushID(label);
    RowCtx r = UiRowBegin(label);
    UiPlaceRight(r, 230.0f);
    bool ch = ImGui::SliderFloat("##s", v, lo, hi, fmt, flags);
    g_uiDone = ImGui::IsItemDeactivatedAfterEdit();
    UiRowEnd(r);
    ImGui::PopID();
    return ch;
}

static bool UiInputInt(const char* label, int* v)
{
    if (!UiRow(label)) return false;
    ImGui::PushID(label);
    RowCtx r = UiRowBegin(label);
    UiPlaceRight(r, 120.0f);
    bool ch = ImGui::InputInt("##i", v, 0, 0);
    g_uiDone = ImGui::IsItemDeactivatedAfterEdit();
    UiRowEnd(r);
    ImGui::PopID();
    return ch;
}

static void UiInfoCol(const char* label, const ImVec4& col, const char* text)
{
    if (!UiRow(label)) return;
    RowCtx r = UiRowBegin(label);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(r.p.x + r.w - ts.x, r.p.y + (kRowH - ts.y) * 0.5f), Col(col), text);
    UiRowEnd(r);
}
static void UiInfo(const char* label, const char* fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UiInfoCol(label, Neon::Palette::Cyan, buf);
}

static void UiText(const char* text, bool warn = false)
{
    if (g_search[0]) return;
    ImGui::PushStyleColor(ImGuiCol_Text, warn ? Neon::Palette::Magenta : Neon::Palette::TextDim);
    ImGui::PushFont(g_fontSmall);
    ImGui::TextWrapped("%s", text);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
}

static bool UiButton(const char* label)
{
    if (!UiRow(label)) return false;
    ImGui::PushID(label);
    RowCtx r = UiRowBegin("");
    ImGui::SetCursorScreenPos(ImVec2(r.p.x, r.p.y + (kRowH - ImGui::GetFrameHeight()) * 0.5f));
    bool c = ImGui::Button(label, ImVec2(r.w, 0));
    UiRowEnd(r);
    ImGui::PopID();
    return c;
}

// две кнопки в одной строке: возвращает 1 или 2
static int UiButtons(const char* a, const char* b)
{
    if (!UiRow(a)) return 0;
    ImGui::PushID(a);
    RowCtx r = UiRowBegin("");
    float bw = (r.w - 10.0f) * 0.5f;
    float y = r.p.y + (kRowH - ImGui::GetFrameHeight()) * 0.5f;
    int res = 0;
    ImGui::SetCursorScreenPos(ImVec2(r.p.x, y));
    if (ImGui::Button(a, ImVec2(bw, 0))) res = 1;
    ImGui::SetCursorScreenPos(ImVec2(r.p.x + bw + 10.0f, y));
    if (ImGui::Button(b, ImVec2(bw, 0))) res = 2;
    UiRowEnd(r);
    ImGui::PopID();
    return res;
}

static void UiKey(const char* label, int action)
{
    if (!UiRow(label)) return;
    ImGui::PushID(label);
    RowCtx r = UiRowBegin(label);
    char key[48], text[64];
    if (g_capture == action) snprintf(key, sizeof(key), "press a key...");
    else                     KeyName(g_keys[action], key, sizeof(key));
    snprintf(text, sizeof(text), "%s##k", key);
    UiPlaceRight(r, 150.0f);
    if (ImGui::Button(text, ImVec2(150.0f, 0))) g_capture = action;
    UiRowEnd(r);
    ImGui::PopID();
}

struct CardDef { const char* title; int col; std::function<void()> fn; };

static void UiCard(const char* title, const std::function<void()>& fn)
{
    if (g_search[0]) {                       // при поиске показываем только карточки с совпадениями
        g_dry = true; g_dryHits = 0;
        fn();
        g_dry = false;
        if (!g_dryHits) return;
    }
    ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Neon::Palette::Card);
    ImGui::BeginChild(title, ImVec2(0, 0),
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    fn();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0, 8));
}

// ===== GL-PREVIEW-BEGIN
// ---------------------------------------------------------------- 3D-превью настроек подсветки (OpenGL внутри draw-списка ImGui)
struct PreviewParams { ImVec2 p0, p1; int kind; VisCfg cfg; float angle; };
static PreviewParams g_prevParams[2];

static void PvQuad(const float v[4][3], const float n[3], const PreviewParams& P, float br, float bg, float bb)
{
    float ca = cosf(P.angle), sa = sinf(P.angle);
    auto rot = [&](const float* a, float* o) { o[0] = a[0] * ca + a[2] * sa; o[1] = a[1]; o[2] = -a[0] * sa + a[2] * ca; };
    float rn[3];
    rot(n, rn);
    const float L[3] = { 0.40f, 0.70f, 0.60f };
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    float d = (rn[0] * L[0] + rn[1] * L[1] + rn[2] * L[2]) / ll;
    float shade = 0.30f + 0.70f * (d > 0.0f ? d : 0.0f);
    const VisCfg& c = P.cfg;
    float r, g, b, a = 1.0f;
    if (!c.on) { r = br * shade; g = bg * shade; b = bb * shade; }
    else if (c.style == 0) { r = br * shade * c.col[0] * c.bright; g = bg * shade * c.col[1] * c.bright; b = bb * shade * c.col[2] * c.bright; }
    else if (c.style == 1) { float k = 0.75f + 0.25f * shade; r = c.col[0] * c.bright * k; g = c.col[1] * c.bright * k; b = c.col[2] * c.bright * k; }
    else { float k = 0.65f + 0.35f * shade; r = c.col[0] * c.bright * k; g = c.col[1] * c.bright * k; b = c.col[2] * c.bright * k; a = 0.55f; }
    float rv[4][3];
    for (int i = 0; i < 4; ++i) rot(v[i], rv[i]);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) glVertex3fv(rv[i]);
    glEnd();
    glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 4; ++i) glVertex3fv(rv[i]);
    glEnd();
}

static void PvBox(const PreviewParams& P, float cx, float cy, float cz, float hx, float hy, float hz)
{
    float c[8][3];
    for (int i = 0; i < 8; ++i) {
        c[i][0] = cx + ((i & 1) ? hx : -hx);
        c[i][1] = cy + ((i & 2) ? hy : -hy);
        c[i][2] = cz + ((i & 4) ? hz : -hz);
    }
    static const int F[6][4] = { { 1, 3, 7, 5 },{ 0, 4, 6, 2 },{ 2, 6, 7, 3 },{ 0, 1, 5, 4 },{ 4, 5, 7, 6 },{ 0, 2, 3, 1 } };
    static const float N[6][3] = { { 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 } };
    for (int f = 0; f < 6; ++f) {
        float v[4][3];
        for (int k = 0; k < 4; ++k) { v[k][0] = c[F[f][k]][0]; v[k][1] = c[F[f][k]][1]; v[k][2] = c[F[f][k]][2]; }
        PvQuad(v, N[f], P, 0.58f, 0.62f, 0.70f);
    }
}

static void PreviewCallback(const ImDrawList*, const ImDrawCmd* cmd)
{
    const PreviewParams& P = *(const PreviewParams*)cmd->UserCallbackData;
    ImGuiIO& io = ImGui::GetIO();
    // видимая часть = пересечение окна превью и области отсечения ImGui (чтобы не вылезать при прокрутке)
    float x0 = fmaxf(P.p0.x, cmd->ClipRect.x), y0 = fmaxf(P.p0.y, cmd->ClipRect.y);
    float x1 = fminf(P.p1.x, cmd->ClipRect.z), y1 = fminf(P.p1.y, cmd->ClipRect.w);
    if (x1 - x0 < 4.0f || y1 - y0 < 4.0f) return;
    int W = (int)(P.p1.x - P.p0.x), H = (int)(P.p1.y - P.p0.y);
    int X = (int)P.p0.x, Y = (int)io.DisplaySize.y - (int)P.p1.y;
    int sx = (int)x0, sy = (int)io.DisplaySize.y - (int)y1, sw = (int)(x1 - x0), sh = (int)(y1 - y0);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glViewport(X, Y, W, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, sw, sh);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.027f, 0.027f, 0.038f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    double n = 0.1, t = n * tan(20.0 * 3.14159265 / 180.0), r = t * (double)W / (double)H;
    glFrustum(-r, r, -t, t, n, 50.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, P.cfg.on && P.cfg.style == 2 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    if (P.cfg.on && P.cfg.style == 2) glDepthMask(GL_FALSE);
    glLineWidth(1.0f);

    if (P.kind == 0) {                                       // человечек
        glTranslatef(0.0f, -0.95f, -4.6f);
        PvBox(P, 0.00f, 1.60f, 0.0f, 0.20f, 0.20f, 0.20f);    // голова
        PvBox(P, 0.00f, 1.00f, 0.0f, 0.34f, 0.42f, 0.18f);    // торс
        PvBox(P, -0.48f, 1.00f, 0.0f, 0.10f, 0.40f, 0.10f);   // руки
        PvBox(P, 0.48f, 1.00f, 0.0f, 0.10f, 0.40f, 0.10f);
        PvBox(P, -0.17f, 0.32f, 0.0f, 0.13f, 0.40f, 0.13f);   // ноги
        PvBox(P, 0.17f, 0.32f, 0.0f, 0.13f, 0.40f, 0.13f);
    }
    else {                                                 // оружие
        glTranslatef(0.0f, 0.0f, -3.4f);
        PvBox(P, 0.55f, 0.12f, 0.0f, 0.70f, 0.07f, 0.07f);    // ствол
        PvBox(P, -0.10f, 0.00f, 0.0f, 0.42f, 0.14f, 0.09f);   // корпус
        PvBox(P, -0.60f, -0.02f, 0.0f, 0.24f, 0.11f, 0.08f);  // приклад
        PvBox(P, -0.22f, -0.38f, 0.0f, 0.07f, 0.26f, 0.07f);  // рукоять
        PvBox(P, 0.10f, -0.30f, 0.0f, 0.06f, 0.22f, 0.06f);   // магазин
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
}
// ===== GL-PREVIEW-END

static void UiPreview(int kind, const VisCfg& cfg, float h)
{
    if (g_dry || g_search[0]) return;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, h));
    PreviewParams& P = g_prevParams[kind];
    P.p0 = p0; P.p1 = ImVec2(p0.x + w, p0.y + h);
    P.kind = kind; P.cfg = cfg;
    P.angle = (float)ImGui::GetTime() * 0.9f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCallback(PreviewCallback, &P);
    dl->AddRect(P.p0, P.p1, Col(Neon::Palette::Border), 8.0f);
    ImGui::Dummy(ImVec2(0, 4));
}

static bool UiColor(const char* label, float col[3])
{
    if (!UiRow(label)) return false;
    ImGui::PushID(label);
    RowCtx r = UiRowBegin(label);
    ImGui::SetCursorScreenPos(ImVec2(r.p.x + r.w - ImGui::GetFrameHeight() - 2.0f, r.p.y + (kRowH - ImGui::GetFrameHeight()) * 0.5f));
    bool ch = ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    UiRowEnd(r);
    ImGui::PopID();
    return ch;
}

struct NavItem { const char* group; const char* name; int parent; };
static const NavItem kNav[] = {
    { "OVERVIEW", "Info",     -1 },
    { "CONTROL",  "Time",     -1 },
    { "CONTROL",  "Movement", -1 },
    { "CONTROL",  "Recorder",  2 },     // вложен в Movement
    { "VISUALS",  "Models",   -1 },
    { "MISC",     "Keys",     -1 },
    { "MISC",     "Settings", -1 },
};
static const int PAGE_RECORDER = 3;
static const int kNavCount = (int)(sizeof(kNav) / sizeof(kNav[0]));

static void BuildCards(int page, const Snapshot& s, std::vector<CardDef>& out)
{
    switch (page) {
    case 0: // ---------------------------------------------------------------- Info
        out.push_back({ "PLAYER", 0, [&s]() {
            if (!s.valid) {
                UiText("No player data yet. Load a map and start playing.", true);
                return;
            }
            UiInfo("Speed 2D", "%.1f", s.speed2d);
            UiInfo("Speed 3D", "%.1f", s.speed3d);
            UiInfo("Vertical speed", "%.1f", s.vel.z);
            UiInfo("Position", "%.0f  %.0f  %.0f", s.pos.x, s.pos.y, s.pos.z);
            UiInfo("On ground", "%s", s.onGround ? "yes" : "no");
            UiInfo("Ducking", "%s", s.ducking ? "yes" : "no");
            if (s.frametime > 0.0f) UiInfo("Physics FPS", "%.0f", 1.0f / s.frametime);
            UiInfo("Movetype", "%d", s.movetype);
        } });
        out.push_back({ "STATUS", 1, []() {
            auto st = [](const char* name, bool ok) {
                UiInfoCol(name, ok ? Neon::Palette::Cyan : Neon::Palette::Magenta, ok ? "OK" : "MISSING");
            };
            st("Overlay", true);
            st("Player data hook", g_hookPlayerMove);
            st("Movement hook", g_hookCreateMove);
            st("Time hooks", g_clocksOk);
            st("Engine commands", g_clientCmd != nullptr);
            UiInfoCol("Teleport (optional)", g_teleportOk ? Neon::Palette::Cyan : Neon::Palette::TextDim, g_teleportOk ? "OK" : "n/a");
            UiInfoCol("Map geometry", g_map.ok ? Neon::Palette::Cyan : Neon::Palette::TextDim, g_map.ok ? g_map.name.c_str() : "not loaded");
            UiText("If something is MISSING, send hltas.log from the game folder.");
        } });
        out.push_back({ "RENDER", 1, []() {
            UiInfo("Render FPS", "%.0f", ImGui::GetIO().Framerate);
        } });
        break;

    case 1: // ---------------------------------------------------------------- Time
        out.push_back({ "TIME CONTROL", 0, []() {
            if (!g_clientCmd) UiText("Pause is unavailable (see hltas.log).", true);
            else {
                int b = UiButtons(g_paused ? "Resume" : "Pause", "Step frame");
                if (b == 1) TogglePause();
                if (b == 2) StepFrame();
            }
            if (!g_clocksOk) {
                UiText("Slow motion is unavailable (see hltas.log).", true);
            }
 else {
  bool slow = g_slowOn;
  if (UiToggle("Slow motion", &slow)) ToggleSlow();
  if (UiSlider("Factor", &g_slowScale, 0.01f, 2.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
      if (g_slowOn) ApplySlow();
  }
  if (g_uiDone) SaveConfig();
  UiText("Physics frames stay the same, they just run slower.");
}
} });
        out.push_back({ "SMOOTH SLOW-MO", 1, []() {
            if (UiToggle("More FPS while slowed", &g_smooth)) { ApplySlow(); SaveConfig(); }
            if (g_smooth) {
                if (UiInputInt("Normal fps_max", &g_normalFps)) {
                    if (g_normalFps < 20) g_normalFps = 20;
                    if (g_normalFps > 1000) g_normalFps = 1000;
                }
                if (g_uiDone) { ApplySlow(); SaveConfig(); }
                UiText("Physics frames get shorter: it is no longer 100 FPS physics. Turn slow-mo off before quitting the game.", true);
            }
 else {
  UiText("Raises the FPS limit while slowed, so the picture is smoother.");
}
} });
        break;

    case 2: // ---------------------------------------------------------------- Movement
        out.push_back({ "BUNNYHOP", 0, []() {
            if (!g_hookCreateMove) UiText("Movement hook is not installed (see hltas.log).", true);
            if (UiToggle("Auto bhop", &g_bhop)) SaveConfig();
            UiText("Hold jump: the tool jumps on every landing.");
        } });
        out.push_back({ "AUTO STRAFE", 0, []() {
            if (UiToggle("Auto strafe", &g_strafe)) SaveConfig();
            if (g_strafe) {
                if (UiCombo("Mode", &g_strafeMode, "Auto (follows view)\0Helper (A/D by mouse)\0")) SaveConfig();
                if (g_strafeMode == 0) {
                    if (UiCombo("Direction", &g_strafeDirMode, "Forward\0Left\0Right\0Back\0Auto (from W/A/S/D)\0")) SaveConfig();
                }
                if (UiToggle("Only while jump held", &g_strafeJumpOnly)) SaveConfig();
            }
            UiText("Steer with the mouse in the air: the strafes follow your view.");
        } });
        out.push_back({ "REPLAY", 1, []() {
            if (UiCombo("Go to start", &g_replayStartMode, "Walk there\0Teleport\0I stand there myself\0")) SaveConfig();
            if (g_replayStartMode != 2) {
                UiSlider("Max start distance", &g_replayMaxDist, 20.0f, 2000.0f, "%.0f u");
                if (g_uiDone) SaveConfig();
            }
            if (UiToggle("Include run-up when recording", &g_leadIn)) { if (!g_leadIn) g_lead.clear(); SaveConfig(); }
            if (UiToggle("Lock-step time (experimental)", &g_replayLockstep)) SaveConfig();
            if (g_replayLockstep) {
                if (UiCombo("Replay speed", &g_replayPace, "Real time\0As fast as possible\0")) SaveConfig();
            }
            if (UiToggle("Stop if it drifts", &g_stopOnDrift)) SaveConfig();
            if (g_stopOnDrift) {
                UiSlider("Drift limit", &g_driftLimit, 0.5f, 50.0f, "%.1f u");
                if (g_uiDone) SaveConfig();
            }
            UiText("Recorder -> Play. The game repeats the recorded input frame by frame. Run-up: a recording includes the frames since you last stood still, so the replay starts from a standstill and the speed before the first jump is reproduced.");
        } });
        out.push_back({ "TRAIL & STATS", 1, []() {
            if (UiToggle("Show path in world", &g_trailShow)) SaveConfig();
            if (UiToggle("Path through walls", &g_trailThrough)) SaveConfig();
            if (UiToggle("Jump stats", &g_jumpStatsOn)) SaveConfig();
            UiText("The path is drawn in the game world: while recording and for the run opened in Recorder.");
            if (!g_hookDrawTrans) UiText("World path hook is not installed (see hltas.log).", true);
        } });
        out.push_back({ "GROUND STRAFE", 1, []() {
            if (UiToggle("Ground strafe", &g_gstrafe)) SaveConfig();
            UiKey("Hold key", ACT_GSTRAFE);
            UiText("Hold the key on the ground: the tool ducks every other frame.");
        } });
        break;

    case 3: // ---------------------------------------------------------------- Recorder (рисуется отдельно)
        break;

    case 4: // ---------------------------------------------------------------- Models (подсветка)
        out.push_back({ "MODEL COLOURS", 0, []() {
            if (!g_glHooksOk) UiText("OpenGL hooks are not installed (see hltas.log).", true);
            if (UiToggle("Colour models", &g_chams.on)) SaveConfig();
            if (UiCombo("Style", &g_chams.style, "Tint (keep textures)\0Flat colour\0Glow\0")) SaveConfig();
            if (UiColor("Colour", g_chams.col)) SaveConfig();
            if (UiSlider("Brightness", &g_chams.bright, 0.5f, 4.0f, "%.2f")) {}
            if (g_uiDone) SaveConfig();
            if (UiToggle("Visible through walls", &g_chams.through)) SaveConfig();
            UiText("Applies to every 3D model except your weapon: monsters, people, items. Through walls draws them on top of the level.");
        } });
        out.push_back({ "WEAPON", 0, []() {
            if (UiToggle("Colour weapon", &g_weap.on)) SaveConfig();
            if (UiCombo("Style ", &g_weap.style, "Tint (keep textures)\0Flat colour\0Glow\0")) SaveConfig();
            if (UiColor("Colour ", g_weap.col)) SaveConfig();
            if (UiSlider("Brightness ", &g_weap.bright, 0.5f, 4.0f, "%.2f")) {}
            if (g_uiDone) SaveConfig();
            UiText("Your weapon model in first person.");
        } });
        out.push_back({ "PREVIEW", 1, []() {
            ImGui::PushFont(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
            if (!g_search[0]) ImGui::TextUnformatted("Models");
            ImGui::PopStyleColor();
            ImGui::PopFont();
            UiPreview(0, g_chams, 190.0f);
            ImGui::PushFont(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
            if (!g_search[0]) ImGui::TextUnformatted("Weapon");
            ImGui::PopStyleColor();
            ImGui::PopFont();
            UiPreview(1, g_weap, 150.0f);
            UiText("The preview shows how the settings look; it is not the real game model.");
        } });
        out.push_back({ "HOOK ACTIVITY", 1, []() {
            UiInfoCol("OpenGL hooks", g_glHooksOk ? Neon::Palette::Cyan : Neon::Palette::Magenta, g_glHooksOk ? "OK" : "MISSING");
            UiInfo("Colour hooks", "%d of 6", g_glColorHooks);
            UiInfo("Models recoloured / s", "%d", g_visHitsShown[0]);
            UiInfo("Weapon recoloured / s", "%d", g_visHitsShown[1]);
            UiText("If these stay at 0 while a style is on, send hltas.log: the game draws models in a way the hooks do not see.");
        } });
        break;

    case 5: // ---------------------------------------------------------------- Keys
        out.push_back({ "BINDINGS", 0, []() {
            for (int i = 0; i < ACT_COUNT; ++i) UiKey(kActionName[i], i);
            UiText("Click a button, then press a key.  Esc - cancel,  Backspace - unbind.");
        } });
        break;

    case 6: // ---------------------------------------------------------------- Settings
        out.push_back({ "INTERFACE", 0, []() {
            if (UiCombo("Theme", &g_theme, "Neon\0Mono\0")) { Neon::SetTheme(g_theme); SaveConfig(); }
        } });
        out.push_back({ "COMPATIBILITY", 0, []() {
            if (UiToggle("Hook GDI SwapBuffers", &g_hookGdiSwap)) SaveConfig();
            UiText("If a recording or streaming program (OBS, Discord) shows a black screen or freezes with the overlay on, turn this off and restart the game. Needs a restart.");
        } });
        out.push_back({ "CONFIG", 0, []() {
            if (UiButton("Save config")) { SaveConfig(); g_toast = 1.6f; }
            UiInfoCol("File", Neon::Palette::TextDim, "hltas.ini  (next to hl.exe)");
        } });
        out.push_back({ "ABOUT", 1, []() {
            UiInfo("Tool", "ArcHangel | Alpha");
            UiInfo("Game", "Half-Life 1, OpenGL");
            UiText("Single-player only. Start the game with -insecure.");
        } });
        break;
    }
}

// ---------------------------------------------------------------- страница Recorder
struct Cam3 { Vec3 pos, fwd, right, up; float f; ImVec2 c; };

static Vec3  V3(float x, float y, float z) { Vec3 v = { x, y, z }; return v; }
static Vec3  VSub(Vec3 a, Vec3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float VDot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3  VCross(Vec3 a, Vec3 b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static Vec3  VNorm(Vec3 a) { float l = sqrtf(VDot(a, a)); return l < 1e-6f ? V3(1, 0, 0) : V3(a.x / l, a.y / l, a.z / l); }

static Cam3 MakeCam(Vec3 pos, Vec3 fwd, ImVec2 c, float f)
{
    Cam3 k;
    k.pos = pos; k.fwd = VNorm(fwd);
    k.right = VNorm(VCross(k.fwd, V3(0, 0, 1)));
    k.up = VCross(k.right, k.fwd);
    k.f = f; k.c = c;
    return k;
}
static bool ProjectCam(const Cam3& k, Vec3 p, ImVec2* out)
{
    Vec3 v = VSub(p, k.pos);
    float z = VDot(v, k.fwd);
    if (z < 4.0f) return false;
    out->x = k.c.x + VDot(v, k.right) / z * k.f;
    out->y = k.c.y - VDot(v, k.up) / z * k.f;
    return true;
}

static void FmtTime(float t, char* b, int n)
{
    int m = (int)(t / 60.0f);
    snprintf(b, n, "%d:%05.2f", m, t - m * 60.0f);
}

// ---------------------------------------------------------------- карта в окне рекордера (OpenGL внутри draw-списка ImGui)
struct MapDrawParams
{
    ImVec2 p0, p1;
    int    mode;            // 0 = перспектива (Orbit/POV), 1 = сверху (ортогональная)
    Cam3   cam;
    float  sc;              // масштаб для вида сверху
    bool   cut;
    float  zcut;
    const Frame* fr;
    int    n, ph;
};
static MapDrawParams g_mapParams;

static void BuildMapLists(MapMesh& m)
{
    m.listTri = glGenLists(2);
    m.listLine = m.listTri + 1;
    glNewList(m.listTri, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (size_t i = 0; i + 5 < m.tri.size(); i += 6) {
        glColor3f(m.tri[i + 3], m.tri[i + 4], m.tri[i + 5]);
        glVertex3f(m.tri[i], m.tri[i + 1], m.tri[i + 2]);
    }
    glEnd();
    glEndList();
    glNewList(m.listLine, GL_COMPILE);
    glBegin(GL_LINES);
    for (size_t i = 0; i + 2 < m.lines.size(); i += 3) glVertex3f(m.lines[i], m.lines[i + 1], m.lines[i + 2]);
    glEnd();
    glEndList();
}

static void DrawTrailGL(const MapDrawParams& P, float alpha)
{
    if (P.n < 2) return;
    int stride = P.n > 6000 ? P.n / 6000 : 1;
    bool dim = P.ph > 0 && P.ph < P.n - 1;
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < P.n; i += stride) {
        const Frame& f = P.fr[i];
        ImVec4 c = HeatColor(Speed2D(f));
        glColor4f(c.x, c.y, c.z, (dim && i > P.ph) ? alpha * 0.3f : alpha);
        glVertex3f(f.pos.x, f.pos.y, f.pos.z);
    }
    glEnd();
}

static void MapRenderCallback(const ImDrawList*, const ImDrawCmd* cmd)
{
    const MapDrawParams& P = *(const MapDrawParams*)cmd->UserCallbackData;
    for (unsigned int l : g_staleLists) glDeleteLists(l, 2);
    g_staleLists.clear();
    MapMesh& m = g_map;
    if (!m.ok) return;
    if (!m.listTri) BuildMapLists(m);

    ImGuiIO& io = ImGui::GetIO();
    int W = (int)(P.p1.x - P.p0.x), H = (int)(P.p1.y - P.p0.y);
    if (W < 8 || H < 8) return;
    int X = (int)P.p0.x, Y = (int)io.DisplaySize.y - (int)P.p1.y;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glViewport(X, Y, W, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(X, Y, W, H);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.031f, 0.031f, 0.043f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    if (P.mode == 1) {
        double hw = (double)W * 0.5 / P.sc, hh = (double)H * 0.5 / P.sc;
        glOrtho(-hw, hw, -hh, hh, 1.0, 200000.0);
    }
    else {
        double n = 4.0;
        double r = n * (W * 0.5) / P.cam.f, t = n * (H * 0.5) / P.cam.f;
        glFrustum(-r, r, -t, t, n, 60000.0);
    }
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    const Cam3& k = P.cam;
    float mv[16] = {
        k.right.x, k.up.x, -k.fwd.x, 0.0f,
        k.right.y, k.up.y, -k.fwd.y, 0.0f,
        k.right.z, k.up.z, -k.fwd.z, 0.0f,
        -VDot(k.right, k.pos), -VDot(k.up, k.pos), VDot(k.fwd, k.pos), 1.0f };
    glLoadMatrixf(mv);

    if (P.cut) {                                   // срезаем всё выше пути, чтобы потолки не закрывали вид
        GLdouble eq[4] = { 0.0, 0.0, -1.0, (GLdouble)P.zcut };
        glClipPlane(GL_CLIP_PLANE0, eq);
        glEnable(GL_CLIP_PLANE0);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    glCallList(m.listTri);
    glDisable(GL_POLYGON_OFFSET_FILL);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.0f);
    glColor4f(0.02f, 0.03f, 0.05f, 0.45f);
    glCallList(m.listLine);

    // путь: бледный сквозь стены + яркий там, где виден
    glDisable(GL_DEPTH_TEST);
    glLineWidth(2.0f);
    DrawTrailGL(P, 0.30f);
    glEnable(GL_DEPTH_TEST);
    glLineWidth(3.0f);
    DrawTrailGL(P, 1.0f);

    if (P.ph >= 0 && P.ph < P.n) {
        glDisable(GL_DEPTH_TEST);
        glPointSize(10.0f);
        glBegin(GL_POINTS);
        glColor3f(1.0f, 1.0f, 1.0f);
        glVertex3f(P.fr[P.ph].pos.x, P.fr[P.ph].pos.y, P.fr[P.ph].pos.z);
        glEnd();
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
}

static void DrawRunView(const Run& r, ImVec2 size)
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##runview", size);
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    dl->AddRectFilled(p0, p1, IM_COL32(8, 8, 11, 255), 8.0f);

    const int n = (int)r.frames.size();
    if (n < 2) {
        const char* t = "Record a run or pick one from the list";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), Col(Neon::Palette::TextDim), t);
        dl->AddRect(p0, p1, Col(Neon::Palette::Border), 8.0f);
        return;
    }

    Vec3 mn = r.frames[0].pos, mx = mn;
    for (const Frame& f : r.frames) {
        mn.x = fminf(mn.x, f.pos.x); mn.y = fminf(mn.y, f.pos.y); mn.z = fminf(mn.z, f.pos.z);
        mx.x = fmaxf(mx.x, f.pos.x); mx.y = fmaxf(mx.y, f.pos.y); mx.z = fmaxf(mx.z, f.pos.z);
    }
    Vec3 ctr = V3((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
    float ext = fmaxf(fmaxf(mx.x - mn.x, mx.y - mn.y), fmaxf(mx.z - mn.z, 200.0f));
    int ph = g_playhead < 0 ? 0 : (g_playhead > n - 1 ? n - 1 : g_playhead);
    const Frame& cur = r.frames[ph];
    ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);

    std::function<bool(Vec3, ImVec2*)> proj;
    Cam3 camK = MakeCam(V3(0, 0, 0), V3(1, 0, 0), c, 1.0f);
    float scTop = 1.0f;
    if (g_viewMode == 0) {                                   // Orbit
        if (act && ImGui::IsMouseDragging(0, 0.0f)) {
            g_orbYaw -= io.MouseDelta.x * 0.4f;
            g_orbPitch += io.MouseDelta.y * 0.4f;
            if (g_orbPitch < 5.0f) g_orbPitch = 5.0f;
            if (g_orbPitch > 85.0f) g_orbPitch = 85.0f;
        }
        if (hov && io.MouseWheel != 0.0f) {
            g_orbZoom *= 1.0f - io.MouseWheel * 0.1f;
            if (g_orbZoom < 0.3f) g_orbZoom = 0.3f;
            if (g_orbZoom > 8.0f) g_orbZoom = 8.0f;
        }
        float yr = g_orbYaw * kPi / 180.0f, pr = g_orbPitch * kPi / 180.0f, dist = ext * 0.8f * g_orbZoom;
        Vec3 cam = V3(ctr.x + dist * cosf(pr) * cosf(yr), ctr.y + dist * cosf(pr) * sinf(yr), ctr.z + dist * sinf(pr));
        Cam3 k = MakeCam(cam, VSub(ctr, cam), c, size.y * 1.1f);
        camK = k;
        proj = [k](Vec3 p, ImVec2* o) { return ProjectCam(k, p, o); };
    }
    else if (g_viewMode == 1) {                            // Top
        float sc = fminf(size.x / fmaxf(mx.x - mn.x, 100.0f), size.y / fmaxf(mx.y - mn.y, 100.0f)) * 0.85f;
        scTop = sc;
        float zEye = fmaxf(g_map.ok ? g_map.mx.z : mx.z, mx.z) + 200.0f;
        camK = MakeCam(V3(ctr.x, ctr.y, zEye), V3(0, 0, -1), c, 1.0f);
        proj = [=](Vec3 p, ImVec2* o) { o->x = c.x + (p.x - ctr.x) * sc; o->y = c.y - (p.y - ctr.y) * sc; return true; };
    }
    else {                                                 // POV
        float yr = cur.yaw * kPi / 180.0f, pr = cur.pitch * kPi / 180.0f;
        Vec3 fwd = V3(cosf(pr) * cosf(yr), cosf(pr) * sinf(yr), -sinf(pr));
        Cam3 k = MakeCam(V3(cur.pos.x, cur.pos.y, cur.pos.z + 17.0f), fwd, c, size.x * 0.5f);
        camK = k;
        proj = [k](Vec3 p, ImVec2* o) { return ProjectCam(k, p, o); };
    }

    // карта + путь рисуются через OpenGL; без карты остаётся упрощённый вид
    const bool useMap = g_mapShow && g_map.ok;
    if (useMap) {
        MapDrawParams& P = g_mapParams;
        P.p0 = p0; P.p1 = p1;
        P.mode = (g_viewMode == 1) ? 1 : 0;
        P.cam = camK;
        P.sc = scTop;
        P.cut = g_mapCut && g_viewMode != 2;
        P.zcut = mx.z + g_mapCutOff;
        P.fr = r.frames.data();
        P.n = n;
        P.ph = ph;
        dl->AddCallback(MapRenderCallback, &P);
    }

    dl->PushClipRect(p0, p1, true);

    // сетка на уровне пола
    if (!useMap) {
        float step = ext > 1600.0f ? 200.0f : (ext > 800.0f ? 100.0f : 50.0f);
        float half = ext * 0.75f, z0 = mn.z - 36.0f;
        ImU32 gc = Col(Neon::WithAlpha(Neon::Palette::Text, 0.07f));
        float gx0 = floorf((ctr.x - half) / step) * step, gy0 = floorf((ctr.y - half) / step) * step;
        for (float gx = gx0; gx <= ctr.x + half; gx += step) {
            ImVec2 a, b;
            if (proj(V3(gx, ctr.y - half, z0), &a) && proj(V3(gx, ctr.y + half, z0), &b)) dl->AddLine(a, b, gc);
        }
        for (float gy = gy0; gy <= ctr.y + half; gy += step) {
            ImVec2 a, b;
            if (proj(V3(ctr.x - half, gy, z0), &a) && proj(V3(ctr.x + half, gy, z0), &b)) dl->AddLine(a, b, gc);
        }
    }

    // путь, окрашенный скоростью; «будущее» относительно ползунка приглушено
    if (!useMap) {
        int stride = n > 2500 ? n / 2500 : 1;
        bool dim = ph > 0 && ph < n - 1;
        ImVec2 pa; bool pv = false;
        for (int i = 0; i < n; i += stride) {
            const Frame& f = r.frames[i];
            ImVec2 pb;
            bool ok = proj(V3(f.pos.x, f.pos.y, f.pos.z), &pb);
            if (ok && pv) {
                ImVec4 hc = HeatColor(Speed2D(f));
                hc.w = (dim && i > ph) ? 0.30f : 1.0f;
                dl->AddLine(pa, pb, Col(hc), 2.4f);
            }
            pa = pb; pv = ok;
        }
    }

    // метки: старт, финиш, текущий кадр
    {
        ImVec2 q;
        if (proj(r.frames[0].pos, &q))     dl->AddCircleFilled(q, 5.0f, IM_COL32(70, 220, 120, 255));
        if (proj(r.frames[n - 1].pos, &q)) dl->AddCircleFilled(q, 5.0f, IM_COL32(240, 80, 80, 255));
        if (proj(cur.pos, &q)) {
            dl->AddCircleFilled(q, 6.0f, IM_COL32(255, 255, 255, 255));
            dl->AddCircle(q, 9.0f, Col(Neon::Palette::Cyan), 24, 2.0f);
        }
    }

    // подписи
    {
        char b[64];
        snprintf(b, sizeof(b), "%.0f u/s", Speed2D(cur));
        ImVec2 ts = ImGui::CalcTextSize(b);
        ImVec2 q(p0.x + 10.0f, p1.y - ts.y - 14.0f);
        dl->AddRectFilled(ImVec2(q.x - 6.0f, q.y - 3.0f), ImVec2(q.x + ts.x + 6.0f, q.y + ts.y + 3.0f), IM_COL32(0, 0, 0, 170), 6.0f);
        dl->AddText(q, Col(HeatColor(Speed2D(cur))), b);
        if (g_viewMode == 0) {
            const char* hint = "drag - rotate   wheel - zoom";
            ImVec2 hs = ImGui::CalcTextSize(hint);
            dl->AddText(ImVec2(p1.x - hs.x - 10.0f, p1.y - hs.y - 12.0f), Col(Neon::WithAlpha(Neon::Palette::TextDim, 0.8f)), hint);
        }
    }
    dl->PopClipRect();
    dl->AddRect(p0, p1, Col(Neon::Palette::Border), 8.0f);
}

static void DrawSpeedGraph(const Run& r, ImVec2 size)
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##graph", size);
    const int n = (int)r.frames.size();
    dl->AddRectFilled(p0, p1, IM_COL32(8, 8, 11, 255), 8.0f);
    if (n < 2) return;
    if (ImGui::IsItemActive()) {
        float t = (io.MousePos.x - p0.x) / size.x;
        g_playhead = (int)(t * (float)(n - 1) + 0.5f);
        if (g_playhead < 0) g_playhead = 0;
        if (g_playhead > n - 1) g_playhead = n - 1;
        g_play = false;
    }
    float maxSp = fmaxf(r.st.top, 100.0f);
    dl->PushClipRect(p0, p1, true);
    ImVec2 pa; bool pv = false;
    for (float px = 0.0f; px <= size.x; px += 2.0f) {
        int idx = (int)(px / size.x * (float)(n - 1));
        float sp = Speed2D(r.frames[idx]);
        ImVec2 pb(p0.x + px, p1.y - 6.0f - sp / maxSp * (size.y - 14.0f));
        if (pv) dl->AddLine(pa, pb, Col(HeatColor(sp)), 1.8f);
        pa = pb; pv = true;
    }
    float hx = p0.x + (float)g_playhead / (float)(n - 1) * size.x;
    dl->AddLine(ImVec2(hx, p0.y), ImVec2(hx, p1.y), Col(Neon::Palette::Text), 1.5f);
    dl->PopClipRect();
    dl->AddRect(p0, p1, Col(Neon::Palette::Border), 8.0f);
}

static void DrawTrailSettingsRows()
{
    ImGui::Dummy(ImVec2(300, 0));
    if (UiToggle("Show path in world", &g_trailShow)) SaveConfig();
    if (UiToggle("Path through walls", &g_trailThrough)) SaveConfig();
    if (UiToggle("Jump stats", &g_jumpStatsOn)) SaveConfig();
    if (UiCombo("Go to start", &g_replayStartMode, "Walk there\0Teleport\0I stand there myself\0")) SaveConfig();
    if (UiToggle("Include run-up when recording", &g_leadIn)) { if (!g_leadIn) g_lead.clear(); SaveConfig(); }
    if (UiToggle("Lock-step time (experimental)", &g_replayLockstep)) SaveConfig();
    if (UiToggle("Show map", &g_mapShow)) SaveConfig();
    if (UiToggle("Cut ceilings above path", &g_mapCut)) SaveConfig();
    if (g_mapCut) {
        UiSlider("Cut height above path", &g_mapCutOff, 0.0f, 600.0f, "%.0f u");
        if (g_uiDone) SaveConfig();
    }
    {
        static char mapbuf[40] = "";
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetNextItemWidth(190.0f);
        ImGui::InputTextWithHint("##mapn", "map name, e.g. c1a0", mapbuf, sizeof(mapbuf));
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Load map", ImVec2(102.0f, 0))) LoadMapByName(mapbuf);
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
        ImGui::TextWrapped("%s", g_mapMsg[0] ? g_mapMsg : (g_map.ok ? g_map.name.c_str() : "No map loaded"));
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
}

static void DrawRecorderPage(float w, float h)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::BeginChild("##rec", ImVec2(w, h), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // ---- верхняя строка
    ImGui::PushFont(g_fontBig);
    ImGui::TextUnformatted("Recorder");
    ImGui::PopFont();
    ImGui::SameLine(0, 12);
    ImGui::TextColored(Neon::Palette::TextDim, "%d runs", (int)g_runs.size());
    ImGui::SameLine(0, 14);
    if (g_rec) ImGui::TextColored(Neon::Palette::Magenta, "recording  %.1f s", g_recClock);
    else       ImGui::TextColored(Neon::Palette::TextDim, "idle");

    const float bwS = 96.0f, bwR = 110.0f;
    ImGui::SameLine(w - bwS - bwR - 10.0f);
    if (ImGui::Button("Settings", ImVec2(bwS, 0))) ImGui::OpenPopup("rec_settings");
    ImGui::SameLine(0, 10);
    if (g_rec) ImGui::PushStyleColor(ImGuiCol_Button, Neon::WithAlpha(Neon::Palette::Magenta, 0.55f));
    if (ImGui::Button(g_rec ? "Stop" : "Record", ImVec2(bwR, 0))) ToggleRecord();
    if (g_rec) ImGui::PopStyleColor();
    if (ImGui::BeginPopup("rec_settings")) { DrawTrailSettingsRows(); ImGui::EndPopup(); }
    ImGui::Dummy(ImVec2(0, 6));

    const float leftW = 270.0f;
    float colH = ImGui::GetContentRegionAvail().y;

    // ---- список забегов
    ImGui::BeginChild("##runs", ImVec2(leftW, colH), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::SetNextItemWidth(leftW);
        ImGui::InputTextWithHint("##flt", "Filter runs...", g_runFilter, sizeof(g_runFilter));
        if (g_runSort == 0) ImGui::PushStyleColor(ImGuiCol_Button, Neon::WithAlpha(Neon::Palette::Cyan, 0.30f));
        if (ImGui::Button("Name", ImVec2((leftW - 8.0f) * 0.5f, 0))) g_runSort = 0;
        if (g_runSort == 0) ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);
        if (g_runSort == 1) ImGui::PushStyleColor(ImGuiCol_Button, Neon::WithAlpha(Neon::Palette::Cyan, 0.30f));
        if (ImGui::Button("Longest", ImVec2((leftW - 8.0f) * 0.5f, 0))) g_runSort = 1;
        if (g_runSort == 1) ImGui::PopStyleColor();

        float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() - 10.0f;
        ImGui::BeginChild("##list", ImVec2(leftW, listH), ImGuiChildFlags_None);
        std::vector<const RunInfo*> shown;
        for (const RunInfo& ri : g_runs) {
            if (g_runFilter[0]) {
                char a[96], b[96];
                size_t i = 0;
                for (; ri.name[i] && i < sizeof(a) - 1; ++i) a[i] = (char)tolower((unsigned char)ri.name[i]);
                a[i] = 0;
                for (i = 0; g_runFilter[i] && i < sizeof(b) - 1; ++i) b[i] = (char)tolower((unsigned char)g_runFilter[i]);
                b[i] = 0;
                if (!strstr(a, b)) continue;
            }
            shown.push_back(&ri);
        }
        std::sort(shown.begin(), shown.end(), [](const RunInfo* a, const RunInfo* b) {
            if (g_runSort == 1) return a->time > b->time;
            return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
            });
        if (shown.empty()) ImGui::TextColored(Neon::Palette::TextDim, g_runs.empty() ? "No runs yet. Press Record." : "No matches.");
        for (size_t i = 0; i < shown.size(); ++i) {
            const RunInfo& ri = *shown[i];
            ImGui::PushID((int)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float iw = ImGui::GetContentRegionAvail().x;
            if (ImGui::InvisibleButton("##run", ImVec2(iw, 54.0f))) SelectRun(ri);
            bool sel = (ri.path == g_selectPath), hv = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + iw, p.y + 54.0f),
                Col(sel ? Neon::WithAlpha(Neon::Palette::Cyan, 0.14f) : (hv ? ImVec4(1, 1, 1, 0.05f) : Neon::Palette::Card)), 8.0f);
            if (sel) dl->AddRect(p, ImVec2(p.x + iw, p.y + 54.0f), Col(Neon::WithAlpha(Neon::Palette::Cyan, 0.55f)), 8.0f);
            dl->AddText(ImVec2(p.x + 14.0f, p.y + 9.0f), Col(Neon::Palette::Text), ri.name.c_str());
            char sub[64], tb[24];
            FmtTime(ri.time, tb, sizeof(tb));
            snprintf(sub, sizeof(sub), "%s   -   %d frames", tb, ri.frames);
            dl->AddText(g_fontSmall, 13.0f, ImVec2(p.x + 14.0f, p.y + 31.0f), Col(Neon::Palette::TextDim), sub);
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SetNextItemWidth(leftW);
        ImGui::InputTextWithHint("##nn", "Next run name (blank = auto)", g_nextName, sizeof(g_nextName));
    }
    ImGui::EndChild();

    // ---- подробности забега
    ImGui::SameLine(0, 14);
    float dw = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##detail", ImVec2(dw, colH), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const Run& r = g_curLoaded ? g_cur : g_recRun;
        const int n = (int)g_cur.frames.size();

        ImGui::PushFont(g_fontBig);
        ImGui::TextUnformatted(g_curLoaded ? g_cur.name.c_str() : "No run selected");
        ImGui::PopFont();
        if (g_curLoaded) {
            ImGui::SameLine(0, 12);
            float fps = g_cur.st.time > 0.0f ? g_cur.st.frames / g_cur.st.time : 0.0f;
            ImGui::TextColored(g_curDirty ? Neon::Palette::Magenta : Neon::Palette::TextDim,
                "%s  -  %.0f fps", g_curDirty ? "unsaved" : "saved", fps);
            if (!g_cur.frames.empty()) {
                const Frame& s0 = g_cur.frames[0];
                float sp0 = sqrtf(s0.vel.x * s0.vel.x + s0.vel.y * s0.vel.y);
                ImGui::SameLine(0, 12);
                if (sp0 > 10.0f) ImGui::TextColored(Neon::Palette::Magenta, "starts in motion (%.0f u/s)", sp0);
                else             ImGui::TextColored(Neon::Palette::TextDim, "starts at rest");
            }
        }

        if (g_replayActive)
        {
            if (g_replayPhase == 1)
                ImGui::TextColored(Neon::Palette::Cyan, "Walking to the start: %.0f u", g_walkDist);
            else
                ImGui::TextColored(Neon::Palette::Cyan, "Replaying in game: %d / %d   drift %.1f u", g_replayIdx, (int)g_replayRun.frames.size(), g_driftNow);
        }
        else if (g_replayMsgT > 0.0f || g_replayMsg[0])
            ImGui::TextColored(Neon::Palette::TextDim, "%s", g_replayMsg);
        else
            ImGui::TextColored(Neon::Palette::TextDim, " ");

        // ---- вид и действия
        const char* modes[3] = { "Orbit", "Top", "POV" };
        for (int m = 0; m < 3; ++m) {
            if (m) ImGui::SameLine(0, 6);
            bool on = (g_viewMode == m);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, Neon::WithAlpha(Neon::Palette::Cyan, 0.32f));
            if (ImGui::Button(modes[m], ImVec2(64, 0))) g_viewMode = m;
            if (on) ImGui::PopStyleColor();
        }
        ImGui::SameLine(0, 10);
        {
            bool on = g_mapShow;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, Neon::WithAlpha(Neon::Palette::Cyan, 0.32f));
            if (ImGui::Button(g_map.ok ? "Map" : "Map -", ImVec2(64, 0))) { g_mapShow = !g_mapShow; SaveConfig(); }
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && g_mapMsg[0]) ImGui::SetTooltip("%s", g_mapMsg);
        }

        // ---- действия
        if (g_replayActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, Neon::WithAlpha(Neon::Palette::Magenta, 0.55f));
            if (ImGui::Button("Stop", ImVec2(70, 0))) StopReplay("stopped");
            ImGui::PopStyleColor();
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, Neon::WithAlpha(Neon::Palette::Cyan, 0.40f));
            if (ImGui::Button("Play", ImVec2(70, 0))) { if (StartReplay()) SetMenu(false); }
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(0, 6);
        if (g_curDirty) {
            if (ImGui::Button("Save", ImVec2(64, 0))) {
                std::string path;
                if (SaveRun(g_cur, path)) { g_curDirty = false; RefreshRuns(); }
            }
            ImGui::SameLine(0, 6);
        }
        if (ImGui::Button("Trim", ImVec2(64, 0))) ImGui::OpenPopup("trim_pop");
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Delete", ImVec2(70, 0))) ImGui::OpenPopup("del_pop");
        ImGui::SameLine(0, 6);
        if (ImGui::Button(g_map.ok ? "Map..." : "Set map", ImVec2(70, 0))) ImGui::OpenPopup("map_pop");
        if (g_curLoaded && !g_cur.map.empty()) {
            ImGui::SameLine(0, 12);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Neon::Palette::TextDim, "map: %s", g_cur.map.c_str());
        }
        if (ImGui::BeginPopup("map_pop")) {
            static char mapname[40] = "";
            ImGui::Dummy(ImVec2(280, 0));
            ImGui::TextColored(Neon::Palette::TextDim, "Map name, e.g. c1a0 (file maps\\name.bsp)");
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputTextWithHint("##mn", "c1a0", mapname, sizeof(mapname));
            ImGui::SameLine(0, 8);
            if (ImGui::Button("Load", ImVec2(90, 0))) LoadMapByName(mapname);
            if (!g_cur.map.empty() && ImGui::Button("Use this run's map", ImVec2(280, 0))) LoadMapByName(g_cur.map);
            std::string cm = CurrentMapName();
            if (!cm.empty() && ImGui::Button("Use current game map", ImVec2(280, 0))) LoadMapByName(cm);
            ImGui::PushFont(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
            ImGui::TextWrapped("%s", g_mapMsg[0] ? g_mapMsg : (g_map.ok ? g_map.name.c_str() : "No map loaded"));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("trim_pop")) {
            ImGui::Dummy(ImVec2(220, 0));
            if (n > 1) {
                if (ImGui::Button("Cut before playhead", ImVec2(220, 0))) {
                    int k = g_playhead;
                    if (k > 0 && k < n) {
                        g_cur.frames.erase(g_cur.frames.begin(), g_cur.frames.begin() + k);
                        ComputeStats(g_cur); g_curDirty = true; g_playhead = 0; ImGui::CloseCurrentPopup();
                    }
                }
                if (ImGui::Button("Cut after playhead", ImVec2(220, 0))) {
                    int k = g_playhead;
                    if (k >= 0 && k < n - 1) {
                        g_cur.frames.erase(g_cur.frames.begin() + k + 1, g_cur.frames.end());
                        ComputeStats(g_cur); g_curDirty = true; ImGui::CloseCurrentPopup();
                    }
                }
            }
            else {
                ImGui::TextColored(Neon::Palette::TextDim, "Select a run first");
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("del_pop")) {
            ImGui::Dummy(ImVec2(220, 0));
            if (g_curLoaded) {
                ImGui::Text("Delete \"%s\"?", g_cur.name.c_str());
                if (ImGui::Button("Yes, delete", ImVec2(106, 0))) {
                    DeleteRunFile(g_curPath);
                    g_cur = Run(); g_curLoaded = false; g_curDirty = false; g_selectPath.clear();
                    g_playhead = 0; g_play = false;
                    RefreshRuns();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(0, 8);
                if (ImGui::Button("Cancel", ImVec2(106, 0))) ImGui::CloseCurrentPopup();
            }
            else {
                ImGui::TextColored(Neon::Palette::TextDim, "Nothing selected");
            }
            ImGui::EndPopup();
        }

        // ---- 3D/2D вид
        float below = 34.0f + 66.0f + 64.0f + 3 * 8.0f + 26.0f + 36.0f;
        float vh = ImGui::GetContentRegionAvail().y - below;
        if (vh < 120.0f) vh = 120.0f;
        DrawRunView(g_cur, ImVec2(dw, vh));

        // ---- таймлайн
        const Run& cr = g_cur;
        int cn = (int)cr.frames.size();
        if (g_play && cn > 1) {
            g_playAccum += io.DeltaTime;
            while (g_playhead < cn - 1) {
                float fd = cr.frames[g_playhead].msec * 0.001f;
                if (fd < 0.001f) fd = 0.01f;
                if (g_playAccum < fd) break;
                g_playAccum -= fd;
                ++g_playhead;
            }
            if (g_playhead >= cn - 1) { g_play = false; g_playAccum = 0.0f; }
        }
        if (ImGui::Button(g_play ? "||" : ">", ImVec2(34, 0))) {
            if (cn > 1) { if (!g_play && g_playhead >= cn - 1) g_playhead = 0; g_play = !g_play; g_playAccum = 0.0f; }
        }
        ImGui::SameLine(0, 10);
        char t1[24] = "0:00.00", t2[24] = "0:00.00";
        if (cn > 1) { FmtTime(cr.frames[g_playhead < cn ? g_playhead : cn - 1].t, t1, sizeof(t1)); FmtTime(cr.st.time, t2, sizeof(t2)); }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s / %s", t1, t2);
        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (cn > 1) {
            if (ImGui::SliderInt("##ph", &g_playhead, 0, cn - 1, "")) g_play = false;
        }
        else {
            int z = 0;
            ImGui::SliderInt("##ph", &z, 0, 1, "");
        }
        ImGui::Dummy(ImVec2(0, 4));

        // ---- график скорости
        DrawSpeedGraph(cr, ImVec2(dw, 64.0f));
        ImGui::Dummy(ImVec2(0, 4));

        // ---- сводка
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(dw, 62.0f));
            dl->AddRectFilled(p, ImVec2(p.x + dw, p.y + 62.0f), Col(Neon::Palette::Card), 10.0f);
            const RunStats& st = cr.st;
            char v[6][32], tt[24];
            FmtTime(st.time, tt, sizeof(tt));
            snprintf(v[0], 32, "%s", tt);
            snprintf(v[1], 32, "%d", st.frames);
            snprintf(v[2], 32, "%.0f u", st.distance);
            snprintf(v[3], 32, "%.0f u/s", st.top);
            snprintf(v[4], 32, "%.0f u/s", st.avg);
            snprintf(v[5], 32, "%d", st.jumps);
            const char* lb[6] = { "TIME", "FRAMES", "DISTANCE", "TOP SPEED", "AVG SPEED", "JUMPS" };
            float cw = dw / 6.0f;
            for (int i = 0; i < 6; ++i) {
                float x = p.x + cw * i + 16.0f;
                dl->AddText(g_fontSmall, 12.0f, ImVec2(x, p.y + 10.0f), Col(Neon::Palette::TextDim), lb[i]);
                dl->AddText(ImVec2(x, p.y + 30.0f), Col(i == 3 && cn > 1 ? Neon::Palette::Magenta : Neon::Palette::Text), cn > 1 || i == 99 ? v[i] : "-");
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

static void DrawMenu(const Snapshot& s)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ws(fminf(920.0f, io.DisplaySize.x - 30.0f), fminf(600.0f, io.DisplaySize.y - 30.0f));
    ImGui::SetNextWindowSize(ws, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - ws.x) * 0.5f, (io.DisplaySize.y - ws.y) * 0.5f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::Begin("##hltas", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    ImDrawList* wdl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 wsz = ImGui::GetWindowSize();
    const float sideW = 200.0f;

    // фон боковой панели со скруглением только слева
    wdl->AddRectFilled(wp, ImVec2(wp.x + sideW, wp.y + wsz.y), Col(Neon::Palette::Sidebar), 12.0f, ImDrawFlags_RoundCornersLeft);

    // ---------------------------------------------------------------- боковая панель
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 16));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##side", ImVec2(sideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::PushFont(g_fontBig);
        ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::Cyan);
        ImGui::TextUnformatted("ArcHangel");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
        ImGui::TextUnformatted("Alpha  -  Half-Life 1");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 14));

        const char* lastGroup = "";
        for (int i = 0; i < kNavCount; ++i) {
            if (strcmp(lastGroup, kNav[i].group) != 0) {
                lastGroup = kNav[i].group;
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushFont(g_fontSmall);
                ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
                ImGui::TextUnformatted(kNav[i].group);
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            ImGui::PushID(i);
            const bool child = kNav[i].parent >= 0;
            const float ind = child ? 18.0f : 0.0f;
            if (child) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ind);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            const float ih = child ? 32.0f : 36.0f;
            if (ImGui::InvisibleButton("##nav", ImVec2(w, ih))) { g_page = i; g_search[0] = 0; }
            bool hov = ImGui::IsItemHovered();
            bool sel = (g_page == i) && !g_search[0];
            if (sel || hov)
                wdl->AddRectFilled(p, ImVec2(p.x + w, p.y + ih),
                    Col(Neon::WithAlpha(Neon::Palette::Cyan, sel ? 0.16f : 0.07f)), 8.0f);
            ImVec2 ts = ImGui::CalcTextSize(kNav[i].name);
            if (child) {
                float lx = p.x - ind + 20.0f;
                wdl->AddLine(ImVec2(lx, p.y - 2.0f), ImVec2(lx, p.y + ih * 0.5f), Col(Neon::WithAlpha(Neon::Palette::TextDim, 0.5f)), 1.2f);
                wdl->AddLine(ImVec2(lx, p.y + ih * 0.5f), ImVec2(lx + 10.0f, p.y + ih * 0.5f), Col(Neon::WithAlpha(Neon::Palette::TextDim, 0.5f)), 1.2f);
                wdl->AddText(ImVec2(p.x + 16.0f, p.y + (ih - ts.y) * 0.5f),
                    Col(sel ? Neon::Palette::Text : Neon::Palette::TextDim), kNav[i].name);
            }
            else {
                ImVec2 c(p.x + 20.0f, p.y + 18.0f);
                wdl->AddCircle(c, 7.0f, Col(sel ? Neon::Palette::Cyan : Neon::Palette::TextDim), 24, 1.6f);
                if (sel) wdl->AddCircleFilled(c, 3.2f, Col(Neon::Palette::Cyan));
                wdl->AddText(ImVec2(p.x + 38.0f, p.y + (ih - ts.y) * 0.5f),
                    Col(sel ? Neon::Palette::Text : Neon::Palette::TextDim), kNav[i].name);
            }
            ImGui::PopID();
        }

        // статус внизу
        int okCount = 1 + (g_hookPlayerMove ? 1 : 0) + (g_hookCreateMove ? 1 : 0) + (g_clocksOk ? 1 : 0) + (g_clientCmd ? 1 : 0);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 54.0f);
        ImVec2 sp = ImGui::GetCursorScreenPos();
        bool allOk = okCount == 5;
        wdl->AddCircleFilled(ImVec2(sp.x + 8.0f, sp.y + 9.0f), 4.5f, Col(allOk ? Neon::Palette::Cyan : Neon::Palette::Magenta));
        ImGui::SetCursorScreenPos(ImVec2(sp.x + 22.0f, sp.y));
        ImGui::TextUnformatted("single-player");
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, Neon::Palette::TextDim);
        ImGui::SetCursorScreenPos(ImVec2(sp.x + 22.0f, sp.y + 20.0f));
        ImGui::Text("hooks: %d / 5 ok", okCount);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // ---------------------------------------------------------------- правая часть
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float pad = 22.0f;
        float mw = ImGui::GetWindowWidth();

        // шапка: хлебные крошки слева, поиск / Save / закрыть справа
        ImGui::SetCursorPos(ImVec2(pad, 17.0f));
        bool searching = g_search[0] != 0;
        const NavItem& nv = kNav[g_page];
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Neon::Palette::TextDim, "%s", searching ? "Search" : (nv.parent >= 0 ? kNav[nv.parent].name : nv.group));
        ImGui::SameLine(0, 8);
        ImGui::TextColored(Neon::Palette::TextDim, ">");
        ImGui::SameLine(0, 8);
        ImGui::TextUnformatted(searching ? "Results" : nv.name);

        const float closeW = 32.0f, saveW = 72.0f, searchW = 200.0f, gap = 10.0f;
        float rx = mw - pad - closeW - gap - saveW - gap - searchW;
        ImGui::SameLine(rx);
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##search", "Search features", g_search, sizeof(g_search));
        ImGui::SameLine(0, gap);
        if (ImGui::Button("Save", ImVec2(saveW, 0))) { SaveConfig(); g_toast = 1.6f; }
        ImGui::SameLine(0, gap);
        if (ImGui::Button("X", ImVec2(closeW, 0))) SetMenu(false);

        if (g_toast > 0.0f) {
            g_toast -= io.DeltaTime;
            ImVec2 tp = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddText(ImVec2(tp.x + rx - 70.0f, tp.y + 24.0f),
                Col(Neon::WithAlpha(Neon::Palette::Cyan, fminf(1.0f, g_toast))), "Saved");
        }

        ImVec2 mp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mp.x, mp.y + 60.0f), ImVec2(mp.x + mw, mp.y + 60.0f),
            Col(Neon::Palette::Border));

        if (!searching && g_page == PAGE_RECORDER) {
            ImGui::SetCursorPos(ImVec2(pad, 72.0f));
            DrawRecorderPage(mw - pad * 2.0f, ImGui::GetWindowHeight() - 72.0f - 14.0f);
        }
        else {
            // карточки
            std::vector<CardDef> cards;
            if (searching) { for (int pg = 0; pg < kNavCount; ++pg) BuildCards(pg, s, cards); }
            else { BuildCards(g_page, s, cards); }

            float colW = (mw - pad * 2.0f - 16.0f) * 0.5f;
            float colH = ImGui::GetWindowHeight() - 60.0f - 14.0f - 12.0f;
            for (int col = 0; col < 2; ++col) {
                ImGui::SetCursorPos(ImVec2(pad + col * (colW + 16.0f), 72.0f));
                char cid[16];
                snprintf(cid, sizeof(cid), "##col%d", col);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::BeginChild(cid, ImVec2(colW, colH), ImGuiChildFlags_None);
                for (auto& cd : cards) {
                    bool left = (cd.col == 0);
                    if (searching) left = ((&cd - &cards[0]) % 2 == 0);   // при поиске раскладываем по очереди
                    if ((col == 0) == left) UiCard(cd.title, cd.fn);
                }
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // тянуть окно можно за шапку
    {
        static bool dragging = false;
        ImVec2 hmin(wp.x + sideW, wp.y), hmax(wp.x + wsz.x, wp.y + 60.0f);
        if (ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(hmin, hmax) && !ImGui::IsAnyItemHovered()) dragging = true;
        if (!ImGui::IsMouseDown(0)) dragging = false;
        if (dragging) ImGui::SetWindowPos(ImVec2(wp.x + io.MouseDelta.x, wp.y + io.MouseDelta.y));
    }

    ImGui::End();
}

static bool InitImGui(HDC hdc)
{
    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd) return false;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                       // не мусорить imgui.ini в папке игры
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // шрифты из Windows, если найдутся; иначе останется встроенный
    g_fontMain = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    if (!g_fontMain) g_fontMain = io.Fonts->AddFontDefault();
    g_fontSmall = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 13.0f);
    if (!g_fontSmall) g_fontSmall = g_fontMain;
    g_fontBig = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 24.0f);
    if (!g_fontBig) g_fontBig = g_fontMain;

    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplOpenGL2_Init())   return false;

    Neon::SetTheme(g_theme);

    g_hwnd = hwnd;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProcHook);
    g_init = true;
    Log("overlay initialised, hwnd=%p", (void*)hwnd);
    return true;
}

static void RenderFrame(HDC hdc)
{
    if (!g_init && !InitImGui(hdc)) return;
    if (WindowFromDC(hdc) != g_hwnd) return;        // рисуем только в окно игры

    {
        static unsigned long long lastTry = 0;
        unsigned long long now = GetTickCount64();
        if (!g_teleportOk && now - lastTry > 3000ULL) { lastTry = now; TryFindServerFuncs(); }
    }

    {
        unsigned long long nowT = GetTickCount64();
        if (nowT - g_visTick >= 1000ULL) {
            g_visTick = nowT;
            g_visHitsShown[0] = g_visHits[0]; g_visHitsShown[1] = g_visHits[1];
            g_visHits[0] = g_visHits[1] = 0;
        }
    }

    g_passthru = true;
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    g_passthru = false;
    ImGui::NewFrame();

    UpdateStep();
    Snapshot s = ReadPlayer();
    DrawHud(s);
    if (g_menu) DrawMenu(s);

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}

static BOOL WINAPI hkWglSwap(HDC hdc)
{
    if (g_inSwap) return oWglSwap(hdc);
    g_inSwap = true;
    RenderFrame(hdc);
    BOOL r = oWglSwap(hdc);
    g_inSwap = false;
    return r;
}

static BOOL WINAPI hkGdiSwap(HDC hdc)
{
    if (g_inSwap) return oGdiSwap(hdc);
    g_inSwap = true;
    RenderFrame(hdc);
    BOOL r = oGdiSwap(hdc);
    g_inSwap = false;
    return r;
}

// ---------------------------------------------------------------- установка хуков
static DWORD WINAPI InitThread(LPVOID)
{
    Log("---- HLTas loaded ----");
    LoadConfig();
    RefreshRuns();

    HMODULE gl = nullptr, client = nullptr;
    for (int i = 0; i < 300 && !(gl && client); ++i) {   // ждём до 30 секунд
        gl = GetModuleHandleA("opengl32.dll");
        client = GetModuleHandleA("client.dll");
        if (!(gl && client)) Sleep(100);
    }

    if (!gl) {
        Log("opengl32.dll not found - renderer is not OpenGL");
        MessageBoxA(nullptr,
            "OpenGL не найден.\nВ Half-Life: Options -> Video -> Renderer выбери OpenGL и перезапусти игру.",
            "HLTas", MB_ICONWARNING);
        return 0;
    }

    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 0; }

    if (MH_CreateHookApi(L"opengl32", "wglSwapBuffers", (LPVOID)hkWglSwap, (LPVOID*)&oWglSwap) != MH_OK)
        Log("hook wglSwapBuffers failed");
    if (g_hookGdiSwap) {
        if (MH_CreateHookApi(L"gdi32", "SwapBuffers", (LPVOID)hkGdiSwap, (LPVOID*)&oGdiSwap) != MH_OK)
            Log("hook SwapBuffers failed");
    }
    else {
        Log("gdi32!SwapBuffers hook disabled by settings (hook_gdi_swap=0)");
    }
    {
        int ok = 0, total = 0;
        auto hk = [&](const char* name, LPVOID hook, LPVOID* orig) {
            ++total;
            if (MH_CreateHookApi(L"opengl32", name, hook, orig) == MH_OK) ++ok; else Log("hook %s failed", name);
            };
        hk("glBegin", (LPVOID)hkGlBegin, (LPVOID*)&oGlBegin);
        hk("glEnd", (LPVOID)hkGlEnd, (LPVOID*)&oGlEnd);
        hk("glDepthRange", (LPVOID)hkGlDepthRange, (LPVOID*)&oGlDepthRange);
        int before = ok;
        hk("glColor3f", (LPVOID)hkGlColor3f, (LPVOID*)&oGlColor3f);
        hk("glColor3fv", (LPVOID)hkGlColor3fv, (LPVOID*)&oGlColor3fv);
        hk("glColor4f", (LPVOID)hkGlColor4f, (LPVOID*)&oGlColor4f);
        hk("glColor4fv", (LPVOID)hkGlColor4fv, (LPVOID*)&oGlColor4fv);
        hk("glColor3ub", (LPVOID)hkGlColor3ub, (LPVOID*)&oGlColor3ub);
        hk("glColor4ub", (LPVOID)hkGlColor4ub, (LPVOID*)&oGlColor4ub);
        g_glColorHooks = ok - before;
        g_glHooksOk = oGlBegin && oGlEnd && oGlDepthRange && oGlColor4f;
        Log("model colour hooks: %d of %d installed", ok, total);
    }
    EngineRange();
    LoadLibraryW(L"winmm.dll");
    if (MH_CreateHookApi(L"kernel32", "QueryPerformanceCounter", (LPVOID)hkQPC, (LPVOID*)&oQPC) != MH_OK)
        Log("hook QueryPerformanceCounter failed");
    if (MH_CreateHookApi(L"winmm", "timeGetTime", (LPVOID)hkTimeGetTime, (LPVOID*)&oTimeGetTime) != MH_OK)
        Log("hook timeGetTime failed");
    if (MH_CreateHookApi(L"kernel32", "GetTickCount", (LPVOID)hkGetTickCount, (LPVOID*)&oGetTickCount) != MH_OK)
        Log("hook GetTickCount failed");
    InitClocks();
    Log("time hooks: %s (engine range %p..%p)", g_clocksOk ? "ok" : "unavailable", (void*)g_engLo, (void*)g_engHi);

    if (MH_CreateHookApi(L"kernel32", "CreateFileA", (LPVOID)hkCreateFileA, (LPVOID*)&oCreateFileA) != MH_OK)
        Log("hook CreateFileA failed (map name detection by file is unavailable)");
    if (MH_CreateHookApi(L"kernel32", "CreateFileW", (LPVOID)hkCreateFileW, (LPVOID*)&oCreateFileW) != MH_OK)
        Log("hook CreateFileW failed (map name detection by file is unavailable)");
    if (MH_CreateHookApi(L"user32", "SetCursorPos", (LPVOID)hkSetCursorPos, (LPVOID*)&oSetCursorPos) != MH_OK)
        Log("hook SetCursorPos failed");
    if (MH_CreateHookApi(L"user32", "GetCursorPos", (LPVOID)hkGetCursorPos, (LPVOID*)&oGetCursorPos) != MH_OK)
        Log("hook GetCursorPos failed");

    if (client) {
        void* target = (void*)GetProcAddress(client, "HUD_PlayerMove");
        if (target && MH_CreateHook(target, (LPVOID)hkPlayerMove, (LPVOID*)&oPlayerMove) == MH_OK) {
            g_hookPlayerMove = true;
            Log("hook HUD_PlayerMove ok");
            void* dt = (void*)GetProcAddress(client, "HUD_DrawTransparentTriangles");
            if (dt && MH_CreateHook(dt, (LPVOID)hkDrawTrans, (LPVOID*)&oDrawTrans) == MH_OK) {
                g_hookDrawTrans = true;
                Log("hook HUD_DrawTransparentTriangles ok");
            }
            else {
                Log("hook HUD_DrawTransparentTriangles failed (trail in world unavailable)");
            }
            void* cm = (void*)GetProcAddress(client, "CL_CreateMove");
            if (cm && MH_CreateHook(cm, (LPVOID)hkCreateMove, (LPVOID*)&oCreateMove) == MH_OK) {
                g_hookCreateMove = true;
                Log("hook CL_CreateMove ok");
            }
            else {
                Log("hook CL_CreateMove failed (export not found?)");
            }
        }
        else {
            Log("hook HUD_PlayerMove failed (export not found?)");
        }
    }
    else {
        Log("client.dll not found");
    }

    MH_EnableHook(MH_ALL_HOOKS);
    Log("hooks enabled");
    FindEngineFuncs();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}