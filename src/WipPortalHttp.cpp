#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipPortalHttp.h"
#include "WipCrypto.h"
#include "OpenKNX.h"
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/platform.h>

namespace Wis
{
    namespace
    {
        // A TLS session needs about 40 KB; routed to PSRAM like OFM-Vaillant's HTTP
        // client (VaillantHttp.cpp) and OFM-HueGatewayModule so the KNX stack, MQTT and
        // the web server keep their internal RAM. Installing it twice (once per module)
        // is harmless.
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
                logError("WelcomeIPSip", "TLS allocator: switch to PSRAM refused");
        }

        class Reader
        {
          public:
            Reader(WiFiClientSecure &client, uint32_t timeoutMs)
                : _client(client), _start(millis()), _timeoutMs(timeoutMs) {}

            int get()
            {
                while (_pos == _len)
                {
                    if (millis() - _start > _timeoutMs) return -1;
                    const int avail = _client.available();
                    if (avail > 0)
                    {
                        const int n = _client.read(_buf, avail < (int)sizeof(_buf) ? avail : sizeof(_buf));
                        if (n > 0)
                        {
                            _pos = 0;
                            _len = (size_t)n;
                            break;
                        }
                    }
                    else if (!_client.connected())
                        return -1;
                    else
                        vTaskDelay(1);
                }
                return _buf[_pos++];
            }

            bool line(std::string &out, size_t maxLen = 8192)
            {
                out.clear();
                while (true)
                {
                    const int c = get();
                    if (c < 0) return false;
                    if (c == '\n')
                    {
                        if (!out.empty() && out.back() == '\r') out.pop_back();
                        return true;
                    }
                    if (out.size() >= maxLen) return false;
                    out += (char)c;
                }
            }

            bool timedOut() const { return millis() - _start > _timeoutMs; }

          private:
            WiFiClientSecure &_client;
            uint32_t _start;
            uint32_t _timeoutMs;
            uint8_t _buf[1024];
            size_t _pos = 0;
            size_t _len = 0;
        };

        bool startsWithNoCase(const std::string &s, const char *prefix)
        {
            return strncasecmp(s.c_str(), prefix, strlen(prefix)) == 0;
        }

        std::string trimmed(const std::string &s, size_t from)
        {
            while (from < s.size() && (s[from] == ' ' || s[from] == '\t')) from++;
            return s.substr(from);
        }

        void splitUrl(const std::string &url, std::string &host, std::string &path)
        {
            const size_t hostStart = 8; // "https://"
            size_t pathStart = url.find('/', hostStart);
            host = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
            path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
        }
    } // namespace

    HttpResponse PortalHttp::request(const char *method, const std::string &url, const std::vector<std::string> &headers,
                                     const std::string &body, const std::string &clientCertPem,
                                     const std::string &clientKeyPem, uint32_t timeoutMs)
    {
        HttpResponse res;

        if (url.rfind("https://", 0) != 0)
        {
            res.status = -1;
            res.error = "only https URLs";
            return res;
        }
        std::string host, path;
        splitUrl(url, host, path);

        installPsramTlsAllocatorOnce();

        WiFiClientSecure client;
        client.setInsecure(); // portal and panel certs are not pinned; the client cert is what authenticates us
        client.setHandshakeTimeout(timeoutMs / 1000);
        if (!clientCertPem.empty() && !clientKeyPem.empty())
        {
            client.setCertificate(clientCertPem.c_str());
            client.setPrivateKey(clientKeyPem.c_str());
        }

        if (!client.connect(host.c_str(), 443, (int32_t)timeoutMs))
        {
            res.status = -1;
            res.error = "connect failed: " + host;
            return res;
        }

        std::string req;
        req.reserve(512 + body.size());
        req += method;
        req += ' ';
        req += path;
        req += " HTTP/1.1\r\nHost: ";
        req += host;
        req += "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
        for (const auto &h : headers)
        {
            req += h;
            req += "\r\n";
        }
        const bool needsLength = !body.empty() || strcmp(method, "GET") != 0;
        if (needsLength)
        {
            req += "Content-Length: ";
            req += std::to_string(body.size());
            req += "\r\n";
        }
        req += "\r\n";
        req += body;

        size_t sent = 0;
        while (sent < req.size())
        {
            const size_t n = client.write((const uint8_t *)req.data() + sent, req.size() - sent);
            if (n == 0)
            {
                client.stop();
                res.status = -1;
                res.error = "write failed";
                return res;
            }
            sent += n;
        }

        Reader in(client, timeoutMs);
        std::string line;

        if (!in.line(line) || line.rfind("HTTP/", 0) != 0)
        {
            client.stop();
            res.status = -1;
            res.error = in.timedOut() ? "timeout" : "no status line";
            return res;
        }
        const size_t sp = line.find(' ');
        res.status = sp == std::string::npos ? -1 : atoi(line.c_str() + sp + 1);

        long contentLength = -1;
        bool chunked = false;
        while (true)
        {
            if (!in.line(line))
            {
                client.stop();
                res.status = -1;
                res.error = "header read failed";
                return res;
            }
            if (line.empty()) break;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            if (startsWithNoCase(line, "content-length:"))
                contentLength = atol(line.c_str() + colon + 1);
            else if (startsWithNoCase(line, "transfer-encoding:"))
                chunked = strcasestr(line.c_str() + colon + 1, "chunked") != nullptr;
            else if (startsWithNoCase(line, "www-authenticate:"))
                res.wwwAuthenticate = trimmed(line, colon + 1);
        }

        constexpr size_t kMaxBody = 32 * 1024;
        auto appendByte = [&](int c) -> bool {
            if (res.body.size() >= kMaxBody) return false;
            res.body += (char)c;
            return true;
        };

        bool bodyOk = true;
        if (strcmp(method, "HEAD") == 0 || res.status == 204 || res.status == 304)
        {
            // no body
        }
        else if (chunked)
        {
            while (bodyOk)
            {
                if (!in.line(line))
                {
                    bodyOk = false;
                    break;
                }
                const long size = strtol(line.c_str(), nullptr, 16);
                if (size <= 0) break;
                if (res.body.size() + (size_t)size > kMaxBody)
                {
                    bodyOk = false;
                    break;
                }
                res.body.reserve(res.body.size() + size);
                for (long i = 0; i < size && bodyOk; i++)
                {
                    const int c = in.get();
                    bodyOk = c >= 0 && appendByte(c);
                }
                in.line(line);
            }
        }
        else if (contentLength >= 0)
        {
            if ((size_t)contentLength > kMaxBody)
                bodyOk = false;
            else
            {
                res.body.reserve(contentLength);
                for (long i = 0; i < contentLength && bodyOk; i++)
                {
                    const int c = in.get();
                    bodyOk = c >= 0 && appendByte(c);
                }
            }
        }
        else
        {
            int c;
            while ((c = in.get()) >= 0)
                if (!appendByte(c))
                {
                    bodyOk = false;
                    break;
                }
        }

        client.stop();
        if (!bodyOk)
        {
            res.error = res.body.size() >= kMaxBody ? "body too large" : "body read failed";
            res.status = -2;
        }
        return res;
    }

    namespace
    {
        // Returns the value for `key` in a WWW-Authenticate challenge, honouring
        // quoted-string values; empty when the key is absent.
        std::string challengeValue(const std::string &challenge, const char *key)
        {
            const std::string needle = std::string(key) + "=";
            size_t pos = challenge.find(needle);
            while (pos != std::string::npos)
            {
                // avoid matching e.g. "nonce=" inside a hypothetical "nextnonce=..." field
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
            return challenge.substr(valueStart, end - valueStart);
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

    HttpResponse digestRequest(const char *method, const std::string &url, const std::string &username,
                               const std::string &password, const std::string &body, uint32_t timeoutMs)
    {
        std::string host, path;
        splitUrl(url, host, path);

        HttpResponse first = PortalHttp::request(method, url, {"Content-Type: application/json"}, body,
                                                 std::string(), std::string(), timeoutMs);
        if (first.status != 401 || first.wwwAuthenticate.empty()) return first;
        if (!startsWithNoCase(first.wwwAuthenticate, "digest")) return first;

        const std::string realm = challengeValue(first.wwwAuthenticate, "realm");
        const std::string nonce = challengeValue(first.wwwAuthenticate, "nonce");
        const std::string opaque = challengeValue(first.wwwAuthenticate, "opaque");
        std::string qop = challengeValue(first.wwwAuthenticate, "qop");
        if (realm.empty() || nonce.empty()) return first; // not a challenge we can answer

        // One of possibly several comma-separated qop values; "auth" is all we support.
        qop = qop.find("auth") != std::string::npos ? "auth" : "";

        const std::string ha1 = md5Hex(username + ":" + realm + ":" + password);
        const std::string ha2 = md5Hex(std::string(method) + ":" + path);
        const std::string cnonce = randomHex(8);
        const char *nc = "00000001";

        const std::string response = qop.empty()
            ? md5Hex(ha1 + ":" + nonce + ":" + ha2)
            : md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);

        std::string authHeader = "Authorization: Digest username=\"" + username + "\", realm=\"" + realm +
                                 "\", nonce=\"" + nonce + "\", uri=\"" + path + "\", response=\"" + response + "\"";
        if (!qop.empty()) authHeader += ", qop=" + qop + ", nc=" + std::string(nc) + ", cnonce=\"" + cnonce + "\"";
        if (!opaque.empty()) authHeader += ", opaque=\"" + opaque + "\"";

        return PortalHttp::request(method, url, {"Content-Type: application/json", authHeader}, body, std::string(),
                                   std::string(), timeoutMs);
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
