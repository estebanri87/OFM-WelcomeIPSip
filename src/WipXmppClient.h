#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include <WiFiClientSecure.h>
#include <cstdint>
#include <string>

// Minimal XMPP-over-TLS transport for the cloud door-unlock RPC: plaintext stream
// negotiation, in-place STARTTLS upgrade (mTLS with the pairing certificate), SASL
// EXTERNAL, resource bind/session, then one Jabber-RPC IQ round trip. Connect-on-demand,
// not a persistent link — WipXmppLink owns the lifecycle; this class only knows how to
// speak one short-lived XMPP stream. See the plan's "Türöffner (Schritt 8)".
namespace Wis
{
    struct XmppRpcResult
    {
        bool ok = false;
        std::string error;       // transport/XML-level failure (connection, timeout, IQ error)
        int resultCode = -1;      // from the methodResponse struct's "resultCode" member, -1 if absent
        std::string faultString; // set if the server answered with a Jabber-RPC <fault>
    };

    class XmppClient
    {
      public:
        // Connects to host:port, opens the XMPP stream against xmppDomain, upgrades to
        // mTLS via STARTTLS using certPem/keyPem (the same certificate the pairing flow
        // already obtained — see WipSipStore::PersistedPairing), authenticates via SASL
        // EXTERNAL and binds a resource/session. On any failure returns false with
        // outError set; the caller must not call sendUnlock() afterwards.
        bool connectAndAuthenticate(const std::string &host, uint16_t port, const std::string &xmppDomain,
                                     const std::string &certPem, const std::string &keyPem, uint32_t timeoutMs,
                                     std::string &outError);

        // Sends the RemoteInterface.unlockCloudContact Jabber-RPC call and waits for the
        // methodResponse. "to" is the destination JID (gateway_uuid@xmppDomain, see the
        // plan — unconfirmed addressing, verified only by trying it), "contactId" the
        // single string parameter (DoorInfo::address or ::secondUnlockAddress).
        XmppRpcResult sendUnlock(const std::string &to, const std::string &contactId, uint32_t timeoutMs);

        void close();

      private:
        bool sendRaw(const std::string &data);
        // Buffered byte read over _client — a persistent member (not a per-call local),
        // since a single TCP read can return more than one stanza's worth of bytes; a
        // fresh buffer per call would silently drop whatever was read but not yet
        // consumed. Transparent whether the connection is still in STARTTLS's plain phase
        // or already upgraded (NetworkClientSecure dispatches that internally).
        int getByte(uint32_t deadlineMs);
        // Reads one XML tag ("<...>", starting at the next '<'), appending every consumed
        // byte to outXml; see the .cpp for the classification this returns.
        bool readOneTag(uint32_t deadlineMs, std::string &outXml, bool &outIsClose, bool &outIsSelfClose,
                        bool &outIsPiOrDecl);
        // Reads one top-level element whose tag name is known in advance (every stanza we
        // ever receive on this connection is one of a handful of fixed shapes we already
        // know by name — "iq", "stream:features" — so this needs no general XML parser:
        // read the opening tag, stop immediately if self-closing, otherwise scan for the
        // one matching closing-tag string). false on timeout/EOF/oversized stanza.
        bool readElement(const char *tagName, std::string &outXml, uint32_t deadlineMs);
        // Consumes the server's opening "<?xml ...?><stream:stream ...>" (or just the
        // latter, some servers skip the declaration) without waiting for its closing tag,
        // which never arrives until the connection ends.
        bool readOpeningStreamTag(uint32_t deadlineMs, std::string &outError);
        bool sendStreamOpen(const std::string &xmppDomain, uint32_t deadlineMs, std::string &outError);
        std::string nextId();

        WiFiClientSecure _client;
        uint8_t _readBuf[512];
        size_t _readPos = 0;
        size_t _readLen = 0;
        uint32_t _idCounter = 0;
    };
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
