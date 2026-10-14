#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipXmppClient.h"
#include "OpenKNX.h"
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <cstdlib>
#include <cstring>

namespace Wis
{
    namespace
    {
        const char *const kModuleTag = "WelcomeIPSip";

        // Same PSRAM TLS allocator as WipPortalHttp.cpp — duplicated rather than shared,
        // like the project's Digest-auth code (see the plan); installing it twice is
        // harmless, each call sites checks its own "attempted" flag.
        void *tlsCalloc(size_t count, size_t size)
        {
            void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p == nullptr) p = calloc(count, size);
            return p;
        }

        void tlsFree(void *p)
        {
            free(p);
        }

        void installPsramTlsAllocatorOnce()
        {
            static bool attempted = false;
            if (attempted) return;
            attempted = true;
            if (!psramFound()) return;
            if (mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree) != 0)
                logError(kModuleTag, "TLS allocator: switch to PSRAM refused");
        }

        bool due(uint32_t deadlineMs)
        {
            return (int32_t)(millis() - deadlineMs) >= 0;
        }

        std::string xmlEscape(const std::string &s)
        {
            std::string r;
            r.reserve(s.size());
            for (char c : s)
            {
                switch (c)
                {
                    case '&': r += "&amp;"; break;
                    case '<': r += "&lt;"; break;
                    case '>': r += "&gt;"; break;
                    case '\'': r += "&apos;"; break;
                    case '"': r += "&quot;"; break;
                    default: r += c; break;
                }
            }
            return r;
        }
    } // namespace

    int XmppClient::getByte(uint32_t deadlineMs)
    {
        while (_readPos == _readLen)
        {
            if (due(deadlineMs)) return -1;
            const int avail = _client.available();
            if (avail > 0)
            {
                const int n = _client.read(_readBuf, avail < (int)sizeof(_readBuf) ? avail : sizeof(_readBuf));
                if (n > 0)
                {
                    _readPos = 0;
                    _readLen = (size_t)n;
                    break;
                }
            }
            else if (!_client.connected())
                return -1;
            else
                vTaskDelay(1);
        }
        return _readBuf[_readPos++];
    }

    // Reads one XML tag ("<...>", starting at the next '<'), appending every consumed byte
    // to outXml. Classifies it as closing ("</..."), self-closing ("<... />") or an opening
    // tag; a leading "<?...?>" or "<!...>" is reported via outIsPiOrDecl so the caller can
    // skip it without affecting depth. Honours quoted attribute values so a '>' inside e.g.
    // to='...' does not end the tag early.
    bool XmppClient::readOneTag(uint32_t deadlineMs, std::string &outXml, bool &outIsClose, bool &outIsSelfClose,
                                bool &outIsPiOrDecl)
    {
        outIsClose = outIsSelfClose = outIsPiOrDecl = false;
        int c;
        do
        {
            c = getByte(deadlineMs);
            if (c < 0) return false;
            outXml += (char)c;
        } while (c != '<');

        c = getByte(deadlineMs);
        if (c < 0) return false;
        outXml += (char)c;
        outIsClose = (c == '/');
        outIsPiOrDecl = (c == '?' || c == '!');

        char quote = 0;
        while (true)
        {
            c = getByte(deadlineMs);
            if (c < 0) return false;
            outXml += (char)c;
            if (quote)
            {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'')
            {
                quote = (char)c;
                continue;
            }
            if (c == '>')
            {
                outIsSelfClose = outXml.size() >= 2 && outXml[outXml.size() - 2] == '/';
                return true;
            }
        }
    }

    bool XmppClient::sendRaw(const std::string &data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const size_t n = _client.write((const uint8_t *)data.data() + sent, data.size() - sent);
            if (n == 0) return false;
            sent += n;
        }
        logTrace(kModuleTag, "XMPP >> %s", data.c_str());
        return true;
    }

    bool XmppClient::readElement(const char *tagName, std::string &outXml, uint32_t deadlineMs)
    {
        outXml.clear();
        bool isClose, isSelf, isPi;
        if (!readOneTag(deadlineMs, outXml, isClose, isSelf, isPi)) return false;
        if (isPi)
        {
            outXml.clear();
            if (!readOneTag(deadlineMs, outXml, isClose, isSelf, isPi)) return false;
        }
        if (!isSelf)
        {
            const std::string closeTag = std::string("</") + tagName + ">";
            while (outXml.size() < 8192 &&
                  (outXml.size() < closeTag.size() ||
                   outXml.compare(outXml.size() - closeTag.size(), closeTag.size(), closeTag) != 0))
            {
                const int c = getByte(deadlineMs);
                if (c < 0) return false;
                outXml += (char)c;
            }
        }
        logTrace(kModuleTag, "XMPP << %s", outXml.c_str());
        return true;
    }

    bool XmppClient::readOpeningStreamTag(uint32_t deadlineMs, std::string &outError)
    {
        std::string tag;
        bool isClose, isSelf, isPi;
        if (!readOneTag(deadlineMs, tag, isClose, isSelf, isPi))
        {
            outError = "Stream-Header: Zeitlimit";
            return false;
        }
        if (isPi)
        {
            tag.clear();
            if (!readOneTag(deadlineMs, tag, isClose, isSelf, isPi))
            {
                outError = "Stream-Header: Zeitlimit";
                return false;
            }
        }
        logTrace(kModuleTag, "XMPP << %s", tag.c_str());
        if (isClose || isSelf)
        {
            outError = "unerwarteter Stream-Header";
            return false;
        }
        return true;
    }

    bool XmppClient::sendStreamOpen(const std::string &xmppDomain, uint32_t deadlineMs, std::string &outError)
    {
        const std::string open = "<?xml version=\"1.0\"?><stream:stream to=\"" + xmppDomain +
                                 "\" xml:lang=\"en\" version=\"1.0\" xmlns=\"jabber:client\" "
                                 "xmlns:stream=\"http://etherx.jabber.org/streams\">";
        if (!sendRaw(open))
        {
            outError = "Stream-Oeffnen fehlgeschlagen";
            return false;
        }
        return readOpeningStreamTag(deadlineMs, outError);
    }

    std::string XmppClient::nextId()
    {
        return "wis" + std::to_string(++_idCounter);
    }

    bool XmppClient::connectAndAuthenticate(const std::string &host, uint16_t port, const std::string &xmppDomain,
                                            const std::string &certPem, const std::string &keyPem, uint32_t timeoutMs,
                                            std::string &outError)
    {
        installPsramTlsAllocatorOnce();

        _client.stop();
        _readPos = _readLen = 0; // discard any bytes buffered from a previous connection
        _client.setInsecure(); // server cert not pinned, like the portal/panel connections elsewhere in this module
        _client.setHandshakeTimeout(timeoutMs / 1000);
        if (!certPem.empty() && !keyPem.empty())
        {
            _client.setCertificate(certPem.c_str());
            _client.setPrivateKey(keyPem.c_str());
        }
        _client.setPlainStart(); // postpone the TLS handshake until startTLS() — see the header

        if (!_client.connect(host.c_str(), port, (int32_t)timeoutMs))
        {
            outError = "TCP-Verbindung fehlgeschlagen: " + host;
            return false;
        }

        const uint32_t deadline = millis() + timeoutMs;

        // 1) plaintext stream, STARTTLS negotiation
        if (!sendStreamOpen(xmppDomain, deadline, outError)) return false;
        std::string scratch;
        if (!readElement("stream:features", scratch, deadline))
        {
            outError = "Features (vor STARTTLS): Zeitlimit";
            return false;
        }
        if (scratch.find("<starttls") == std::string::npos)
        {
            outError = "Server bietet kein STARTTLS an";
            return false;
        }
        if (!sendRaw("<starttls xmlns=\"urn:ietf:params:xml:ns:xmpp-tls\"/>"))
        {
            outError = "STARTTLS senden fehlgeschlagen";
            return false;
        }
        // <proceed/> and <failure> are both answered by just their own opening tag — the
        // former always self-closes, the latter we do not need to drain correctly since
        // any failure here means the whole connection is torn down right away.
        bool isClose, isSelf, isPi;
        scratch.clear();
        if (!readOneTag(deadline, scratch, isClose, isSelf, isPi))
        {
            outError = "STARTTLS-Antwort: Zeitlimit";
            return false;
        }
        if (scratch.find("<proceed") == std::string::npos)
        {
            outError = "STARTTLS abgelehnt: " + scratch;
            return false;
        }

        // 2) in-place TLS upgrade, now with the client certificate (mTLS)
        if (!_client.startTLS())
        {
            outError = "TLS-Handshake fehlgeschlagen";
            return false;
        }

        // 3) fresh stream per RFC 6120, SASL EXTERNAL (identity comes from the client cert)
        if (!sendStreamOpen(xmppDomain, deadline, outError)) return false;
        if (!readElement("stream:features", scratch, deadline))
        {
            outError = "Features (nach TLS): Zeitlimit";
            return false;
        }
        if (!sendRaw("<auth xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\" mechanism=\"EXTERNAL\">=</auth>"))
        {
            outError = "SASL senden fehlgeschlagen";
            return false;
        }
        scratch.clear();
        if (!readOneTag(deadline, scratch, isClose, isSelf, isPi))
        {
            outError = "SASL-Antwort: Zeitlimit";
            return false;
        }
        if (scratch.find("<success") == std::string::npos)
        {
            outError = "SASL EXTERNAL abgelehnt: " + scratch;
            return false;
        }

        // 4) fresh stream again per RFC 6120, then bind + session
        if (!sendStreamOpen(xmppDomain, deadline, outError)) return false;
        if (!readElement("stream:features", scratch, deadline))
        {
            outError = "Features (nach SASL): Zeitlimit";
            return false;
        }
        if (!sendRaw("<iq type=\"set\" id=\"bind1\"><bind xmlns=\"urn:ietf:params:xml:ns:xmpp-bind\"/></iq>"))
        {
            outError = "Bind senden fehlgeschlagen";
            return false;
        }
        if (!readElement("iq", scratch, deadline))
        {
            outError = "Bind-Antwort: Zeitlimit";
            return false;
        }
        if (scratch.find("type=\"error\"") != std::string::npos || scratch.find("type='error'") != std::string::npos)
        {
            outError = "Bind abgelehnt: " + scratch;
            return false;
        }
        if (!sendRaw("<iq type=\"set\" id=\"sess1\"><session xmlns=\"urn:ietf:params:xml:ns:xmpp-session\"/></iq>"))
        {
            outError = "Session senden fehlgeschlagen";
            return false;
        }
        if (!readElement("iq", scratch, deadline))
        {
            outError = "Session-Antwort: Zeitlimit";
            return false;
        }
        if (scratch.find("type=\"error\"") != std::string::npos || scratch.find("type='error'") != std::string::npos)
        {
            outError = "Session abgelehnt: " + scratch;
            return false;
        }
        return true;
    }

    XmppRpcResult XmppClient::sendUnlock(const std::string &to, const std::string &contactId, uint32_t timeoutMs)
    {
        XmppRpcResult result;
        const std::string id = nextId();
        const std::string req = "<iq type=\"set\" id=\"" + id + "\" to=\"" + xmlEscape(to) + "\">"
                                "<query xmlns=\"jabber:iq:rpc\">"
                                "<methodCall>"
                                "<methodName>RemoteInterface.unlockCloudContact</methodName>"
                                "<params><param><value><string>" + xmlEscape(contactId) + "</string></value></param></params>"
                                "</methodCall>"
                                "</query>"
                                "</iq>";
        if (!sendRaw(req))
        {
            result.error = "RPC senden fehlgeschlagen";
            return result;
        }

        std::string resp;
        if (!readElement("iq", resp, millis() + timeoutMs))
        {
            result.error = "RPC-Antwort: Zeitlimit";
            return result;
        }

        if (resp.find("type=\"error\"") != std::string::npos || resp.find("type='error'") != std::string::npos)
        {
            result.error = "Server meldet IQ-Fehler: " + resp;
            return result;
        }

        const size_t faultPos = resp.find("<fault>");
        if (faultPos != std::string::npos)
        {
            const size_t strStart = resp.find("<string>", faultPos);
            if (strStart != std::string::npos)
            {
                const size_t strEnd = resp.find("</string>", strStart);
                result.faultString = resp.substr(strStart + 8, strEnd == std::string::npos ? std::string::npos : strEnd - (strStart + 8));
            }
            result.error = "Jabber-RPC fault" + (result.faultString.empty() ? std::string() : ": " + result.faultString);
            return result;
        }

        const size_t namePos = resp.find("<name>resultCode</name>");
        if (namePos != std::string::npos)
        {
            const size_t intPos = resp.find("<int>", namePos);
            if (intPos != std::string::npos) result.resultCode = atoi(resp.c_str() + intPos + 5);
        }
        result.ok = true;
        return result;
    }

    void XmppClient::close()
    {
        _client.stop();
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
