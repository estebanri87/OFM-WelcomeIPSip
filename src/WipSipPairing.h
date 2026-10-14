#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipSipStore.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Wis
{
    // The two client-type strings accepted by the portal. "FreeAtHomeNext" is the
    // user's preferred portal entry label but is *unverified*: if the gateway never
    // reacts to it (ACL-poll times out), the help text and diagnosis point back at
    // "Welcome", which is confirmed working. Switching is never automatic (see plan).
    enum class ClientTypeChoice : uint8_t
    {
        FreeAtHomeNext = 0,
        Welcome = 1,
    };

    struct DoorInfo
    {
        std::string name;
        std::string stationId;
        std::string address;
        std::string secondUnlockAddress; // empty if this station has no second lock/gate
    };

    struct PairingSettings
    {
        std::string username;
        std::string password;
        std::string clientName;
        ClientTypeChoice clientType = ClientTypeChoice::FreeAtHomeNext;
        // Usually empty: the gateway_uuid is normally found automatically via the
        // portal's discovery event. Set only when that discovery is empty or ambiguous
        // (e.g. several gateways on one account) — then it replaces that lookup.
        std::string gatewayUuidOverride;
    };

    // Lightweight, frequently-read status — the KO/console/web-UI facing view. The full
    // persisted record (certificate, keys, SIP credentials) is read directly from the
    // Store on the console/web thread instead, since that only happens for explicit,
    // infrequent commands ("wis doors", "wis code") and must never appear in logs.
    struct PairingState
    {
        uint32_t seq = 0;
        Phase phase = Phase::NONE;
        char diag[40] = {};
        char lastError[64] = {};
        char integrityDisplay[12] = {}; // "NNNN HHHH"
        char gatewayName[48] = {};
        uint8_t doorCount = 0;
        bool configStale = false; // set by the module when ETS parameters no longer match
    };

    // Owns the one-time cloud pairing against the MyBuildings portal: RSA keypair/CSR,
    // HTTP-Digest certificate request, gateway discovery, the connect event, and the
    // ACL-update poll that yields the SIP password, SIP domain and door list. Runs on
    // its own FreeRTOS task — RSA keygen alone can take 30-120s — and never touches a
    // GroupObject; the module reads PairingState via snapshot() and the door/SIP data
    // via Store::load() to build KOs, console output and the web page.
    class Pairing
    {
      public:
        enum CommandType : uint8_t
        {
            CMD_PAIR,
            CMD_UNPAIR,
        };

        // Starts the worker task. If a prior pairing completed with different settings
        // (user, client type or gateway override changed in the ETS), the status is
        // marked "stale" instead of ever re-pairing automatically — the user must run
        // "wis pair" (or the KO) deliberately after reviewing why it changed.
        void begin(const PairingSettings &settings);

        bool enqueue(CommandType type);
        bool snapshot(PairingState &out, uint32_t lastSeq);

        // True once SIP credentials are available (ACL_RECEIVED or PAIRED) — convenience
        // for the module's createChannel()/showHelp() paths that must not block on the
        // store.
        bool hasSipCredentials();

        Store &store() { return _store; } // console/web read access to the full record

      private:
        struct Command
        {
            CommandType type;
        };

        static void taskEntry(void *arg);
        void run();

        void publish();
        void setDiag(const char *diag);
        void fail(const char *diag, const std::string &error);

        void onPairCommand();
        bool stepRequestCertificate();
        bool stepDiscoverGateway();
        bool stepSendConnect();
        bool stepPollAclOnce();
        bool parseAndSaveAclUpdate(const std::string &payload);
        void finalizePairing();
        void doUnpair();

        std::string portalUrl();
        std::string clientTypeString() const;
        std::string computeConfigHash() const;

        PairingSettings _settings;
        PersistedPairing _persisted;
        Store _store;

        PairingState _work, _shared;
        QueueHandle_t _queue = nullptr;
        SemaphoreHandle_t _mutex = nullptr;
        TaskHandle_t _task = nullptr;

        uint32_t _nextAclPollAt = 0;
        uint32_t _aclBackoffStep = 0;
        uint32_t _aclDeadlineAt = 0;
    };
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
