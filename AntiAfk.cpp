#include "AntiAfk.h"
#include <stdio.h>
#include <unordered_map>
#include <string>
#include <cmath>
#include <map>
#include <cstdarg>

int g_initialWarningTime = 15;  
int g_warningInterval = 30;    
int g_maxWarnings = 3;  
bool g_kickInsteadOfSpec = false;

#define IN_FORWARD    0x8ULL
#define IN_BACK       0x10ULL
#define IN_MOVELEFT   0x200ULL
#define IN_MOVERIGHT  0x400ULL

#define TEAM_SPECTATOR 1
#define MAX_PLAYERS 64

extern IFileSystem* g_pFullFileSystem;

struct PlayerAimData {
    float afkStartTime;
    bool initialWarningSent;
    int lastCountdown;
    int warningCount;
};

std::unordered_map<int, PlayerAimData> g_PlayerAimData;
std::map<std::string, std::string> g_AfkPhrases;

IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;

IUtilsApi* g_pUtils;
IPlayersApi* g_pPlayers;
ICookiesApi* g_pCookies;

AntiAfk g_AntiAfk;
PLUGIN_EXPOSE(AntiAfk, g_AntiAfk);

void AfkPrintToChat(int slot, const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    V_vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    std::string prefix = g_AfkPhrases["AFK_Prefix"];         
    std::string output = " " + prefix + " " + std::string(buffer);
    
    g_pUtils->PrintToChat(slot, output.c_str());
}

void LoadAfkTranslations()
{
    KeyValues::AutoDelete kvPhrases("Phrases");
    const char *pszPath = "addons/translations/antiafk.phrases.txt";
    if (!kvPhrases->LoadFromFile(g_pFullFileSystem, pszPath))
    {
        Warning("Failed to load %s\n", pszPath);
        return;
    }
    
    std::string language = std::string(g_pUtils->GetLanguage());
    const char* pszLang = language.c_str();
    
    for (KeyValues *pKey = kvPhrases->GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
    {
        g_AfkPhrases[std::string(pKey->GetName())] = std::string(pKey->GetString(pszLang));
    }
}

void LoadAfkConfig()
{
    KeyValues* kv = new KeyValues("AntiAfk");
    const char* pszPath = "addons/configs/AntiAfk/settings.ini";
    if (!kv->LoadFromFile(g_pFullFileSystem, pszPath))
    {
         g_pUtils->ErrorLog("[AntiAfk] Failed to load config from %s", pszPath);
         delete kv;
         return;
    }
    g_initialWarningTime = kv->GetFloat("initial_warning_time", 15);
    g_warningInterval = kv->GetFloat("warning_interval", 35);
    g_maxWarnings = kv->GetInt("max_warnings", 3);
    g_kickInsteadOfSpec = (kv->GetInt("kick_instead_of_spec", 0) != 0);
    delete kv;
}

CGameEntitySystem* GameEntitySystem()
{
    return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = g_pUtils->GetCEntitySystem();
    gpGlobals = g_pUtils->GetCGlobalVars();
}

bool AntiAfk::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

    g_SMAPI->AddListener(this, this);
    return true;
}

bool AntiAfk::Unload(char* error, size_t maxlen)
{
    ConVar_Unregister();
    if (m_pAimTimer)
    {
        g_pUtils->RemoveTimer(m_pAimTimer);
        m_pAimTimer = nullptr;
    }
    return true;
}

void AntiAfk::AllPluginsLoaded()
{
    char error[64];
    int ret;
    
    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string sBuffer = "meta unload " + std::to_string(g_PLID);
        engine->ServerCommand(sBuffer.c_str());
        return;
    }
    
    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string sBuffer = "meta unload " + std::to_string(g_PLID);
        engine->ServerCommand(sBuffer.c_str());
        return;
    }
    
    g_pCookies = (ICookiesApi*)g_SMAPI->MetaFactory(COOKIES_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Cookies system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string sBuffer = "meta unload " + std::to_string(g_PLID);
        engine->ServerCommand(sBuffer.c_str());
    }
    
    LoadAfkConfig();
    LoadAfkTranslations();

    g_pUtils->StartupServer(g_PLID, StartupServer);
    
    m_pAimTimer = g_pUtils->CreateTimer(1.0f, [this]() -> float {
        this->CheckPlayerAimMovement();
        return 1.0f;
    });
}

void AntiAfk::CheckPlayerAimMovement()
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (!g_pPlayers->IsInGame(i))
            continue;
        
        CCSPlayerController* pPlayer = (CCSPlayerController*)g_pEntitySystem->GetEntityInstance(CEntityIndex(i + 1));
        if (!pPlayer)
            continue;
        
        if (pPlayer->m_iTeamNum() == TEAM_SPECTATOR)
            continue;
  
        CCSPlayerPawn* pPawn = pPlayer->GetPlayerPawn();
        if (!pPawn)
            continue;
        
        if (!pPawn || !pPlayer->m_bPawnIsAlive())
        {
            if (g_PlayerAimData.find(i) != g_PlayerAimData.end())
            {
                auto &data = g_PlayerAimData[i];
                data.afkStartTime = gpGlobals->curtime;
                data.initialWarningSent = false;  
                data.lastCountdown = -1; 
            }
            continue;
        }

        if (g_PlayerAimData.find(i) == g_PlayerAimData.end())
        {
            PlayerAimData data;
            data.afkStartTime = gpGlobals->curtime;
            data.initialWarningSent = false;
            data.lastCountdown = -1;
            data.warningCount = 0;
            g_PlayerAimData[i] = data;
            if (g_pCookies)
                g_pCookies->SetCookie(i, "afk_warning_count", "0");
            continue;
        }
        
        auto &data = g_PlayerAimData[i];
     
        uint64_t buttons = 0;
        CPlayer_MovementServices* pMove = pPawn->m_pMovementServices(); 
        if (pMove && pMove->m_nButtons().m_pButtonStates())
            buttons = pMove->m_nButtons().m_pButtonStates()[0];
        
        bool isMoving = (buttons & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT)) != 0;

        if (isMoving)
        {
            data.afkStartTime = gpGlobals->curtime;
            data.initialWarningSent = false;
            data.lastCountdown = -1;
            data.warningCount = 0;
            if (g_pCookies)
                g_pCookies->SetCookie(i, "afk_warning_count", "0");
            continue;
        }
        
        float idleTime = gpGlobals->curtime - data.afkStartTime;

        if (idleTime >= g_warningInterval - 5.0f && idleTime < g_warningInterval)
        {
            int remaining = (int)floor(g_warningInterval - idleTime + 0.999f);
            if (data.lastCountdown != remaining)
            {
                char msg[256];
                if (data.warningCount == g_maxWarnings - 1)
                {
                    if (g_kickInsteadOfSpec)
                    {
                        const char* fmt = g_AfkPhrases.count("AFK_CountdownKick") ? g_AfkPhrases["AFK_CountdownKick"].c_str() :
                                       "Вы не двигаетесь, и через %d секунд вас кикнут за AFK.";
                        snprintf(msg, sizeof(msg), fmt, remaining);
                    }
                    else
                    {
                        const char* fmt = g_AfkPhrases.count("AFK_CountdownSpec") ? g_AfkPhrases["AFK_CountdownSpec"].c_str() :
                                       "Вы не двигаетесь, и через %d секунд вас переведут в наблюдатели.";
                        snprintf(msg, sizeof(msg), fmt, remaining);
                    }
                }
                else
                {
                    const char* fmt = g_AfkPhrases.count("AFK_CountdownKill") ? g_AfkPhrases["AFK_CountdownKill"].c_str() :
                                   "Вы не двигаетесь, и через %d секунд вас убьют за AFK.";
                    snprintf(msg, sizeof(msg), fmt, remaining);
                }
                AfkPrintToChat(i, "%s", msg);
                data.lastCountdown = remaining;
            }
        }

        if (idleTime >= g_initialWarningTime && idleTime < g_warningInterval && !data.initialWarningSent)
        {
            int remaining = (int)floor(g_warningInterval - idleTime + 0.999f);
            char msg[256];
            if (data.warningCount == g_maxWarnings - 1)
            {
                if (g_kickInsteadOfSpec)
                {
                    const char* fmt = g_AfkPhrases.count("AFK_CountdownKick") ? g_AfkPhrases["AFK_CountdownKick"].c_str() :
                                   "Вы не двигаетесь, и через %d секунд вас кикнут за AFK.";
                    snprintf(msg, sizeof(msg), fmt, remaining);
                }
                else
                {
                    const char* fmt = g_AfkPhrases.count("AFK_CountdownSpec") ? g_AfkPhrases["AFK_CountdownSpec"].c_str() :
                                   "Вы не двигаетесь, и через %d секунд вас переведут в наблюдатели.";
                    snprintf(msg, sizeof(msg), fmt, remaining);
                }
            }
            else
            {
                const char* fmt = g_AfkPhrases.count("AFK_CountdownKill") ? g_AfkPhrases["AFK_CountdownKill"].c_str() :
                               "Вы не двигаетесь, и через %d секунд вас убьют за AFK.";
                snprintf(msg, sizeof(msg), fmt, remaining);
            }            
            AfkPrintToChat(i, "%s", msg);
            data.initialWarningSent = true;
        }

        if (idleTime >= g_warningInterval)
        {
            if (data.warningCount < g_maxWarnings - 1)
            {
                g_pPlayers->CommitSuicide(i, false, true);
                data.warningCount++;
                char msg[256];
                const char* fmt = g_AfkPhrases.count("AFK_WarningKill") ? g_AfkPhrases["AFK_WarningKill"].c_str() :
                                  "Предупреждение %d/%d: вас убиты за AFK.";
                snprintf(msg, sizeof(msg), fmt, data.warningCount, g_maxWarnings);
                AfkPrintToChat(i, "%s", msg);
                if (g_pCookies)
                {
                    char warningStr[16];
                    snprintf(warningStr, sizeof(warningStr), "%d", data.warningCount);
                    g_pCookies->SetCookie(i, "afk_warning_count", warningStr);
                }
                data.lastCountdown = -1;
                data.afkStartTime = gpGlobals->curtime;
                data.initialWarningSent = false;
            }
            else
            {
                if (g_kickInsteadOfSpec)
                {
                    engine->DisconnectClient(CPlayerSlot(i), NETWORK_DISCONNECT_KICKED);
                }
                else
                {
                    g_pPlayers->ChangeTeam(i, TEAM_SPECTATOR);
                    const char* finalMsg = g_AfkPhrases.count("AFK_SpecFinal") ? g_AfkPhrases["AFK_SpecFinal"].c_str() :
                        "Вы были переведены в наблюдатели из-за AFK.";
                    AfkPrintToChat(i, "%s", finalMsg);
                }
                if (g_pCookies)
                    g_pCookies->SetCookie(i, "afk_warning_count", "0");
                g_PlayerAimData.erase(i);
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
    return "1.0";
}

const char* AntiAfk::GetDate()
{
    return __DATE__;
}

const char* AntiAfk::GetLogTag()
{
    return "Anti-Afk";
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
