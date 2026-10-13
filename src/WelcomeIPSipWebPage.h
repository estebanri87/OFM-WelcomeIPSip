#pragma once
#include "WelcomeIPSipConfig.h"
#if defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "WipSipLink.h"
#include <cstdint>
#include <string>

class WelcomeIPSipModule;

namespace OpenKNX
{
    namespace Network
    {
        class WebRequest;
        class WebResponse;
        struct WebSocketFrame;
    } // namespace Network
} // namespace OpenKNX

// Statusseite /wis im WebInterface von OFM-Network: zeigt genau das, was auch die
// Konsole zeigt (Verbindung, Pairing-Stand, Stationen), nur im Browser. "Pairing
// starten"/"Unpair" rufen dieselben Aktionen wie "wis pair"/"wis unpair" auf. Eigene
// Datei nach dem Muster von OFM-NetworkBridge/NetworkBridgeConfigPage; Seiteninhalte
// und Live-Mechanik nach OFM-HueGatewayModule.
class WelcomeIPSipWebPage
{
  public:
    explicit WelcomeIPSipWebPage(WelcomeIPSipModule &module)
        : _module(module) {}

    void setup();
    // Vom Modul-loop() aufgerufen; prüft, ob sich die Ruf-Tabelle geändert hat, und
    // verschickt bei offener WebSocket-Verbindung ein Update (sonst Kurzschluss).
    void flushLiveUpdates();

  private:
    void handleStatus(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handlePair(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleUnpair(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleWsConnect(int clientId, bool connected);
    void sendLiveSnapshot(int clientId);
    std::string buildRingJson(const Wis::LinkState &link) const;

    WelcomeIPSipModule &_module;
    Wis::RingSlot _lastSentRing[Wis::kMaxOpenCalls];
    uint32_t _lastLiveSentMs = 0;
};

#endif // OPENKNX_WEBSERVER && KNX_IP
