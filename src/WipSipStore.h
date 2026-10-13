#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include <cstdint>
#include <string>

namespace Wis
{
    // Persisted pairing state in ESP32 Preferences/NVS. Every field here survives ETS
    // download, OTA and power loss, so a half-finished pairing resumes instead of
    // requesting a second certificate (the portal has no way to revoke the first one).
    //
    // Fields are stored as plain text via Preferences::putString; a 2048-bit RSA private
    // key PEM and a signed cert PEM each stay well under the ~4000-byte practical limit
    // of that call (unlike the DER + putBytes approach used for raw binary elsewhere in
    // this project), so no extra PEM<->DER conversion is needed here.
    enum class Phase : uint8_t
    {
        NONE = 0,         // nothing persisted yet
        KEY = 1,           // keypair generated
        CERT = 2,          // certificate issued by the portal
        DISCOVERED = 3,    // gateway_uuid known
        CONNECT_SENT = 4,  // welcome.connect sent, integrity code shown to the user
        ACL_RECEIVED = 5,  // SIP credentials + door list decrypted
        PAIRED = 6,        // confirmed complete; config hash stored for staleness checks
    };

    struct PersistedPairing
    {
        Phase phase = Phase::NONE;
        std::string configHash; // md5 of (user|portalUuid|clientType), detects a changed ETS config

        std::string portalUrl;
        std::string clientName;

        std::string privateKeyPem;
        std::string certPem;

        std::string fingerprintSha1; // 40 hex chars, uppercase
        std::string gruu;
        std::string sipUsername;
        std::string ownPortalUuid;

        std::string gatewayUuid;
        std::string gatewayName;

        std::string integrityEight;  // 8 chars, sent to the portal
        std::string integrityDisplay; // "NNNN HHHH", shown to the user

        std::string sipPassword; // decrypted, only valid from ACL_RECEIVED on
        std::string sipDomain;
        std::string doorsJson; // ArduinoJson array, see WipSipPairing
    };

    // Thin wrapper around ESP32 Preferences. Every save*() call is a separate NVS
    // commit so a crash between two calls still leaves a consistent, resumable state —
    // callers must persist a phase only once everything that phase implies is stored.
    class Store
    {
      public:
        bool load(PersistedPairing &out);

        void saveKey(const std::string &privateKeyPem);
        void saveCert(const std::string &certPem, const std::string &portalUrl, const std::string &clientName);
        void saveDiscovered(const std::string &fingerprintSha1, const std::string &gruu, const std::string &sipUsername,
                             const std::string &ownPortalUuid, const std::string &gatewayUuid, const std::string &gatewayName);
        void saveConnectSent(const std::string &integrityEight, const std::string &integrityDisplay);
        void saveAclReceived(const std::string &sipPassword, const std::string &sipDomain, const std::string &doorsJson);
        void savePaired(const std::string &configHash);

        // Clears every key ("wis unpair"). Does not contact the portal — there is no
        // known delete endpoint; the user must remove the entry there manually. Does
        // NOT clear the certificate-request guard below — unpairing must not reset it.
        void clear();

        // At most one certificate request per 24h, even across "wis unpair" + "wis pair" —
        // stored in a separate NVS namespace so clear() cannot reset it.
        uint32_t lastCertRequestEpoch();
        void markCertRequested(uint32_t nowEpoch);
    };
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
