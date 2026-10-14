#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipSipPairing.h"
#include "WipCrypto.h"
#include "WipPortalHttp.h"
#include "OpenKNX.h"
#include <ArduinoJson.h>
#include <esp_random.h>
#include <time.h>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace Wis
{
    namespace
    {
        const char *const kModuleTag = "WelcomeIPSip";
        const char *const kGeoUrl = "https://geo.mybuildings.abb.com";
        const char *const kDefaultPortalUrl = "https://api.eu.mybuildings.abb.com";
        // Verified against the real portal; the free@home-next label is a guess, see
        // the plan's "client-type" section and ClientTypeChoice::FreeAtHomeNext.
        const char *const kClientTypeWelcome = "com.abb.ispf.client.globalip.app.abb.android";
        const char *const kClientTypeFreeAtHomeNext = "com.abb.ispf.client.freeathome.app.abb.android";
        const char *const kGatewayClientType = "com.abb.ispf.client.welcome.gateway";
        const char *const kEventDiscovery = "com.abb.ispf.event.discovery";
        const char *const kEventConnect = "com.abb.ispf.event.welcome.connect";
        const char *const kEventAclUpdate = "com.abb.ispf.event.welcome.acl-update";

        // Backoff schedule for the ACL-update poll, summing to the plan's ~30 minutes
        // of patience before giving up and waiting for the user to retry ("wis pair").
        const uint32_t kAclBackoffMs[] = {3000, 10000, 30000, 60000, 120000, 300000};
        const uint8_t kAclBackoffSteps = sizeof(kAclBackoffMs) / sizeof(kAclBackoffMs[0]);
        const uint32_t kAclTotalWaitMs = 30UL * 60UL * 1000UL;
        const uint32_t kCertGuardSeconds = 24UL * 3600UL;
        const uint32_t kTaskStack = 16384; // RSA-2048 keygen needs headroom, per the plan

        bool due(uint32_t at)
        {
            return (int32_t)(millis() - at) >= 0;
        }

        bool timeValid()
        {
            return time(nullptr) > 1700000000;
        }

        std::string isoUtcNow()
        {
            const time_t t = time(nullptr);
            struct tm tmv;
            gmtime_r(&t, &tmv);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
            return buf;
        }

        std::string randomUuidV4()
        {
            unsigned char b[16];
            esp_fill_random(b, sizeof(b));
            b[6] = (unsigned char)((b[6] & 0x0F) | 0x40); // version 4
            b[8] = (unsigned char)((b[8] & 0x3F) | 0x80); // variant 10xx
            char out[37];
            snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                     b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            return out;
        }

        void copyText(char *dst, size_t size, const std::string &src)
        {
            strncpy(dst, src.c_str(), size - 1);
            dst[size - 1] = 0;
        }

        std::string trimmed(const std::string &s)
        {
            size_t start = 0, end = s.size();
            while (start < end && isspace((unsigned char)s[start])) start++;
            while (end > start && isspace((unsigned char)s[end - 1])) end--;
            return s.substr(start, end - start);
        }

        // Minimal INI parser for the ACL-update body: "[section]" headers and "key=value"
        // lines, enough for "[network] domain/localid" and "[outdoorstation_N] address/name".
        // configparser-equivalent features we deliberately don't need (interpolation,
        // comments other than ';'/'#') are left out.
        void parseIniDoors(const std::string &text, std::string &sipDomain, std::vector<DoorInfo> &doors)
        {
            // A handful of keys per section (3-5) — a linear-scan vector of pairs is
            // simpler here than std::map and avoids a GCC 14 -Wfree-nonheap-object false
            // positive seen with an std::map<string,string> at this call site.
            using KeyValue = std::pair<std::string, std::string>;
            struct Section
            {
                std::string name;
                std::vector<KeyValue> kv;

                const std::string *find(const char *key) const
                {
                    for (const auto &p : kv)
                        if (p.first == key) return &p.second;
                    return nullptr;
                }
            };
            std::vector<Section> sections;

            size_t pos = 0;
            while (pos <= text.size())
            {
                size_t eol = text.find('\n', pos);
                const std::string rawLine = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                pos = eol == std::string::npos ? text.size() + 1 : eol + 1;

                const std::string line = trimmed(rawLine);
                if (line.empty() || line[0] == ';' || line[0] == '#') continue;
                if (line.front() == '[' && line.back() == ']')
                {
                    sections.push_back({line.substr(1, line.size() - 2), {}});
                    continue;
                }
                if (sections.empty()) continue;
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                sections.back().kv.emplace_back(trimmed(line.substr(0, eq)), trimmed(line.substr(eq + 1)));
            }

            std::string localId;
            for (const auto &sec : sections)
            {
                if (sec.name != "network") continue;
                if (const std::string *v = sec.find("domain")) sipDomain = *v;
                if (const std::string *v = sec.find("localid")) localId = *v;
            }
            for (const auto &sec : sections)
            {
                if (sec.name.rfind("outdoorstation_", 0) != 0) continue;
                const std::string *address = sec.find("address");
                if (!address || address->empty()) continue;

                DoorInfo door;
                door.address = *address;
                const size_t colon = door.address.find(':');
                if (colon != std::string::npos)
                {
                    const std::string afterColon = door.address.substr(colon + 1);
                    const size_t at = afterColon.find('@');
                    door.stationId = at == std::string::npos ? afterColon : afterColon.substr(0, at);
                }
                const std::string *name = sec.find("name");
                door.name = name && !name->empty() ? *name : !localId.empty() ? localId : sec.name;
                if (const std::string *second = sec.find("secondunlock")) door.secondUnlockAddress = *second;
                doors.push_back(door);
            }
        }
    } // namespace

    void Pairing::begin(const PairingSettings &settings)
    {
        if (_task) return;
        _settings = settings;
        _queue = xQueueCreate(4, sizeof(Command));
        _mutex = xSemaphoreCreateMutex();

        _store.load(_persisted);
        const std::string hash = computeConfigHash();
        if (_persisted.phase == Phase::PAIRED && _persisted.configHash != hash)
        {
            // Never re-pair automatically — a changed user/client-type means a different
            // portal identity, and silently requesting a new certificate for it would be
            // surprising. The user confirms explicitly via "wis pair" or the KO.
            _work.configStale = true;
            setDiag("Pairing veraltet");
            logWarning(kModuleTag, "ETS-Konfiguration hat sich geaendert, Pairing bleibt bestehen bis \"wis pair\"");
        }
        else
        {
            setDiag(_persisted.phase == Phase::PAIRED ? "Verbunden" : "Nicht gepaart");
        }
        publish();

        xTaskCreatePinnedToCore(taskEntry, "WisPairing", kTaskStack, this, 1, &_task, 0);
    }

    bool Pairing::enqueue(CommandType type)
    {
        if (!_queue) return false;
        const Command cmd{type};
        return xQueueSend(_queue, &cmd, 0) == pdTRUE;
    }

    bool Pairing::snapshot(PairingState &out, uint32_t lastSeq)
    {
        if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
        const bool changed = _shared.seq != lastSeq;
        if (changed) out = _shared;
        xSemaphoreGive(_mutex);
        return changed;
    }

    bool Pairing::hasSipCredentials()
    {
        if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
        const bool ok = _shared.phase == Phase::ACL_RECEIVED || _shared.phase == Phase::PAIRED;
        xSemaphoreGive(_mutex);
        return ok;
    }

    void Pairing::publish()
    {
        _work.seq++;
        _work.phase = _persisted.phase;
        copyText(_work.integrityDisplay, sizeof(_work.integrityDisplay), _persisted.integrityDisplay);
        copyText(_work.gatewayName, sizeof(_work.gatewayName), _persisted.gatewayName);
        if (!_mutex) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _shared = _work;
        xSemaphoreGive(_mutex);
    }

    void Pairing::setDiag(const char *diag)
    {
        copyText(_work.diag, sizeof(_work.diag), diag);
    }

    void Pairing::fail(const char *diag, const std::string &error)
    {
        setDiag(diag);
        copyText(_work.lastError, sizeof(_work.lastError), error);
        logError(kModuleTag, "%s: %s", diag, error.c_str());
        publish();
    }

    std::string Pairing::clientTypeString() const
    {
        return _settings.clientType == ClientTypeChoice::Welcome ? kClientTypeWelcome : kClientTypeFreeAtHomeNext;
    }

    std::string Pairing::computeConfigHash() const
    {
        return md5Hex(_settings.username + "|" + _settings.gatewayUuidOverride + "|" + clientTypeString());
    }

    std::string Pairing::portalUrl()
    {
        if (!_persisted.portalUrl.empty()) return _persisted.portalUrl;

        setDiag("Portal-Adresse ermitteln");
        publish();
        const std::string sha = sha256Hex(_settings.username);
        const std::string url = std::string(kGeoUrl) + "/api/config/services?by_username_sha256=" + sha;
        HttpResponse res = PortalHttp::request("GET", url, {}, "", std::string(), std::string(), 10000);
        if (res.ok())
        {
            JsonDocument doc;
            if (!deserializeJson(doc, res.body))
            {
                JsonArrayConst hosts = doc["api"].as<JsonArrayConst>();
                if (hosts.size() > 0)
                {
                    const std::string host = hosts[0].as<std::string>();
                    if (!host.empty()) return "https://" + host;
                }
            }
        }
        logWarning(kModuleTag, "GEO-Suche fehlgeschlagen, verwende Standard-Portal");
        return kDefaultPortalUrl;
    }

    void Pairing::taskEntry(void *arg)
    {
        static_cast<Pairing *>(arg)->run();
        vTaskDelete(nullptr);
    }

    void Pairing::run()
    {
        // A reboot while CONNECT_SENT (the portal already has our connect event; we are
        // only waiting for the user's app confirmation) must resume polling by itself —
        // no second "wis pair" should be required just because the device restarted.
        if (_persisted.phase == Phase::CONNECT_SENT)
        {
            _nextAclPollAt = millis();
            _aclBackoffStep = 0;
            _aclDeadlineAt = millis() + kAclTotalWaitMs;
        }

        while (true)
        {
            Command cmd;
            const bool got = xQueueReceive(_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE;

            if (got && cmd.type == CMD_UNPAIR)
            {
                doUnpair();
                _nextAclPollAt = 0;
                continue;
            }
            if (got && cmd.type == CMD_PAIR)
            {
                _work.configStale = false;
                onPairCommand();
                if (_persisted.phase == Phase::CONNECT_SENT)
                {
                    _nextAclPollAt = millis();
                    _aclBackoffStep = 0;
                    _aclDeadlineAt = millis() + kAclTotalWaitMs;
                }
            }

            if (_persisted.phase == Phase::CONNECT_SENT && due(_nextAclPollAt))
            {
                if (stepPollAclOnce())
                {
                    finalizePairing();
                }
                else if (due(_aclDeadlineAt))
                {
                    setDiag("wartet auf Freigabe (Zeitlimit)");
                    publish();
                    _nextAclPollAt = 0x7FFFFFFFUL; // stop auto-polling; "wis pair" resumes it
                }
                else
                {
                    _nextAclPollAt = millis() + kAclBackoffMs[_aclBackoffStep];
                    if (_aclBackoffStep + 1 < kAclBackoffSteps) _aclBackoffStep++;
                }
            }
        }
    }

    void Pairing::onPairCommand()
    {
        if (!timeValid())
        {
            fail("warte auf NTP-Zeit", "Systemzeit noch nicht gueltig (NET_ServiceNTP aktivieren)");
            return;
        }

        if (_persisted.phase == Phase::NONE)
        {
            setDiag("Schluessel erzeugen");
            publish();
            KeyAndCsr kc = generateKeyAndCsr(_settings.username);
            if (!kc.ok)
            {
                fail("Schluesselerzeugung fehlgeschlagen", kc.error);
                return;
            }
            _persisted.privateKeyPem = kc.privateKeyPem;
            _store.saveKey(kc.privateKeyPem);
            _persisted.phase = Phase::KEY;
            setDiag("Schluessel erzeugt");
            publish();
        }

        if (_persisted.phase == Phase::KEY)
            if (!stepRequestCertificate()) return;

        if (_persisted.phase == Phase::CERT)
            if (!stepDiscoverGateway()) return;

        if (_persisted.phase == Phase::DISCOVERED)
            if (!stepSendConnect()) return;
    }

    bool Pairing::stepRequestCertificate()
    {
        const uint32_t now = (uint32_t)time(nullptr);
        const uint32_t last = _store.lastCertRequestEpoch();
        if (last != 0 && now > last && (now - last) < kCertGuardSeconds)
        {
            fail("Zertifikat gesperrt",
                 "letzte Anfrage vor " + std::to_string((now - last) / 3600) + "h, max. 1x/24h erlaubt");
            return false;
        }

        setDiag("Zertifikat anfragen");
        publish();

        KeyAndCsr kc = buildCsrFromKey(_persisted.privateKeyPem, _settings.username);
        if (!kc.ok)
        {
            fail("CSR fehlgeschlagen", kc.error);
            return false;
        }

        JsonDocument reqDoc;
        reqDoc["client-csr"] = base64Encode(kc.csrPem);
        reqDoc["client-name"] = _settings.clientName;
        reqDoc["client-type"] = clientTypeString();
        std::string body;
        serializeJson(reqDoc, body);

        const std::string url = portalUrl();
        HttpResponse res = digestRequest("POST", url + "/certificate/request", _settings.username, _settings.password,
                                         body);
        _store.markCertRequested(now); // counts the attempt, success or not — see the plan's 24h guard

        if (res.status == 401)
        {
            fail("Zugangsdaten abgelehnt", "Portal: HTTP 401 - MyBuildings-Benutzername/Passwort pruefen");
            return false;
        }
        if (!res.ok())
        {
            fail("Zertifikat abgelehnt", "Portal: HTTP " + std::to_string(res.status) +
                                            " (client-type evtl. nicht akzeptiert - ggf. auf \"Welcome\" umstellen)");
            return false;
        }

        std::string certPem = trimmed(res.body);
        if (certPem.rfind("-----BEGIN", 0) != 0)
        {
            JsonDocument d;
            std::string field;
            if (!deserializeJson(d, res.body))
            {
                field = d["client-certificate"].as<std::string>();
                if (field.empty()) field = d["certificate"].as<std::string>();
            }
            if (field.rfind("-----BEGIN", 0) != 0)
            {
                fail("Zertifikat unlesbar", "Portal-Antwort weder PEM noch erwartetes JSON");
                return false;
            }
            certPem = field;
        }

        _persisted.certPem = certPem;
        _persisted.portalUrl = url;
        _store.saveCert(certPem, url, _settings.clientName);
        _persisted.phase = Phase::CERT;
        setDiag("Zertifikat erhalten");
        publish();
        return true;
    }

    bool Pairing::stepDiscoverGateway()
    {
        Identity id = deriveIdentity(_persisted.certPem, _settings.username);
        if (!id.ok)
        {
            fail("Identitaet fehlgeschlagen", id.error);
            return false;
        }

        std::string gatewayUuid = _settings.gatewayUuidOverride;
        std::string gatewayName = "ABB Welcome Gateway";

        if (gatewayUuid.empty())
        {
            setDiag("Gateway suchen");
            publish();
            const std::string url = _persisted.portalUrl + "/event?type=" + kEventDiscovery +
                                    "&pagination_limit=1&pagination_page=1&order=desc";
            HttpResponse res = PortalHttp::request("GET", url, {}, "", _persisted.certPem, _persisted.privateKeyPem);
            if (!res.ok())
            {
                fail("Discovery fehlgeschlagen", "Portal: HTTP " + std::to_string(res.status));
                return false;
            }

            JsonDocument doc;
            if (deserializeJson(doc, res.body))
            {
                fail("Discovery unlesbar", "Antwort kein gueltiges JSON");
                return false;
            }
            JsonArrayConst events = doc["events"].as<JsonArrayConst>();
            if (events.size() == 0)
            {
                fail("kein Gateway im Portal sichtbar", "Discovery-Event-Liste leer");
                return false;
            }

            std::string payloadRaw;
            if (!base64Decode(events[0]["payload"].as<std::string>(), payloadRaw))
            {
                fail("Discovery unlesbar", "Payload nicht Base64");
                return false;
            }

            JsonDocument entries;
            if (deserializeJson(entries, payloadRaw))
            {
                fail("Discovery unlesbar", "Payload kein gueltiges JSON");
                return false;
            }
            bool found = false;
            for (JsonPairConst kv : entries.as<JsonObjectConst>())
            {
                if (kv.value()["type"].as<std::string>() == kGatewayClientType)
                {
                    gatewayUuid = kv.key().c_str();
                    const std::string name = kv.value()["name"].as<std::string>();
                    if (!name.empty()) gatewayName = name;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                fail("kein Gateway im Portal sichtbar", "Discovery ohne Eintrag vom Typ " +
                                                             std::string(kGatewayClientType));
                return false;
            }
        }

        _persisted.fingerprintSha1 = id.fingerprintSha1;
        _persisted.gruu = id.gruu;
        _persisted.sipUsername = id.sipUsername;
        _persisted.ownPortalUuid = id.ownPortalUuid;
        _persisted.gatewayUuid = gatewayUuid;
        _persisted.gatewayName = gatewayName;
        _store.saveDiscovered(id.fingerprintSha1, id.gruu, id.sipUsername, id.ownPortalUuid, gatewayUuid, gatewayName);
        _persisted.phase = Phase::DISCOVERED;
        setDiag("Gateway gefunden");
        publish();
        return true;
    }

    bool Pairing::stepSendConnect()
    {
        IntegrityCode code = computeIntegrityCode(_persisted.fingerprintSha1);

        JsonDocument doc;
        doc["id"] = randomUuidV4();
        doc["type"] = kEventConnect;
        doc["timestamp"] = isoUtcNow();
        doc["destination"][0] = _persisted.gatewayUuid;
        doc["source"] = _persisted.ownPortalUuid;
        doc["payload"] = code.eight;
        std::string body;
        serializeJson(doc, body);

        const std::string url = _persisted.portalUrl + "/event";
        HttpResponse res = PortalHttp::request("POST", url, {"Content-Type: application/json"}, body, _persisted.certPem,
                                               _persisted.privateKeyPem);
        if (!res.ok())
        {
            fail("Connect abgelehnt", "Portal: HTTP " + std::to_string(res.status));
            return false;
        }

        _persisted.integrityEight = code.eight;
        _persisted.integrityDisplay = code.display;
        _store.saveConnectSent(code.eight, code.display);
        _persisted.phase = Phase::CONNECT_SENT;
        setDiag("wartet auf Freigabe");
        publish();
        logInfo(kModuleTag, "Integritaetscode fuer die App: %s", code.display.c_str());
        return true;
    }

    bool Pairing::stepPollAclOnce()
    {
        const std::string url = _persisted.portalUrl + "/event?type=" + kEventAclUpdate +
                                "&order=desc&pagination_limit=5&pagination_page=1";
        HttpResponse res = PortalHttp::request("GET", url, {}, "", _persisted.certPem, _persisted.privateKeyPem);
        if (!res.ok()) return false; // transient — the backoff loop in run() retries

        JsonDocument doc;
        if (deserializeJson(doc, res.body)) return false;

        for (JsonVariantConst evtVar : doc["events"].as<JsonArrayConst>())
        {
            JsonObjectConst evt = evtVar.as<JsonObjectConst>();
            bool matches = false;
            for (JsonVariantConst d : evt["destination"].as<JsonArrayConst>())
                if (d.as<std::string>() == _persisted.ownPortalUuid)
                {
                    matches = true;
                    break;
                }
            if (!matches) continue;

            std::string payloadRaw;
            if (!base64Decode(evt["payload"].as<std::string>(), payloadRaw)) continue;
            if (parseAndSaveAclUpdate(payloadRaw)) return true;
        }
        return false;
    }

    bool Pairing::parseAndSaveAclUpdate(const std::string &payload)
    {
        const size_t nl = payload.find('\n');
        if (nl == std::string::npos)
        {
            fail("ACL-Update unlesbar", "kein Zeilenumbruch in der Nutzlast");
            return false;
        }
        const std::string encryptedB64 = trimmed(payload.substr(0, nl));

        std::string rest = payload.substr(nl + 1);
        std::string restClean;
        restClean.reserve(rest.size());
        for (char c : rest)
            if (c != '\0') restClean += c;

        std::string cipher;
        if (!base64Decode(encryptedB64, cipher))
        {
            fail("ACL-Update unlesbar", "SIP-Passwort nicht Base64");
            return false;
        }

        std::string sipPassword, rsaError;
        if (!rsaDecryptPkcs1(_persisted.privateKeyPem, cipher, sipPassword, rsaError))
        {
            fail("SIP-Passwort-Entschluesselung fehlgeschlagen", rsaError);
            return false;
        }

        std::string sipDomain;
        std::vector<DoorInfo> doors;
        parseIniDoors(restClean, sipDomain, doors);
        if (sipDomain.empty() || doors.empty())
        {
            fail("ACL-Update unvollstaendig", "keine SIP-Domain oder keine Aussenstationen gefunden");
            return false;
        }

        JsonDocument doorsDoc;
        JsonArray arr = doorsDoc.to<JsonArray>();
        for (const auto &d : doors)
        {
            JsonObject o = arr.add<JsonObject>();
            o["name"] = d.name;
            o["stationId"] = d.stationId;
            o["address"] = d.address;
            o["secondUnlockAddress"] = d.secondUnlockAddress;
        }
        std::string doorsJson;
        serializeJson(doorsDoc, doorsJson);

        _persisted.sipPassword = sipPassword;
        _persisted.sipDomain = sipDomain;
        _persisted.doorsJson = doorsJson;
        _store.saveAclReceived(sipPassword, sipDomain, doorsJson);
        _persisted.phase = Phase::ACL_RECEIVED;
        setDiag("Freigabe erhalten");
        publish();
        logInfo(kModuleTag, "ACL-Update erhalten: %u Aussenstation(en)", (unsigned)doors.size());
        return true;
    }

    void Pairing::finalizePairing()
    {
        _store.savePaired(computeConfigHash());
        _persisted.phase = Phase::PAIRED;

        JsonDocument doc;
        if (!deserializeJson(doc, _persisted.doorsJson)) _work.doorCount = (uint8_t)doc.as<JsonArrayConst>().size();
        setDiag("Verbunden");
        publish();
    }

    void Pairing::doUnpair()
    {
        // Does not contact the portal (no known delete endpoint, see the plan) and does
        // not deregister the SIP link — that is WipSipLink's job once it exists; the
        // module's "wis unpair" command must call both.
        setDiag("Unpair...");
        publish();
        _store.clear();
        _persisted = PersistedPairing();
        _work.doorCount = 0;
        _work.configStale = false;
        setDiag("Nicht gepaart");
        publish();
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
