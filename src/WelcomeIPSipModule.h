#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WISChannelOwnerModule.h"
#include "WelcomeIPSipWebPage.h"
#include "WipSipLink.h"
#include "WipSipPairing.h"
#include "WipXmppLink.h"
#include <vector>

// Busch-Welcome IP / ABB-Welcome door station on the KNX bus, without a SmartAP: one-time
// cloud pairing against the MyBuildings portal, then local SIP to the panel. Channels are
// outdoor stations; module-wide objects report the SIP link and pairing diagnostics.
//
// Stand (Schritt 8 des Plans): ETS-Parameter, Kanalauswahl, Kommunikationsobjekte und der
// Türöffner (XMPP/Jabber-RPC per mTLS, siehe WipXmppLink) sind verdrahtet. Die genaue
// Ziel-JID und der RPC-Parameter sind Arbeitshypothesen aus einer Reverse-Engineering-
// Analyse, nicht am echten Gerät verifiziert — siehe den Plan und die Trace-Logs.
class WelcomeIPSipModule : public WISChannelOwnerModule
{
  public:
    WelcomeIPSipModule();

    const std::string name() override;
    const std::string version() override;

    void setup(bool configured) override;
    void loop(bool configured) override;
    void processInputKo(GroupObject &ko) override;

    OpenKNX::Channel* createChannel(uint8_t _channelIndex) override;

    void showHelp() override;
    bool processCommand(const std::string command, bool debugKo) override;

    Wis::Pairing& pairing() { return _pairing; }
    Wis::Link& link() { return _link; }
    Wis::XmppLink& xmppLink() { return _xmppLink; }
    // stationNumber is 1-based, matching the ETS parameter and the ACL door list order.
    bool doorForStation(uint8_t stationNumber, Wis::DoorInfo &out) const;
    // -1 if no configured channel uses that station number.
    int channelIndexForStation(uint8_t stationNumber);

  private:
    void printStatus();
    void printDoors();
    void startLinkIfReady();
    void startXmppLinkIfReady();
    void updateModuleKos();

    Wis::Pairing _pairing;
    Wis::Link _link;
    Wis::XmppLink _xmppLink;
    bool _linkStarted = false;
    bool _xmppLinkStarted = false;
    std::vector<Wis::DoorInfo> _doors; // parsed once when the Link starts

    bool _lastConnected = false;
    bool _lastInternalCall = false;
    std::string _lastDiag;

#if defined(OPENKNX_WEBSERVER)
    WelcomeIPSipWebPage _webPage{*this};
#endif
};

extern WelcomeIPSipModule openknxWelcomeIPSipModule;

#endif // OPENKNX_WELCOMEIP_SIP
