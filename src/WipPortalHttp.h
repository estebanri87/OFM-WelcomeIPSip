#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include <string>
#include <vector>

namespace Wis
{
    struct HttpResponse
    {
        int status = 0; // < 0: transport error, see error
        std::string body;
        std::string wwwAuthenticate; // for HTTP Digest (401/407)
        std::string error;

        bool ok() const { return status >= 200 && status < 300; }
    };

    // Minimal blocking HTTPS/1.1 client on WiFiClientSecure, adapted from
    // OFM-Vaillant/src/VaillantHttp.{h,cpp} with mutual-TLS support added: the portal's
    // /event endpoint requires the client certificate issued during pairing. Only for
    // the pairing worker task.
    class PortalHttp
    {
      public:
        // clientCertPem/clientKeyPem: pass both for mTLS (discover/connect/acl-poll),
        // leave both empty for plain TLS (certificate/request, GEO lookup).
        static HttpResponse request(const char *method, const std::string &url,
                                    const std::vector<std::string> &headers,
                                    const std::string &body = std::string(),
                                    const std::string &clientCertPem = std::string(),
                                    const std::string &clientKeyPem = std::string(),
                                    uint32_t timeoutMs = 30000);
    };

    // One RFC 2617 HTTP Digest request/response round trip: sends the request without
    // auth, and on a 401 with a WWW-Authenticate: Digest challenge, recomputes and
    // resends once with the Authorization header. Returns the final response (the 401
    // itself if the challenge could not be parsed, or the second response otherwise).
    HttpResponse digestRequest(const char *method, const std::string &url, const std::string &username,
                               const std::string &password, const std::string &body, uint32_t timeoutMs = 30000);
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
