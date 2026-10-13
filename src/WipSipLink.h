#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipSipClient.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdint>
#include <string>

// Owns the one persistent SIP/TLS connection to the panel: connect, unregister any
// stale binding, REGISTER, periodic refresh before expiry, a CRLF keepalive, and
// reconnect with backoff on failure. Runs on its own FreeRTOS task, like WipSipPairing;
// never touches a GroupObject. Ring detection (reading incoming INVITEs) is not part of
// this step yet — see the plan's "Ruf-Erkennung" step.
namespace Wis
{
    enum class LinkPhase : uint8_t
    {
        Disconnected = 0,
        Connecting = 1,
        Registering = 2,
        Registered = 3,
    };

    struct LinkSettings
    {
        std::string panelHost;
        uint16_t panelPort = 5061;
        std::string sipUsername;
        std::string sipPassword;
        std::string sipDomain;
    };

    struct LinkState
    {
        uint32_t seq = 0;
        LinkPhase phase = LinkPhase::Disconnected;
        char diag[40] = {};
        char lastError[64] = {};
        uint32_t registeredSinceMs = 0;
        uint32_t minFreeHeapSeen = 0xFFFFFFFFu; // leak-watch, see the plan
        uint32_t reconnectCount = 0;
    };

    class Link
    {
      public:
        void begin(const LinkSettings &settings);

        enum CommandType : uint8_t
        {
            CMD_REREGISTER, // force an immediate reconnect+REGISTER cycle now
        };
        bool enqueue(CommandType type);

        bool snapshot(LinkState &out, uint32_t lastSeq);

      private:
        static void taskEntry(void *arg);
        void run();

        bool connectAndRegister();
        // expiresSeconds=0 sends the safety unregister ("Contact: *") before the real
        // REGISTER; see the plan's note on orphaned bindings.
        bool performRegister(uint32_t expiresSeconds);
        bool waitFinalResponse(SipFrame &out, const std::string &callId, uint32_t cseq, const char *method,
                               uint32_t timeoutMs);
        void teardown(const char *diag);
        void scheduleReconnect();

        void publish();
        void setDiag(const char *diag);
        void fail(const char *diag, const std::string &error);

        struct Command
        {
            CommandType type;
        };

        LinkSettings _settings;
        SipClient _client;

        QueueHandle_t _queue = nullptr;
        SemaphoreHandle_t _mutex = nullptr;
        TaskHandle_t _task = nullptr;
        LinkState _work, _shared;

        // Dialog identity kept stable across REGISTER refreshes (plan: "Call-ID und
        // From-Tag über Refreshes beibehalten").
        std::string _callId;
        std::string _fromTag;
        std::string _instanceUuid; // for the +sip.instance Contact parameter
        uint32_t _cseq = 0;

        uint32_t _refreshAtMs = 0;
        uint32_t _nextKeepaliveAtMs = 0;
        uint32_t _reconnectBackoffStep = 0;
        uint32_t _nextConnectAttemptAtMs = 0;
    };
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
