#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipSipClient.h"
#include "WipCrypto.h"
#include "OpenKNX.h"
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <mbedtls/platform.h>
#include <cctype>
#include <cstring>

namespace Wis
{
    namespace
    {
        const char *const kModuleTag = "WelcomeIPSip";

        // Same PSRAM TLS allocator as WipPortalHttp — a long-lived persistent SIP
        // connection needs it just as much as the short-lived portal requests.
        void *tlsCalloc(size_t count, size_t size)
        {
            void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p == nullptr) p = calloc(count, size);
            return p;
        }
        void tlsFree(void *p) { free(p); }
        void installPsramTlsAllocatorOnce()
        {
            static bool attempted = false;
            if (attempted) return;
            attempted = true;
            if (!psramFound()) return;
            if (mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree) != 0)
                logError(kModuleTag, "TLS allocator: switch to PSRAM refused");
        }

        bool startsWithNoCase(const std::string &s, const char *prefix)
        {
            return strncasecmp(s.c_str(), prefix, strlen(prefix)) == 0;
        }

        std::string trimmed(const std::string &s)
        {
            size_t start = 0, end = s.size();
            while (start < end && isspace((unsigned char)s[start])) start++;
            while (end > start && isspace((unsigned char)s[end - 1])) end--;
            return s.substr(start, end - start);
        }

        std::string challengeValue(const std::string &challenge, const char *key)
        {
            const std::string needle = std::string(key) + "=";
            size_t pos = challenge.find(needle);
            while (pos != std::string::npos)
            {
                if (pos == 0 || challenge[pos - 1] == ' ' || challenge[pos - 1] == ',') break;
                pos = challenge.find(needle, pos + 1);
            }
            if (pos == std::string::npos) return std::string();
            size_t valueStart = pos + needle.size();
            if (valueStart < challenge.size() && challenge[valueStart] == '"')
            {
                const size_t end = challenge.find('"', valueStart + 1);
                if (end == std::string::npos) return std::string();
                return challenge.substr(valueStart + 1, end - valueStart - 1);
            }
            size_t end = challenge.find(',', valueStart);
            if (end == std::string::npos) end = challenge.size();
            return trimmed(challenge.substr(valueStart, end - valueStart));
        }

        std::string randomHex(size_t bytes)
        {
            std::vector<unsigned char> buf(bytes);
            esp_fill_random(buf.data(), buf.size());
            static const char hex[] = "0123456789abcdef";
            std::string out;
            out.reserve(bytes * 2);
            for (unsigned char b : buf)
            {
                out += hex[b >> 4];
                out += hex[b & 0x0F];
            }
            return out;
        }
    } // namespace

    std::string SipFrame::method() const
    {
        if (isResponse() || startLine.empty()) return std::string();
        const size_t sp = startLine.find(' ');
        return sp == std::string::npos ? startLine : startLine.substr(0, sp);
    }

    int SipFrame::statusCode() const
    {
        if (!isResponse()) return -1;
        const size_t firstSpace = startLine.find(' ');
        if (firstSpace == std::string::npos) return -1;
        return atoi(startLine.c_str() + firstSpace + 1);
    }

    std::string SipFrame::header(const char *name) const
    {
        for (const auto &h : headers)
            if (strcasecmp(h.first.c_str(), name) == 0) return h.second;
        return std::string();
    }

    bool SipClient::connect(const std::string &host, uint16_t port, uint32_t timeoutMs, std::string &outError)
    {
        installPsramTlsAllocatorOnce();
        _buffer.clear();

        // The panel's certificate is self-signed (see the plan); only the SIP Digest
        // credentials authenticate us, same trust model as the Python reference.
        _client.setInsecure();
        _client.setHandshakeTimeout(timeoutMs / 1000);
        if (!_client.connect(host.c_str(), port, (int32_t)timeoutMs))
        {
            outError = "TLS-Verbindung zu " + host + ":" + std::to_string(port) + " fehlgeschlagen";
            return false;
        }

        // SO_KEEPALIVE as a second line of defence under the application-level CRLF
        // ping (see the plan) - available via NetworkClient::setSocketOption() on this
        // ESP32-Arduino core's socket wrapper.
        int one = 1;
        _client.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        return true;
    }

    void SipClient::close()
    {
        _client.stop();
        _buffer.clear();
    }

    bool SipClient::connected()
    {
        return _client.connected();
    }

    std::string SipClient::localIp()
    {
        return _client.localIP().toString().c_str();
    }

    uint16_t SipClient::localPort()
    {
        return _client.localPort();
    }

    bool SipClient::sendRaw(const std::string &data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const size_t n = _client.write((const uint8_t *)data.data() + sent, data.size() - sent);
            if (n == 0) return false;
            sent += n;
        }
        return true;
    }

    bool SipClient::send(const std::string &method, const std::string &uri, const std::vector<std::string> &headers,
                         const std::string &body)
    {
        std::string req;
        req.reserve(256 + body.size());
        req += method;
        req += ' ';
        req += uri;
        req += " SIP/2.0\r\n";
        for (const auto &h : headers)
        {
            req += h;
            req += "\r\n";
        }
        req += "Content-Length: ";
        req += std::to_string(body.size());
        req += "\r\n\r\n";
        req += body;
        return sendRaw(req);
    }

    bool SipClient::sendResponse(int code, const char *reason, const std::vector<std::string> &headers,
                                 const std::string &body)
    {
        std::string resp;
        resp.reserve(256 + body.size());
        resp += "SIP/2.0 ";
        resp += std::to_string(code);
        resp += ' ';
        resp += reason;
        resp += "\r\n";
        for (const auto &h : headers)
        {
            resp += h;
            resp += "\r\n";
        }
        resp += "Content-Length: ";
        resp += std::to_string(body.size());
        resp += "\r\n\r\n";
        resp += body;
        return sendRaw(resp);
    }

    bool SipClient::fillBuffer(uint32_t deadlineMs, bool &outTimedOut)
    {
        outTimedOut = false;
        uint8_t chunk[1024];
        while (true)
        {
            const int avail = _client.available();
            if (avail > 0)
            {
                const int n = _client.read(chunk, avail < (int)sizeof(chunk) ? avail : sizeof(chunk));
                if (n <= 0) return false;
                _buffer.append((char *)chunk, n);
                return true;
            }
            if (!_client.connected()) return false;
            if ((int32_t)(millis() - deadlineMs) >= 0)
            {
                outTimedOut = true;
                return false;
            }
            vTaskDelay(1);
        }
    }

    bool SipClient::tryRecvFrame(SipFrame &out, uint32_t timeoutMs, bool &outTimedOut)
    {
        const uint32_t deadline = millis() + timeoutMs;
        outTimedOut = false;

        // Swallow leading bare CRLF "noise": our own pong for a ping we sent, or a
        // ping the panel sent us (we don't answer a panel-initiated ping explicitly —
        // see the plan's note on why there is no strict pong-timeout state machine here).
        while (_buffer.size() >= 2 && _buffer[0] == '\r' && _buffer[1] == '\n')
            _buffer.erase(0, 2);

        while (_buffer.find("\r\n\r\n") == std::string::npos)
        {
            bool timedOut = false;
            if (!fillBuffer(deadline, timedOut))
            {
                outTimedOut = timedOut;
                return false;
            }
            while (_buffer.size() >= 2 && _buffer[0] == '\r' && _buffer[1] == '\n')
                _buffer.erase(0, 2);
        }

        const size_t sep = _buffer.find("\r\n\r\n");
        const std::string headerBlock = _buffer.substr(0, sep);

        long contentLength = 0;
        {
            const char *cl = strcasestr(headerBlock.c_str(), "content-length:");
            if (cl != nullptr) contentLength = atol(cl + strlen("content-length:"));
        }

        while (_buffer.size() < sep + 4 + (size_t)contentLength)
        {
            bool timedOut = false;
            if (!fillBuffer(deadline, timedOut))
            {
                outTimedOut = timedOut;
                return false;
            }
        }

        out.headers.clear();
        out.body = _buffer.substr(sep + 4, (size_t)contentLength);

        size_t lineStart = 0;
        bool first = true;
        while (lineStart < headerBlock.size())
        {
            size_t lineEnd = headerBlock.find("\r\n", lineStart);
            if (lineEnd == std::string::npos) lineEnd = headerBlock.size();
            const std::string line = headerBlock.substr(lineStart, lineEnd - lineStart);
            if (first)
            {
                out.startLine = line;
                first = false;
            }
            else
            {
                const size_t colon = line.find(':');
                if (colon != std::string::npos) out.headers.emplace_back(trimmed(line.substr(0, colon)), trimmed(line.substr(colon + 1)));
            }
            lineStart = lineEnd + 2;
        }

        _buffer.erase(0, sep + 4 + (size_t)contentLength);
        return true;
    }

    std::string sipDigestHeader(const std::string &username, const std::string &password, const std::string &method,
                                const std::string &uri, const std::string &challenge, const char *headerName)
    {
        const std::string realm = challengeValue(challenge, "realm");
        const std::string nonce = challengeValue(challenge, "nonce");
        const std::string opaque = challengeValue(challenge, "opaque");
        std::string qop = challengeValue(challenge, "qop");
        qop = qop.find("auth") != std::string::npos ? "auth" : "";

        const std::string ha1 = md5Hex(username + ":" + realm + ":" + password);
        const std::string ha2 = md5Hex(method + ":" + uri);
        const std::string cnonce = randomHex(4);
        const char *nc = "00000001";

        const std::string response = qop.empty() ? md5Hex(ha1 + ":" + nonce + ":" + ha2)
                                                  : md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);

        std::string h = std::string(headerName) + ": Digest username=\"" + username + "\", realm=\"" + realm +
                        "\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" + response + "\", algorithm=MD5";
        if (!qop.empty()) h += ", cnonce=\"" + cnonce + "\", qop=" + qop + ", nc=" + std::string(nc);
        if (!opaque.empty()) h += ", opaque=\"" + opaque + "\"";
        return h;
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
