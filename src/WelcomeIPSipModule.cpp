#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WelcomeIPSipModule.h"
#include "WelcomeIPSipChannel.h"
#include <ArduinoJson.h>

namespace
{
    constexpr uint8_t kChannelCount = 4;

    const char *phaseName(Wis::Phase phase)
    {
        switch (phase)
        {
            case Wis::Phase::NONE: return "nicht gepaart";
            case Wis::Phase::KEY: return "Schluessel erzeugt";
            case Wis::Phase::CERT: return "Zertifikat erhalten";
            case Wis::Phase::DISCOVERED: return "Gateway gefunden";
            case Wis::Phase::CONNECT_SENT: return "wartet auf Freigabe";
            case Wis::Phase::ACL_RECEIVED: return "Freigabe erhalten";
            case Wis::Phase::PAIRED: return "verbunden";
        }
        return "?";
    }

    const char *linkPhaseName(Wis::LinkPhase phase)
    {
        switch (phase)
        {
            case Wis::LinkPhase::Disconnected: return "nicht verbunden";
            case Wis::LinkPhase::Connecting: return "verbinde";
            case Wis::LinkPhase::Registering: return "registriere";
            case Wis::LinkPhase::Registered: return "registriert";
        }
        return "?";
    }

    // "Aktiv" in der Modulliste von OpenKNX/Common. Ohne diese Liste ist das Modul an.
    bool moduleEnabled()
    {
#ifdef ParamBASE_ModuleEnabled_WIS
        return ParamBASE_ModuleEnabled_WIS;
#else
        return true;
#endif
    }
} // namespace

WelcomeIPSipModule::WelcomeIPSipModule()
    : WISChannelOwnerModule(kChannelCount)
{
}

const std::string WelcomeIPSipModule::name()
{
    return "WelcomeIP SIP";
}

const std::string WelcomeIPSipModule::version()
{
#ifdef MODULE_WelcomeIPSip_Version
    return MODULE_WelcomeIPSip_Version;
#else
    return "0.1.0";
#endif
}

OpenKNX::Channel* WelcomeIPSipModule::createChannel(uint8_t _channelIndex)
{
    if (ParamWIS_CHType == WelcomeIPSipChannel::TYPE_NONE) return nullptr;
    return new WelcomeIPSipChannel(_channelIndex, *this);
}

void WelcomeIPSipModule::setup(bool configured)
{
    WISChannelOwnerModule::setup(configured); // creates the channels
    if (!configured || !moduleEnabled()) return;

    Wis::PairingSettings settings;
    settings.username = ParamWIS_WISUserStr;
    settings.password = ParamWIS_WISPasswordStr;
    settings.clientName = ParamWIS_WISClientNameStr;
    settings.clientType = ParamWIS_WISClientType == 1 ? Wis::ClientTypeChoice::Welcome : Wis::ClientTypeChoice::FreeAtHomeNext;
    settings.gatewayUuidOverride = ParamWIS_WISGatewayUuidStr;
    _pairing.begin(settings);

    logInfoP("setup (Pairing-Task gestartet)");
}

void WelcomeIPSipModule::loop(bool configured)
{
    WISChannelOwnerModule::loop(configured);
    if (!configured || !moduleEnabled()) return;

    startLinkIfReady();
    updateModuleKos();
}

void WelcomeIPSipModule::startLinkIfReady()
{
    if (_linkStarted || !_pairing.hasSipCredentials()) return;

    Wis::PersistedPairing p;
    if (!_pairing.store().load(p) || p.sipUsername.empty() || p.sipDomain.empty()) return;

    _doors.clear();
    if (!p.doorsJson.empty())
    {
        JsonDocument doc;
        if (!deserializeJson(doc, p.doorsJson))
            for (JsonVariantConst d : doc.as<JsonArrayConst>())
            {
                Wis::DoorInfo door;
                door.name = d["name"].as<std::string>();
                door.stationId = d["stationId"].as<std::string>();
                door.address = d["address"].as<std::string>();
                _doors.push_back(door);
            }
    }

    Wis::LinkSettings link;
    link.panelHost = ParamWIS_WISPanelHostStr;
    link.sipUsername = p.sipUsername;
    link.sipPassword = p.sipPassword;
    link.sipDomain = p.sipDomain;
    for (const auto &d : _doors) link.doorStationIds.push_back(d.stationId);

    if (link.panelHost.empty())
    {
        logWarningP("Pairing abgeschlossen, aber keine Panel-Adresse konfiguriert");
        return;
    }

    _link.begin(link);
    _linkStarted = true;
    logInfoP("SIP-Link gestartet (%u Tueren aus der ACL)", (unsigned)_doors.size());
}

bool WelcomeIPSipModule::doorForStation(uint8_t stationNumber, Wis::DoorInfo &out) const
{
    if (stationNumber == 0 || stationNumber > _doors.size()) return false;
    out = _doors[stationNumber - 1];
    return true;
}

void WelcomeIPSipModule::updateModuleKos()
{
    Wis::PairingState pairing;
    _pairing.snapshot(pairing, pairing.seq - 1);
    Wis::LinkState link;
    _link.snapshot(link, link.seq - 1);

    const bool connected = link.phase == Wis::LinkPhase::Registered;
    if (connected != _lastConnected)
    {
        _lastConnected = connected;
        KoWIS_WISConnected.value(connected, DPT_State); // DPT 1.011
    }

    if (ParamWIS_WISUseInternalCall)
    {
        bool internalCall = false;
        for (const auto &slot : link.ring)
            if (slot.active && slot.internalCall)
            {
                internalCall = true;
                break;
            }
        if (internalCall != _lastInternalCall)
        {
            _lastInternalCall = internalCall;
            KoWIS_WISInternalCall.value(internalCall, DPT_Switch);
        }
    }

    // Verbindung hat Vorrang vor dem Pairing-Text, sobald beides vorliegt.
    std::string diag = connected ? "Registriert" : link.lastError[0] ? link.lastError : phaseName(pairing.phase);
    if (pairing.configStale) diag = "Pairing veraltet";
    if (diag.size() > 14) diag.resize(14);
    if (diag != _lastDiag)
    {
        _lastDiag = diag;
        KoWIS_WISDiag.value(diag.c_str(), DPT_String_8859_1);
    }
}

void WelcomeIPSipModule::processInputKo(GroupObject &ko)
{
    WISChannelOwnerModule::processInputKo(ko);
    if (!moduleEnabled()) return;

    if (ko.asap() == WIS_KoWISStartPairing) _pairing.enqueue(Wis::Pairing::CMD_PAIR);
}

void WelcomeIPSipModule::printStatus()
{
    Wis::PairingState state;
    _pairing.snapshot(state, state.seq - 1); // force a copy regardless of seq
    logInfoP("Phase: %s (%s)", phaseName(state.phase), state.diag);
    if (state.lastError[0]) logInfoP("letzter Fehler: %s", state.lastError);
    if (state.configStale) logInfoP("Hinweis: ETS-Konfiguration hat sich seit dem Pairing geaendert");
    if (state.phase == Wis::Phase::CONNECT_SENT && state.integrityDisplay[0])
        logInfoP("Integritaetscode fuer die App: %s", state.integrityDisplay);
    if (state.gatewayName[0]) logInfoP("Gateway: %s", state.gatewayName);
    if (state.phase == Wis::Phase::PAIRED) logInfoP("Aussenstationen: %u", (unsigned)state.doorCount);

    Wis::LinkState link;
    _link.snapshot(link, link.seq - 1);
    logInfoP("SIP-Link: %s (%s)", linkPhaseName(link.phase), link.diag);
    if (link.lastError[0]) logInfoP("SIP-Link letzter Fehler: %s", link.lastError);
    if (link.phase == Wis::LinkPhase::Registered)
        logInfoP("registriert seit %lu s, Heap-Minimum %u Byte", (unsigned long)((millis() - link.registeredSinceMs) / 1000),
                 (unsigned)link.minFreeHeapSeen);
    if (link.reconnectCount) logInfoP("Reconnects seit Start: %u", (unsigned)link.reconnectCount);
}

void WelcomeIPSipModule::printDoors()
{
    Wis::PersistedPairing p;
    if (!_pairing.store().load(p) || p.doorsJson.empty())
    {
        logInfoP("keine Tuerliste (noch nicht gepaart)");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, p.doorsJson))
    {
        logErrorP("Tuerliste unlesbar");
        return;
    }
    uint8_t n = 0;
    for (JsonVariantConst d : doc.as<JsonArrayConst>())
    {
        n++;
        logInfoP("%u: %s (station_id=%s)", (unsigned)n, d["name"].as<std::string>().c_str(),
                 d["stationId"].as<std::string>().c_str());
    }
}

void WelcomeIPSipModule::showHelp()
{
    openknx.console.printHelpLine("wis", "WelcomeIPSip status");
    openknx.console.printHelpLine("wis status", "wie \"wis\"");
    openknx.console.printHelpLine("wis doors", "Liste der Aussenstationen aus der ACL");
    openknx.console.printHelpLine("wis code", "Integritaetscode fuer die App");
    openknx.console.printHelpLine("wis pair", "Pairing anstossen/fortsetzen (Zugangsdaten aus der ETS)");
    openknx.console.printHelpLine("wis unpair", "Pairing-Zustand loeschen");
    openknx.console.printHelpLine("wis reregister", "SIP-Verbindung neu aufbauen");
    openknx.console.printHelpLine("wis calls", "Liste gerade offener Rufe (Klingeln)");
}

bool WelcomeIPSipModule::processCommand(const std::string command, bool debugKo)
{
    if (command.rfind("wis", 0) != 0) return false;

    if (command == "wis" || command == "wis status")
    {
        printStatus();
        return true;
    }
    if (command == "wis doors")
    {
        printDoors();
        return true;
    }
    if (command == "wis code")
    {
        Wis::PairingState state;
        _pairing.snapshot(state, state.seq - 1);
        if (state.integrityDisplay[0])
            logInfoP("%s", state.integrityDisplay);
        else
            logInfoP("kein Integritaetscode (Pairing noch nicht bei \"wartet auf Freigabe\")");
        return true;
    }
    if (command == "wis unpair")
    {
        _pairing.enqueue(Wis::Pairing::CMD_UNPAIR);
        logInfoP("Unpair angefordert");
        return true;
    }
    if (command == "wis pair")
    {
        _pairing.enqueue(Wis::Pairing::CMD_PAIR);
        logInfoP("Pairing angestossen");
        return true;
    }
    if (command == "wis reregister")
    {
        _link.enqueue(Wis::Link::CMD_REREGISTER);
        logInfoP("Neuverbindung angefordert");
        return true;
    }
    if (command == "wis calls")
    {
        Wis::LinkState link;
        _link.snapshot(link, link.seq - 1);
        bool any = false;
        for (const auto &slot : link.ring)
        {
            if (!slot.active) continue;
            any = true;
            logInfoP("Station %s, %s, seit %lu s", slot.stationId, slot.internalCall ? "intern" : "extern",
                     (unsigned long)((millis() - slot.sinceMs) / 1000));
        }
        if (!any) logInfoP("kein offener Ruf");
        return true;
    }
    return false;
}

#endif // OPENKNX_WELCOMEIP_SIP
