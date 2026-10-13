#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WelcomeIPSipModule.h"

namespace
{
    // Anzahl der Kanäle (Außenstationen) kommt mit der ETS-Einbindung (op:define
    // NumChannels) in einem späteren Schritt; bis dahin fest auf den geplanten Wert.
    constexpr uint8_t kChannelCount = 4;
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

    logInfoP("setup (Grundgerüst, Pairing/SIP folgen)");
}

void WelcomeIPSipModule::loop(bool configured)
{
    WISChannelOwnerModule::loop(configured);
}

void WelcomeIPSipModule::printStatus()
{
    logInfoP("WelcomeIPSip: Grundgerüst, noch kein Pairing/SIP implementiert");
}

void WelcomeIPSipModule::showHelp()
{
    openknx.console.printHelpLine("wis", "WelcomeIPSip status");
}

bool WelcomeIPSipModule::processCommand(const std::string command, bool debugKo)
{
    if (command != "wis") return false;
    printStatus();
    return true;
}

#endif // OPENKNX_WELCOMEIP_SIP
