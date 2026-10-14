#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipXmppClient.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdint>
#include <string>

// Cloud XMPP/Jabber-RPC door-unlock worker. Unlike WipSipLink (one persistent local
// connection), this connects on demand: a "Türöffner"/"Zweitschloss" KO, console command
// or web link enqueues one CMD_UNLOCK, the task opens a short-lived mTLS XMPP session
// (WipXmppClient), sends RemoteInterface.unlockCloudContact and waits for the response,
// then goes back to Idle and closes the connection. Never touches a GroupObject — the
// module reads XmppLinkState via snapshot(). See the plan's "Türöffner (Schritt 8)".
namespace Wis
{
    enum class XmppLinkPhase : uint8_t
    {
        Idle = 0,
        Connecting = 1,
        Unlocking = 2,
        Done = 3,
        Error = 4,
    };

    struct XmppLinkSettings
    {
        std::string xmppHost; // SRV-resolved connect target (e.g. srv1.xmpp.eu.mybuildings.abb.com)
        uint16_t xmppPort = 5222;
        std::string xmppDomain; // logical XMPP service domain for the stream and JIDs (e.g. xmpp.my.busch-jaeger.de)
        std::string certPem;
        std::string privateKeyPem;
        // Local part of the unlock IQ's "to" JID — working hypothesis, see the plan:
        // "to"-Adressierung is unconfirmed until tested against the real gateway.
        std::string gatewayUuid;
    };

    struct XmppLinkState
    {
        uint32_t seq = 0;
        XmppLinkPhase phase = XmppLinkPhase::Idle;
        char diag[40] = {};
        char lastError[64] = {};
        int lastResultCode = -1;
        char lastStationId[24] = {};
        uint32_t lastFinishedAtMs = 0;
    };

    class XmppLink
    {
      public:
        void begin(const XmppLinkSettings &settings);

        enum CommandType : uint8_t
        {
            CMD_UNLOCK,
        };
        // contactId: DoorInfo::address (lockIndex=1) or ::secondUnlockAddress (lockIndex=2).
        // stationId is only carried along for the diagnosis/state display.
        bool enqueue(CommandType type, const std::string &contactId, const std::string &stationId, uint8_t lockIndex);

        bool snapshot(XmppLinkState &out, uint32_t lastSeq);

      private:
        static void taskEntry(void *arg);
        void run();

        void publish();
        void setDiag(const char *diag);

        struct Command
        {
            CommandType type;
            char contactId[64];
            char stationId[24];
            uint8_t lockIndex;
        };

        XmppLinkSettings _settings;
        XmppClient _client;

        QueueHandle_t _queue = nullptr;
        SemaphoreHandle_t _mutex = nullptr;
        TaskHandle_t _task = nullptr;
        XmppLinkState _work, _shared;
    };
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
