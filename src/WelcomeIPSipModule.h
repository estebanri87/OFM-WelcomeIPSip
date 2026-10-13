#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WISChannelOwnerModule.h"

// Busch-Welcome IP / ABB-Welcome door station on the KNX bus, without a SmartAP: one-time
// cloud pairing against the MyBuildings portal, then local SIP to the panel. Channels are
// outdoor stations; module-wide objects report the SIP link and pairing diagnostics.
//
// Grundgerüst (Schritt 0 des Plans): Pairing, SIP-Client und ETS-Parameter folgen in den
// nächsten Schritten. createChannel() liefert bis dahin immer nullptr.
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
};

extern WelcomeIPSipModule openknxWelcomeIPSipModule;

#endif // OPENKNX_WELCOMEIP_SIP
