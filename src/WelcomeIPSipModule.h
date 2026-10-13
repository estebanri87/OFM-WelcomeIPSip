#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WISChannelOwnerModule.h"
#include "WipSipLink.h"
#include "WipSipPairing.h"

// Busch-Welcome IP / ABB-Welcome door station on the KNX bus, without a SmartAP: one-time
// cloud pairing against the MyBuildings portal, then local SIP to the panel. Channels are
// outdoor stations; module-wide objects report the SIP link and pairing diagnostics.
//
// Stand (Schritt 3 des Plans): Pairing-Zustandsautomat (WipSipPairing) und SIP-Link
// (WipSipLink: REGISTER, Refresh, Keepalive, Reconnect) sind verdrahtet und über die
// Console testbar. Ruf-Erkennung und ETS-Parameter folgen in den nächsten Schritten; bis
// dahin kommen Zugangsdaten für "wis pair"/"wis connect" von der Konsole statt von der
// ETS. createChannel() liefert bis dahin immer nullptr.
class WelcomeIPSipModule : public WISChannelOwnerModule
{
  public:
    WelcomeIPSipModule();

    const std::string name() override;
    const std::string version() override;

    void setup(bool configured) override;
    void loop(bool configured) override;

    OpenKNX::Channel* createChannel(uint8_t _channelIndex) override;

    void showHelp() override;
    bool processCommand(const std::string command, bool debugKo) override;

  private:
    void printStatus();
    void printDoors();

    Wis::Pairing _pairing;
    Wis::Link _link;
    Wis::PairingSettings _testSettings; // see the class comment above; replaced by ETS params in a later step
    std::string _testPanelHost;
};

extern WelcomeIPSipModule openknxWelcomeIPSipModule;

#endif // OPENKNX_WELCOMEIP_SIP
