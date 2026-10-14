#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipXmppLink.h"
#include "OpenKNX.h"
#include <cstring>

namespace Wis
{
    namespace
    {
        const char *const kModuleTag = "WelcomeIPSip";
        const uint32_t kConnectTimeoutMs = 10000;
        const uint32_t kRpcTimeoutMs = 10000;
        const uint32_t kTaskStack = 8192;

        void copyText(char *dst, size_t size, const std::string &src)
        {
            strncpy(dst, src.c_str(), size - 1);
            dst[size - 1] = 0;
        }
    } // namespace

    void XmppLink::begin(const XmppLinkSettings &settings)
    {
        if (_task) return;
        _settings = settings;
        _queue = xQueueCreate(2, sizeof(Command));
        _mutex = xSemaphoreCreateMutex();
        setDiag("Bereit");
        publish();
        xTaskCreatePinnedToCore(taskEntry, "WisXmppLink", kTaskStack, this, 1, &_task, 0);
    }

    bool XmppLink::enqueue(CommandType type, const std::string &contactId, const std::string &stationId, uint8_t lockIndex)
    {
        if (!_queue) return false;
        if (contactId.empty()) return false; // e.g. "Zweitschloss" on a door without a second lock
        Command cmd{};
        cmd.type = type;
        copyText(cmd.contactId, sizeof(cmd.contactId), contactId);
        copyText(cmd.stationId, sizeof(cmd.stationId), stationId);
        cmd.lockIndex = lockIndex;
        return xQueueSend(_queue, &cmd, 0) == pdTRUE;
    }

    bool XmppLink::snapshot(XmppLinkState &out, uint32_t lastSeq)
    {
        if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
        const bool changed = _shared.seq != lastSeq;
        if (changed) out = _shared;
        xSemaphoreGive(_mutex);
        return changed;
    }

    void XmppLink::publish()
    {
        _work.seq++;
        if (!_mutex) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _shared = _work;
        xSemaphoreGive(_mutex);
    }

    void XmppLink::setDiag(const char *diag)
    {
        copyText(_work.diag, sizeof(_work.diag), diag);
    }

    void XmppLink::taskEntry(void *arg)
    {
        static_cast<XmppLink *>(arg)->run();
        vTaskDelete(nullptr);
    }

    void XmppLink::run()
    {
        while (true)
        {
            Command cmd;
            if (xQueueReceive(_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
            if (cmd.type != CMD_UNLOCK) continue;

            _work.phase = XmppLinkPhase::Connecting;
            setDiag("Verbinde");
            publish();

            std::string err;
            if (!_client.connectAndAuthenticate(_settings.xmppHost, _settings.xmppPort, _settings.xmppDomain,
                                                _settings.certPem, _settings.privateKeyPem, kConnectTimeoutMs, err))
            {
                _work.phase = XmppLinkPhase::Error;
                setDiag("Verbindung fehlgeschlagen");
                copyText(_work.lastError, sizeof(_work.lastError), err);
                copyText(_work.lastStationId, sizeof(_work.lastStationId), cmd.stationId);
                _work.lastFinishedAtMs = millis();
                logError(kModuleTag, "Tueroeffner: Verbindung fehlgeschlagen: %s", err.c_str());
                publish();
                continue;
            }

            _work.phase = XmppLinkPhase::Unlocking;
            setDiag("Entsperren...");
            publish();

            const std::string to = _settings.gatewayUuid + "@" + _settings.jidDomain;
            XmppRpcResult result = _client.sendUnlock(to, cmd.contactId, kRpcTimeoutMs);
            _client.close();

            copyText(_work.lastStationId, sizeof(_work.lastStationId), cmd.stationId);
            _work.lastFinishedAtMs = millis();
            _work.lastResultCode = result.resultCode;

            if (result.ok)
            {
                _work.phase = XmppLinkPhase::Done;
                _work.lastError[0] = 0;
                if (result.resultCode == 0)
                {
                    setDiag("Entsperrt");
                    logInfo(kModuleTag, "Tueroeffner: Station %s (Schloss %u) entsperrt", cmd.stationId,
                           (unsigned)cmd.lockIndex);
                }
                else
                {
                    setDiag("Fehler (RPC)");
                    logWarning(kModuleTag, "Tueroeffner: Station %s, resultCode %d", cmd.stationId, result.resultCode);
                }
            }
            else
            {
                _work.phase = XmppLinkPhase::Error;
                setDiag("Entsperren fehlgeschlagen");
                copyText(_work.lastError, sizeof(_work.lastError), result.error);
                logError(kModuleTag, "Tueroeffner: Station %s fehlgeschlagen: %s", cmd.stationId, result.error.c_str());
            }
            publish();
        }
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
