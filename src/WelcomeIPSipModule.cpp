#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WelcomeIPSipModule.h"
#include <ArduinoJson.h>

namespace
{
    // Anzahl der Kanäle (Außenstationen) kommt mit der ETS-Einbindung (op:define
    // NumChannels) in einem späteren Schritt; bis dahin fest auf den geplanten Wert.
    constexpr uint8_t kChannelCount = 4;
    const char *const kTestClientName = "OpenKNX-WelcomeIPSip";

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
    // Kanaltyp-Auswertung (Kanalauswahl-Pattern) folgt mit der ETS-Einbindung.
    return nullptr;
}

void WelcomeIPSipModule::setup(bool configured)
{
    WISChannelOwnerModule::setup(configured); // creates the channels
    if (!configured) return;

    // Platzhalter bis zur ETS-Einbindung (Schritt 5): Benutzername/Passwort kommen bis
    // dahin per Konsole ("wis pair <Benutzername> <Passwort>"), nicht aus Parametern.
    // Pairing::begin() startet den Task erst dort, sobald die Zugangsdaten bekannt sind
    // (begin() ist nur einmal wirksam) - ab Schritt 5 stattdessen hier mit den
    // ETS-Parametern.
    _testSettings.clientName = kTestClientName;

    logInfoP("setup (SIP-Laufzeit folgt; \"wis pair <Benutzername> <Passwort>\" zum Testen)");
}

void WelcomeIPSipModule::loop(bool configured)
{
    WISChannelOwnerModule::loop(configured);
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
    openknx.console.printHelpLine("wis pair [<Benutzername> <Passwort>]", "Pairing anstossen/fortsetzen");
    openknx.console.printHelpLine("wis unpair", "Pairing-Zustand loeschen");
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
    if (command.rfind("wis pair", 0) == 0)
    {
        // Vor der ETS-Einbindung (Schritt 5) kommen die Pairing-Zugangsdaten einmalig per
        // Konsole; danach greift "wis pair" nur noch auf die ETS-Parameter zu.
        if (command.size() > 9)
        {
            const std::string rest = command.substr(9);
            const size_t sp = rest.find(' ');
            if (sp == std::string::npos)
            {
                logErrorP("wis pair <Benutzername> <Passwort>");
                return true;
            }
            _testSettings.username = rest.substr(0, sp);
            _testSettings.password = rest.substr(sp + 1);
        }
        if (_testSettings.username.empty() || _testSettings.password.empty())
        {
            logErrorP("wis pair <Benutzername> <Passwort> (einmalig, vor der ETS-Einbindung)");
            return true;
        }
        _pairing.begin(_testSettings); // no-op if the task is already running (first call wins)
        _pairing.enqueue(Wis::Pairing::CMD_PAIR);
        logInfoP("Pairing angestossen");
        return true;
    }
    return false;
}

#endif // OPENKNX_WELCOMEIP_SIP
