#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include <WiFiClientSecure.h>
#include <cstdint>
#include <string>
#include <vector>

// Minimal SIP-over-TLS transport: frame parsing/building and one persistent
// WiFiClientSecure connection. No transaction/dialog logic (that is WipSipLink's job) —
// this class only knows how to send one SIP message and read the next one off the wire.
namespace Wis
{
    struct SipFrame
    {
        std::string startLine; // "METHOD uri SIP/2.0" or "SIP/2.0 <code> <reason>"
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;

        bool isResponse() const { return startLine.rfind("SIP/2.0 ", 0) == 0; }
        bool isRequest() const { return !startLine.empty() && !isResponse(); }
        std::string method() const; // "" for a response
        int statusCode() const;     // -1 if not a response or unparsable

        // Case-insensitive; returns the first match, "" if absent.
        std::string header(const char *name) const;
    };

    class SipClient
    {
      public:
        bool connect(const std::string &host, uint16_t port, uint32_t timeoutMs, std::string &outError);
        void close();
        bool connected();

        std::string localIp();  // dotted-quad, valid once connected
        uint16_t localPort();

        bool send(const std::string &method, const std::string &uri, const std::vector<std::string> &headers,
                  const std::string &body = std::string());
        // A response ("SIP/2.0 <code> <reason>"); headers are caller-supplied complete
        // lines (e.g. copied Via/From/To/Call-ID/CSeq from the request being answered).
        bool sendResponse(int code, const char *reason, const std::vector<std::string> &headers,
                          const std::string &body = std::string());
        bool sendRaw(const std::string &data);

        // RFC 5626 keepalive ping ("\r\n\r\n"). The matching lone "\r\n" pong (ours or an
        // incoming ping from the panel) is swallowed transparently inside tryRecvFrame();
        // see its comment and the plan's note on why there is no separate pong-timeout.
        bool sendKeepalivePing() { return sendRaw("\r\n\r\n"); }

        // Reads the next complete SIP message. Returns false with outTimedOut=true if
        // nothing arrived within timeoutMs (not an error - just nothing to do yet);
        // false with outTimedOut=false means the connection is gone.
        bool tryRecvFrame(SipFrame &out, uint32_t timeoutMs, bool &outTimedOut);

      private:
        WiFiClientSecure _client;
        std::string _buffer; // bytes read but not yet consumed into a complete frame

        // Fills _buffer with at least one more byte, or returns false on timeout/error.
        bool fillBuffer(uint32_t deadlineMs, bool &outTimedOut);
    };

    // One RFC 2617 SIP Digest Authorization/Proxy-Authorization header line (with the
    // trailing header name and value, ready to append to a request's header list).
    // challenge is the value following "Digest " in a WWW-/Proxy-Authenticate header.
    std::string sipDigestHeader(const std::string &username, const std::string &password, const std::string &method,
                                const std::string &uri, const std::string &challenge, const char *headerName);
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
