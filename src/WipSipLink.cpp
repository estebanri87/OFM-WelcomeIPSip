#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipSipLink.h"
#include "OpenKNX.h"
#include <esp_random.h>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace Wis
{
    namespace
    {
        const char *const kModuleTag = "WelcomeIPSip";
        const char *const kTransportParam = ";transport=tls";

        // Plan: "Backoff 2/5/10/30/60 s".
        const uint32_t kReconnectBackoffMs[] = {2000, 5000, 10000, 30000, 60000};
        const uint8_t kReconnectBackoffSteps = sizeof(kReconnectBackoffMs) / sizeof(kReconnectBackoffMs[0]);
        const uint32_t kRegisterExpiresSeconds = 600;
        const uint32_t kKeepaliveIntervalMs = 30000;
        const uint32_t kRingTimeoutMs = 60000; // plan: "Fallback-Timeout ~60 s"
        const uint32_t kTaskStack = 8192;

        bool due(uint32_t at)
        {
            return (int32_t)(millis() - at) >= 0;
        }

        void copyText(char *dst, size_t size, const std::string &src)
        {
            strncpy(dst, src.c_str(), size - 1);
            dst[size - 1] = 0;
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

        std::string randomUuidV4()
        {
            unsigned char b[16];
            esp_fill_random(b, sizeof(b));
            b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
            b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
            char out[37];
            snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                     b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            return out;
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

        // User part of a From/To/Request-URI-style header value, e.g.
        // "\"Name\" <sip:12345@domain;transport=tls>;tag=..." -> "12345". Falls back to
        // treating the whole value as a bare "sip:user@..." or "user@..." form.
        std::string sipUserPart(const std::string &headerValue)
        {
            std::string uri = headerValue;
            const size_t lt = uri.find('<');
            if (lt != std::string::npos)
            {
                const size_t gt = uri.find('>', lt + 1);
                uri = uri.substr(lt + 1, gt == std::string::npos ? std::string::npos : gt - lt - 1);
            }
            const size_t sip = uri.find("sip:");
            if (sip != std::string::npos) uri = uri.substr(sip + 4);
            const size_t at = uri.find('@');
            if (at != std::string::npos) uri = uri.substr(0, at);
            const size_t semi = uri.find(';');
            if (semi != std::string::npos) uri = uri.substr(0, semi);
            return trimmed(uri);
        }

        // "METHOD request-uri SIP/2.0" -> "request-uri".
        std::string requestUriOf(const std::string &startLine)
        {
            const size_t sp1 = startLine.find(' ');
            if (sp1 == std::string::npos) return std::string();
            const size_t sp2 = startLine.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) return std::string();
            return startLine.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    } // namespace

    void Link::begin(const LinkSettings &settings)
    {
        if (_task) return;
        _settings = settings;
        _instanceUuid = randomUuidV4();
        _queue = xQueueCreate(2, sizeof(Command));
        _mutex = xSemaphoreCreateMutex();
        setDiag("Nicht verbunden");
        publish();
        xTaskCreatePinnedToCore(taskEntry, "WisSipLink", kTaskStack, this, 1, &_task, 0);
    }

    bool Link::enqueue(CommandType type)
    {
        if (!_queue) return false;
        const Command cmd{type};
        return xQueueSend(_queue, &cmd, 0) == pdTRUE;
    }

    bool Link::snapshot(LinkState &out, uint32_t lastSeq)
    {
        if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
        const bool changed = _shared.seq != lastSeq;
        if (changed) out = _shared;
        xSemaphoreGive(_mutex);
        return changed;
    }

    void Link::publish()
    {
        _work.seq++;
        if (!_mutex) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _shared = _work;
        xSemaphoreGive(_mutex);
    }

    void Link::setDiag(const char *diag)
    {
        copyText(_work.diag, sizeof(_work.diag), diag);
    }

    void Link::fail(const char *diag, const std::string &error)
    {
        setDiag(diag);
        copyText(_work.lastError, sizeof(_work.lastError), error);
        logError(kModuleTag, "%s: %s", diag, error.c_str());
        publish();
    }

    void Link::taskEntry(void *arg)
    {
        static_cast<Link *>(arg)->run();
        vTaskDelete(nullptr);
    }

    void Link::scheduleReconnect()
    {
        _nextConnectAttemptAtMs = millis() + kReconnectBackoffMs[_reconnectBackoffStep];
        if (_reconnectBackoffStep + 1 < kReconnectBackoffSteps) _reconnectBackoffStep++;
        _work.reconnectCount++;
        publish();
    }

    void Link::teardown(const char *diag)
    {
        _client.close();
        _work.phase = LinkPhase::Disconnected;
        _work.registeredSinceMs = 0;
        _openCalls.clear(); // the connection they live on is gone
        for (auto &slot : _work.ring) slot = RingSlot();
        if (diag) setDiag(diag);
        publish();
    }

    bool Link::isKnownOutdoorStation(const std::string &stationId) const
    {
        for (const auto &id : _settings.doorStationIds)
            if (id == stationId) return true;
        return false;
    }

    void Link::publishRingTable()
    {
        for (auto &slot : _work.ring) slot = RingSlot();
        for (size_t i = 0; i < _openCalls.size() && i < kMaxOpenCalls; i++)
        {
            RingSlot &slot = _work.ring[i];
            slot.active = true;
            slot.internalCall = _openCalls[i].internalCall;
            slot.sinceMs = _openCalls[i].startedAtMs;
            copyText(slot.stationId, sizeof(slot.stationId), _openCalls[i].stationId);
        }
        publish();
    }

    void Link::sendResponseTo(const SipFrame &req, int code, const char *reason)
    {
        _client.sendResponse(code, reason,
                             {"Via: " + req.header("Via"), "From: " + req.header("From"), "To: " + req.header("To"),
                              "Call-ID: " + req.header("Call-ID"), "CSeq: " + req.header("CSeq")});
    }

    void Link::handleIncomingRequest(const SipFrame &req)
    {
        const std::string method = req.method();
        if (method == "INVITE")
        {
            handleIncomingInvite(req);
        }
        else if (method == "CANCEL")
        {
            handleIncomingCancel(req);
        }
        else if (method == "BYE")
        {
            handleIncomingBye(req);
        }
        else if (method == "ACK")
        {
            // Acknowledges our final response to an INVITE (e.g. the 487 after a
            // CANCEL) — nothing to do, per the plan "ACK schlucken".
        }
        else if (method == "OPTIONS" || method == "NOTIFY")
        {
            sendResponseTo(req, 200, "OK");
        }
        else if (method == "MESSAGE")
        {
            // Likely door-unlock broadcasts from other clients (see the plan's
            // "Türöffner" section) — logged for now, not acted on.
            logInfo(kModuleTag, "eingehende MESSAGE: %s", req.body.c_str());
            sendResponseTo(req, 200, "OK");
        }
        else if (!method.empty())
        {
            sendResponseTo(req, 501, "Not Implemented");
        }
    }

    void Link::handleIncomingInvite(const SipFrame &req)
    {
        const std::string callId = req.header("Call-ID");
        for (const auto &call : _openCalls)
            if (call.callId == callId)
            {
                sendResponseTo(req, 100, "Trying"); // retransmission of an INVITE we already answered
                return;
            }

        if (_openCalls.size() >= kMaxOpenCalls)
        {
            sendResponseTo(req, 486, "Busy Here");
            logWarning(kModuleTag, "INVITE abgelehnt, bereits %u gleichzeitige Rufe offen", (unsigned)kMaxOpenCalls);
            return;
        }

        OpenCall call;
        call.callId = callId;
        call.via = req.header("Via");
        call.from = req.header("From");
        call.to = req.header("To");
        call.cseq = req.header("CSeq");
        call.startedAtMs = millis();

        // Plan: "Klingelnde Station = User-Teil des From-Headers, Fallback Request-URI".
        call.stationId = sipUserPart(call.from);
        if (call.stationId.empty()) call.stationId = sipUserPart(requestUriOf(req.startLine));
        call.internalCall = !isKnownOutdoorStation(call.stationId);

        logInfo(kModuleTag, call.internalCall ? "interner Ruf von %s (Call-ID %s), kein Kanal-KO"
                                              : "Klingeln: Station %s (Call-ID %s)",
               call.stationId.c_str(), callId.c_str());

        _openCalls.push_back(call);
        sendResponseTo(req, 100, "Trying");
        publishRingTable();
    }

    void Link::handleIncomingCancel(const SipFrame &req)
    {
        sendResponseTo(req, 200, "OK"); // answers the CANCEL's own transaction

        const std::string callId = req.header("Call-ID");
        for (auto it = _openCalls.begin(); it != _openCalls.end(); ++it)
        {
            if (it->callId != callId) continue;
            // The 487 belongs to the original INVITE transaction, not the CANCEL's.
            _client.sendResponse(487, "Request Terminated",
                                 {"Via: " + it->via, "From: " + it->from, "To: " + it->to, "Call-ID: " + it->callId,
                                  "CSeq: " + it->cseq});
            logInfo(kModuleTag, "Klingeln beendet (CANCEL): Station %s", it->stationId.c_str());
            _openCalls.erase(it);
            publishRingTable();
            return;
        }
    }

    void Link::handleIncomingBye(const SipFrame &req)
    {
        sendResponseTo(req, 200, "OK");

        const std::string callId = req.header("Call-ID");
        for (auto it = _openCalls.begin(); it != _openCalls.end(); ++it)
        {
            if (it->callId != callId) continue;
            logInfo(kModuleTag, "Klingeln beendet (BYE): Station %s", it->stationId.c_str());
            _openCalls.erase(it);
            publishRingTable();
            return;
        }
    }

    void Link::expireStaleCalls()
    {
        bool changed = false;
        for (auto it = _openCalls.begin(); it != _openCalls.end();)
        {
            if (millis() - it->startedAtMs > kRingTimeoutMs)
            {
                logWarning(kModuleTag, "Klingeln beendet (Zeitlimit): Station %s", it->stationId.c_str());
                it = _openCalls.erase(it);
                changed = true;
            }
            else
                ++it;
        }
        if (changed) publishRingTable();
    }

    void Link::run()
    {
        while (true)
        {
            Command cmd;
            const bool got = xQueueReceive(_queue, &cmd, pdMS_TO_TICKS(250)) == pdTRUE;

            if (got && cmd.type == CMD_REREGISTER)
            {
                teardown("Neuverbindung angefordert");
                _reconnectBackoffStep = 0;
                _nextConnectAttemptAtMs = millis();
            }

            if (_work.phase == LinkPhase::Disconnected && due(_nextConnectAttemptAtMs))
            {
                _work.phase = LinkPhase::Connecting;
                publish();
                if (connectAndRegister())
                {
                    _reconnectBackoffStep = 0;
                }
                else
                {
                    teardown(nullptr); // fail() already set a specific diag/lastError
                    scheduleReconnect();
                }
                continue;
            }

            if (_work.phase != LinkPhase::Registered) continue;

            if (due(_refreshAtMs))
            {
                if (!performRegister(kRegisterExpiresSeconds))
                {
                    teardown(nullptr);
                    scheduleReconnect();
                    continue;
                }
                _refreshAtMs = millis() + (kRegisterExpiresSeconds * 900UL); // refresh at 90% of Expires
                setDiag("Registriert");
                publish();
                continue;
            }
            if (due(_nextKeepaliveAtMs))
            {
                if (!_client.sendKeepalivePing())
                {
                    fail("Verbindung verloren", "Keepalive konnte nicht gesendet werden");
                    teardown(nullptr);
                    scheduleReconnect();
                    continue;
                }
                _nextKeepaliveAtMs = millis() + kKeepaliveIntervalMs;
                continue;
            }

            expireStaleCalls();

            SipFrame frame;
            bool timedOut = false;
            if (_client.tryRecvFrame(frame, 200, timedOut))
            {
                if (frame.isRequest())
                    handleIncomingRequest(frame);
                else
                    logDebug(kModuleTag, "unerwartete Antwort ausserhalb einer Transaktion: %s", frame.startLine.c_str());
            }
            else if (!timedOut)
            {
                fail("Verbindung verloren", "Panel hat die Verbindung geschlossen");
                teardown(nullptr);
                scheduleReconnect();
            }
        }
    }

    bool Link::connectAndRegister()
    {
        setDiag("Verbinde");
        publish();

        std::string err;
        if (!_client.connect(_settings.panelHost, _settings.panelPort, 10000, err))
        {
            fail("Verbindung fehlgeschlagen", err);
            return false;
        }

        // Fresh dialog identity for this TLS connection.
        _callId = randomHex(8) + "@" + _settings.sipDomain;
        _fromTag = randomHex(4);
        _cseq = 0;

        setDiag("Registrierung bereinigen");
        publish();
        // Safety unregister (Contact: *, Expires: 0) — the AOR belongs only to this
        // device, so this is safe and prevents the orphaned-binding accumulation
        // observed in testing (see the plan). A failure here is not fatal.
        if (!performRegister(0)) logWarning(kModuleTag, "Unregister-Schritt ohne Erfolg, fahre mit REGISTER fort");

        setDiag("Registriere");
        publish();
        if (!performRegister(kRegisterExpiresSeconds)) return false;

        _refreshAtMs = millis() + (kRegisterExpiresSeconds * 900UL);
        _nextKeepaliveAtMs = millis() + kKeepaliveIntervalMs;
        _work.registeredSinceMs = millis();
        _work.phase = LinkPhase::Registered;
        setDiag("Registriert");
        publish();

        const uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < _work.minFreeHeapSeen) _work.minFreeHeapSeen = freeHeap;
        logInfo(kModuleTag, "SIP registriert, freier Heap %u Byte (Minimum seit Start %u Byte)", (unsigned)freeHeap,
               (unsigned)_work.minFreeHeapSeen);
        return true;
    }

    bool Link::performRegister(uint32_t expiresSeconds)
    {
        const std::string regUri = "sip:" + _settings.sipDomain + kTransportParam;
        const std::string aorUri = "sip:" + _settings.sipUsername + "@" + _settings.sipDomain + kTransportParam;
        const std::string localIp = _client.localIp();
        const uint16_t localPort = _client.localPort();
        const std::string contact = "<sip:" + _settings.sipUsername + "@" + localIp + ":" + std::to_string(localPort) +
                                    kTransportParam + ">;+sip.instance=\"<urn:uuid:" + _instanceUuid + ">\";reg-id=1";

        _cseq++;
        std::vector<std::string> headers = {
            "Via: SIP/2.0/TLS " + localIp + ":" + std::to_string(localPort) + ";branch=z9hG4bK-" + randomHex(6) + ";rport",
            "Max-Forwards: 70",
            "From: <" + aorUri + ">;tag=" + _fromTag,
            "To: <" + aorUri + ">",
            "Call-ID: " + _callId,
            "CSeq: " + std::to_string(_cseq) + " REGISTER",
            "Contact: " + (expiresSeconds == 0 ? std::string("*") : contact),
            "Expires: " + std::to_string(expiresSeconds),
            "Supported: outbound",
            "User-Agent: OpenKNX-WelcomeIPSip/1.0",
        };

        if (!_client.send("REGISTER", regUri, headers))
        {
            fail("REGISTER senden fehlgeschlagen", "Schreibfehler auf der Verbindung");
            return false;
        }

        SipFrame response;
        if (!waitFinalResponse(response, _callId, _cseq, "REGISTER", 10000))
        {
            fail("REGISTER ohne Antwort", "Zeitlimit");
            return false;
        }

        // Up to two authenticated retries: the normal challenge-response round, and —
        // if that nonce is immediately marked stale — one more with the fresh nonce
        // from the second challenge (plan: "stale=true einmal ohne Fehler wiederholen").
        for (uint8_t authAttempt = 0; authAttempt < 2; authAttempt++)
        {
            const int code = response.statusCode();
            if (code != 401 && code != 407) break;

            const bool isProxy = code == 407;
            std::string challenge = isProxy ? response.header("Proxy-Authenticate") : response.header("WWW-Authenticate");
            if (startsWithNoCase(challenge, "digest")) challenge = trimmed(challenge.substr(strlen("digest")));
            if (challenge.empty())
            {
                fail("REGISTER abgelehnt", "SIP " + std::to_string(code) + " ohne Digest-Challenge");
                return false;
            }
            const bool stale = challenge.find("stale=true") != std::string::npos ||
                                challenge.find("stale=\"true\"") != std::string::npos;
            if (authAttempt == 1 && !stale)
            {
                fail("REGISTER abgelehnt", "Zugangsdaten vom Panel erneut abgelehnt (SIP " + std::to_string(code) + ")");
                return false;
            }

            const std::string authHeader = sipDigestHeader(_settings.sipUsername, _settings.sipPassword, "REGISTER",
                                                            regUri, challenge, isProxy ? "Proxy-Authorization" : "Authorization");
            _cseq++;
            headers[0] = "Via: SIP/2.0/TLS " + localIp + ":" + std::to_string(localPort) + ";branch=z9hG4bK-" +
                        randomHex(6) + ";rport";
            headers[5] = "CSeq: " + std::to_string(_cseq) + " REGISTER";
            // Drop a previous attempt's auth header before adding the new one (relevant
            // only for the second, stale-triggered retry).
            if (!headers.empty() &&
                (startsWithNoCase(headers.back(), "authorization:") || startsWithNoCase(headers.back(), "proxy-authorization:")))
                headers.pop_back();
            headers.push_back(authHeader);

            if (!_client.send("REGISTER", regUri, headers))
            {
                fail("REGISTER senden fehlgeschlagen", "Schreibfehler auf der Verbindung (nach Digest)");
                return false;
            }
            if (!waitFinalResponse(response, _callId, _cseq, "REGISTER", 10000))
            {
                fail("REGISTER ohne Antwort", "Zeitlimit nach Digest-Antwort");
                return false;
            }
        }

        if (response.statusCode() != 200)
        {
            fail("REGISTER abgelehnt", "SIP " + std::to_string(response.statusCode()) + " " + response.startLine);
            return false;
        }
        return true;
    }

    bool Link::waitFinalResponse(SipFrame &out, const std::string &callId, uint32_t cseq, const char *method,
                                 uint32_t timeoutMs)
    {
        const uint32_t deadline = millis() + timeoutMs;
        const std::string wantCseq = std::to_string(cseq) + " " + method;
        while ((int32_t)(millis() - deadline) < 0)
        {
            const uint32_t remaining = deadline - millis(); // both unsigned; deadline not yet reached here
            bool timedOut = false;
            SipFrame frame;
            if (!_client.tryRecvFrame(frame, remaining, timedOut))
            {
                if (timedOut) continue; // overall deadline check above will end the loop
                return false;           // connection gone
            }
            if (!frame.isResponse()) continue; // an incoming request on the same connection; not what we're waiting for here
            if (frame.header("Call-ID") != callId) continue;
            if (frame.header("CSeq") != wantCseq) continue;
            if (frame.statusCode() / 100 == 1) continue; // provisional, keep waiting
            out = frame;
            return true;
        }
        return false;
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
