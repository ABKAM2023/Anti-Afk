#include <stdio.h>
#include <unordered_map>
#include <string>
#include <cmath>
#include <map>
#include <cstdarg>
#include <algorithm>

#include "AntiAfk.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"

AntiAfk g_AntiAfk;
PLUGIN_EXPOSE(AntiAfk, g_AntiAfk);

IVEngineServer2*   engine              = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem*     g_pEntitySystem     = nullptr;
CGlobalVars*       gpGlobals           = nullptr;
CCSGameRules*      g_pGameRules        = nullptr;

IUtilsApi*   g_pUtils   = nullptr;
IPlayersApi* g_pPlayers = nullptr;

extern IFileSystem* g_pFullFileSystem;

#define TEAM_SPECTATOR 1
#define MAX_PLAYERS    64

#define IN_FORWARD    0x8ULL
#define IN_BACK       0x10ULL
#define IN_MOVELEFT   0x200ULL
#define IN_MOVERIGHT  0x400ULL

static int g_initialWarningTime = 15;
static int g_warningInterval = 35;
static int g_maxWarnings = 3;
static bool g_kickInsteadOfSpec = false;
static float g_aimThreshDeg = 1.0f;

static bool g_debugLog = true;

static bool g_canCheckAfk = false;

static inline void Dbg(const char* fmt, ...)
{
    if (!g_debugLog) return;
    char buf[1024];
    va_list va; va_start(va, fmt);
    V_vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    ConColorMsg(Color(0, 200, 255, 255), "[AntiAfk] %s\n", buf);
}

struct PlayerAfkState {
    float afkStart = 0.0f;
    bool warnedOnce = false;
    int lastCountdown = -1;
    int warnCount = 0;

    Vector lastAimDir = {0.f, 0.f, 0.f};
    bool haveAimDir = false;
};

static CTimer* m_pAimTimer = nullptr;

static std::unordered_map<int, PlayerAfkState> g_Afk;
static std::map<std::string, std::string> g_Phrases;

CGameEntitySystem* GameEntitySystem()
{
    return g_pUtils->GetCGameEntitySystem();
}

static inline bool DirFromTrace(const trace_info_t& tr, Vector& outDir)
{
    Vector d = tr.m_vEndPos - tr.m_vStartPos;
    float ls = d.x*d.x + d.y*d.y + d.z*d.z;
    if (ls < 1e-8f || std::isnan(ls) || std::isinf(ls)) return false;
    float inv = 1.0f / std::sqrt(ls);
    outDir.x = d.x * inv;
    outDir.y = d.y * inv;
    outDir.z = d.z * inv;
    return true;
}

static inline float AngleBetweenDirsDeg(const Vector& a, const Vector& b)
{
    float dot = a.x*b.x + a.y*b.y + a.z*b.z;
    if (dot >  1.f) dot = 1.f;
    if (dot < -1.f) dot = -1.f;
    return std::acos(dot) * (180.0f / float(M_PI));
}

static inline void ResetAfk(int slot, PlayerAfkState& st, bool clearAim = false)
{
    st.afkStart = gpGlobals ? gpGlobals->curtime : 0.0f;
    st.warnedOnce = false;
    st.lastCountdown = -1;
    if (clearAim) st.haveAimDir = false;
}

static inline void PrintChat(int slot, const char* fmt, ...)
{
    if (!g_pUtils) return;
    char buf[1024];
    va_list va; va_start(va, fmt);
    V_vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);

    auto it = g_Phrases.find("AFK_Prefix");
    if (it != g_Phrases.end())
    {
        std::string out = " " + it->second + " " + std::string(buf);
        g_pUtils->PrintToChat(slot, out.c_str());
    }
    else
    {
        g_pUtils->PrintToChat(slot, buf);
    }
}

static void LoadConfig()
{
    KeyValues* kv = new KeyValues("AntiAfk");
    const char* pszPath = "addons/configs/AntiAfk/settings.ini";
    if (!kv->LoadFromFile(g_pFullFileSystem, pszPath))
    {
        if (g_pUtils) g_pUtils->ErrorLog("[AntiAfk] Failed to load config from %s", pszPath);
        delete kv;
        return;
    }

    g_initialWarningTime = kv->GetFloat("initial_warning_time", 15.f);
    g_warningInterval = kv->GetFloat("warning_interval", 35.f);
    g_maxWarnings = kv->GetInt("max_warnings", 3);
    g_kickInsteadOfSpec = (kv->GetInt("kick_instead_of_spec", 0) != 0);
    g_aimThreshDeg = kv->GetFloat("aim_move_threshold_deg", 1.0f);
    g_debugLog = (kv->GetInt("debug_log",      1) != 0);

    delete kv;

    Dbg("Config loaded: warn=%d, interval=%d, max=%d, kick=%d, aim=%.2f, dbg=%d",
        g_initialWarningTime, g_warningInterval, g_maxWarnings, (int)g_kickInsteadOfSpec,
        g_aimThreshDeg, (int)g_debugLog);
}

static void LoadPhrases()
{
    KeyValues::AutoDelete kv("Phrases");
    const char *pszPath = "addons/translations/antiafk.phrases.txt";
    if (!kv->LoadFromFile(g_pFullFileSystem, pszPath))
        return;

    const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "en";
    for (KeyValues *p = kv->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey())
        g_Phrases[p->GetName()] = p->GetString(lang);
}

static void OnRoundStartEvent(const char* /*name*/, IGameEvent* /*event*/, bool /*dont*/)
{
    g_canCheckAfk = true;
    for (auto& kv : g_Afk) ResetAfk(kv.first, kv.second, true);
    Dbg("round_start: checks ON; reset all AFK timers");
}
static void OnMatchEndEvent(const char* name, IGameEvent* /*event*/, bool /*dont*/)
{
    g_canCheckAfk = false;
    for (auto& kv : g_Afk) kv.second.warnCount = 0;
    Dbg("%s: checks OFF", name);
}

void CheckAfkTick();

void StartupServer()
{
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = g_pUtils->GetCEntitySystem();
    gpGlobals = g_pUtils->GetCGlobalVars();
    Dbg("StartupServer done");
}

void GetGameRules()
{
    g_pGameRules = g_pUtils->GetCCSGameRules();
    Dbg("GetGameRules: %p", (void*)g_pGameRules);

    if (g_pGameRules && !m_pAimTimer)
    {
        m_pAimTimer = g_pUtils->CreateTimer(1.0f, []() -> float {
            CheckAfkTick();
            return 1.0f;
        });
        Dbg("Timer created (1.0s)");
    }
}

bool AntiAfk::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

    ConVar_Register(FCVAR_GAMEDLL);

    g_SMAPI->AddListener(this, this);
    return true;
}

bool AntiAfk::Unload(char *error, size_t maxlen)
{
    ConVar_Unregister();
    if (m_pAimTimer && g_pUtils)
    {
        g_pUtils->RemoveTimer(m_pAimTimer);
        m_pAimTimer = nullptr;
    }
    Dbg("Plugin unloaded");
    return true;
}

void AntiAfk::AllPluginsLoaded()
{
    char error[64];
    int ret;

    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pPlayers)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    LoadConfig();
    LoadPhrases();

    g_pUtils->StartupServer(g_PLID, StartupServer);
    g_pUtils->OnGetGameRules(g_PLID, GetGameRules);

    g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStartEvent);
    g_pUtils->HookEvent(g_PLID, "cs_win_panel_match", OnMatchEndEvent);
    g_pUtils->HookEvent(g_PLID, "game_end", OnMatchEndEvent);

    Dbg("AllPluginsLoaded: hooks set");
}

void CheckAfkTick()
{
    if (!g_pGameRules && g_pUtils) {
        g_pGameRules = g_pUtils->GetCCSGameRules();
        if (g_pGameRules) Dbg("Tick: re-fetched gamerules=%p", (void*)g_pGameRules);
    }

    if (!g_pPlayers || !gpGlobals) { Dbg("Skip tick: no players/globals"); return; }
    if (!g_pGameRules)            { Dbg("Skip tick: no gamerules"); return; }

    if (!g_canCheckAfk) { Dbg("Skip tick: checks OFF (waiting round_start or in postgame)"); return; }

    const bool inWarmup  = g_pGameRules->m_bWarmupPeriod();
    if (inWarmup) { Dbg("Tick: warmup active -> soft reset"); }

    for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    {
        if (!g_pPlayers->IsInGame(slot))
            continue;

        if (g_pPlayers->IsFakeClient(slot))
            continue;

        bool isSpec = false;
        bool isAlive = true;
        CCSPlayerController* pCtl = nullptr;

        if (g_pEntitySystem)
        {
            pCtl = (CCSPlayerController*)g_pEntitySystem->GetEntityInstance(CEntityIndex(slot + 1));
            if (pCtl)
            {
                isSpec  = (pCtl->m_iTeamNum() == TEAM_SPECTATOR);
                isAlive = pCtl->m_bPawnIsAlive();
            }
        }
        if (isSpec) continue;

        auto it = g_Afk.find(slot);
        if (it == g_Afk.end())
        {
            PlayerAfkState st{};
            st.afkStart = gpGlobals->curtime;
            g_Afk.emplace(slot, st);
            Dbg("Init slot %d (afkStart=%.2f)", slot, st.afkStart);
        }
        PlayerAfkState& st = g_Afk[slot];

        if (inWarmup)
        {
            ResetAfk(slot, st, true);
            continue;
        }

        if (!isAlive)
        {
            ResetAfk(slot, st, true);
            continue;
        }

        bool isMovingKeys = false;
        if (pCtl && isAlive)
        {
            if (CCSPlayerPawn* pPawn = pCtl->GetPlayerPawn())
            {
                if (auto* pMove = pPawn->m_pMovementServices())
                {
                    uint64_t btn = 0;
                    if (pMove->m_nButtons().m_pButtonStates())
                        btn = pMove->m_nButtons().m_pButtonStates()[0];
                    isMovingKeys = (btn & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT)) != 0;
                }
            }
        }

        Vector curDir; bool haveDir = false; float deltaAim = 0.f; bool isAiming = false;
        {
            trace_info_t tr = g_pPlayers->RayTrace(slot);
            haveDir = DirFromTrace(tr, curDir);
            if (haveDir)
            {
                if (!st.haveAimDir) { st.lastAimDir = curDir; st.haveAimDir = true; }
                deltaAim = AngleBetweenDirsDeg(st.lastAimDir, curDir);
                isAiming = (deltaAim >= g_aimThreshDeg);
            }
        }

        if (isMovingKeys || isAiming)
        {
            if (isMovingKeys) Dbg("Slot %d: WASD -> reset", slot);
            if (isAiming)     Dbg("Slot %d: aim moved (%.2f deg) -> reset", slot, deltaAim);
            ResetAfk(slot, st);
        }

        if (haveDir) st.lastAimDir = curDir;

        float idle = gpGlobals->curtime - st.afkStart;

        if (idle >= g_warningInterval - 5.0f && idle < g_warningInterval)
        {
            int remain = (int)floor(g_warningInterval - idle + 0.999f);
            if (st.lastCountdown != remain)
            {
                char msg[256];
                if (st.warnCount == g_maxWarnings - 1)
                {
                    if (g_kickInsteadOfSpec)
                    {
                        auto itP = g_Phrases.find("AFK_CountdownKick");
                        snprintf(msg, sizeof(msg),
                            itP != g_Phrases.end() ? itP->second.c_str() :
                            "Вы не двигаетесь, и через %d секунд вас кикнут за AFK.", remain);
                    }
                    else
                    {
                        auto itP = g_Phrases.find("AFK_CountdownSpec");
                        snprintf(msg, sizeof(msg),
                            itP != g_Phrases.end() ? itP->second.c_str() :
                            "Вы не двигаетесь, и через %d секунд вас переведут в наблюдатели.", remain);
                    }
                }
                else
                {
                    auto itP = g_Phrases.find("AFK_CountdownKill");
                    snprintf(msg, sizeof(msg),
                        itP != g_Phrases.end() ? itP->second.c_str() :
                        "Вы не двигаетесь, и через %d секунд вас убьют за AFK.", remain);
                }
                PrintChat(slot, "%s", msg);
                st.lastCountdown = remain;
                Dbg("Slot %d: countdown %d", slot, remain);
            }
        }

        if (idle >= g_initialWarningTime && idle < g_warningInterval && !st.warnedOnce)
        {
            int remain = (int)floor(g_warningInterval - idle + 0.999f);
            char msg[256];
            if (st.warnCount == g_maxWarnings - 1)
            {
                if (g_kickInsteadOfSpec)
                {
                    auto itP = g_Phrases.find("AFK_CountdownKick");
                    snprintf(msg, sizeof(msg),
                        itP != g_Phrases.end() ? itP->second.c_str() :
                        "Вы не двигаетесь, и через %d секунд вас кикнут за AFK.", remain);
                }
                else
                {
                    auto itP = g_Phrases.find("AFK_CountdownSpec");
                    snprintf(msg, sizeof(msg),
                        itP != g_Phrases.end() ? itP->second.c_str() :
                        "Вы не двигаетесь, и через %d секунд вас переведут в наблюдатели.", remain);
                }
            }
            else
            {
                auto itP = g_Phrases.find("AFK_CountdownKill");
                snprintf(msg, sizeof(msg),
                    itP != g_Phrases.end() ? itP->second.c_str() :
                    "Вы не двигаетесь, и через %d секунд вас убьют за AFK.", remain);
            }
            PrintChat(slot, "%s", msg);
            st.warnedOnce = true;
            Dbg("Slot %d: first warning, remain=%d", slot, remain);
        }

        if (idle >= g_warningInterval)
        {
            if (st.warnCount < g_maxWarnings - 1)
            {
                if (isAlive)
                    g_pPlayers->CommitSuicide(slot, false, true);
                st.warnCount++;

                char msg[256];
                auto itP = g_Phrases.find("AFK_WarningKill");
                snprintf(msg, sizeof(msg),
                    itP != g_Phrases.end() ? itP->second.c_str() :
                    "Предупреждение %d/%d: вы убиты за AFK.",
                    st.warnCount, g_maxWarnings);
                PrintChat(slot, "%s", msg);

                Dbg("Slot %d: soft punish (kill), warnCount=%d", slot, st.warnCount);

                st.lastCountdown = -1;
                st.afkStart = gpGlobals->curtime;
                st.warnedOnce = false;
            }
            else
            {
                if (g_kickInsteadOfSpec)
                {
                    engine->DisconnectClient(CPlayerSlot(slot), NETWORK_DISCONNECT_KICKED);
                    Dbg("Slot %d: final punish -> KICK", slot);
                }
                else
                {
                    g_pPlayers->ChangeTeam(slot, TEAM_SPECTATOR);
                    auto itP = g_Phrases.find("AFK_SpecFinal");
                    PrintChat(slot, "%s",
                        itP != g_Phrases.end() ? itP->second.c_str()
                                               : "Вы были переведены в наблюдатели из-за AFK.");
                    Dbg("Slot %d: final punish -> SPEC", slot);
                }

                g_Afk.erase(slot);
            }
        }
    }
}

const char* AntiAfk::GetLicense()
{
    return "GPL";
}

const char* AntiAfk::GetVersion()
{
    return "1.1";
}

const char* AntiAfk::GetDate()
{
    return __DATE__;
}

const char *AntiAfk::GetLogTag()
{
    return "[AntiAfk]";
}

const char* AntiAfk::GetAuthor()
{
    return "ABKAM";
}

const char* AntiAfk::GetDescription()
{
    return "Anti-Afk";
}

const char* AntiAfk::GetName()
{
    return "Anti-Afk";
}

const char* AntiAfk::GetURL()
{
    return "https://discord.gg/ChYfTtrtmS";
}
