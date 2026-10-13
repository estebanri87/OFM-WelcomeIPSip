#include "WelcomeIPSipConfig.h"
#if defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "WelcomeIPSipWebPage.h"
#include "NetworkModule.h"
#include "OpenKNX/Network/Webserver/WebRequest.h"
#include "OpenKNX/Network/Webserver/WebResponse.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "WelcomeIPSipModule.h"
#include <cstring>

using namespace OpenKNX::Network;

namespace
{
    const char *phaseText(Wis::Phase phase)
    {
        switch (phase)
        {
            case Wis::Phase::NONE: return "Nicht gepaart.";
            case Wis::Phase::KEY: return "Schlüssel erzeugt.";
            case Wis::Phase::CERT: return "Zertifikat erhalten.";
            case Wis::Phase::DISCOVERED: return "Gateway gefunden.";
            case Wis::Phase::CONNECT_SENT: return "Wartet auf Freigabe.";
            case Wis::Phase::ACL_RECEIVED: return "Freigabe erhalten.";
            case Wis::Phase::PAIRED: return "Verbunden.";
        }
        return "?";
    }

    std::string elapsedText(uint32_t sinceMs)
    {
        if (sinceMs == 0) return "n/a";
        const uint32_t diff = millis() - sinceMs;
        if (diff < 60000UL) return "vor " + std::to_string(diff / 1000) + " s";
        if (diff < 3600000UL) return "vor " + std::to_string(diff / 60000) + " min";
        return "vor " + std::to_string(diff / 3600000) + " h";
    }

    std::string htmlEscape(const std::string &s)
    {
        std::string r;
        r.reserve(s.size());
        for (char c : s)
        {
            switch (c)
            {
                case '&': r += "&amp;"; break;
                case '<': r += "&lt;"; break;
                case '>': r += "&gt;"; break;
                case '"': r += "&quot;"; break;
                default: r += c; break;
            }
        }
        return r;
    }

    void sendHtml(WebResponse &res, const std::string &html, const char *activeMenu)
    {
        res.setContentType("text/html; charset=UTF-8");
        res.setLayout(true);
        res.setActiveMenu(activeMenu);
        res.send(html.c_str());
    }

    void sendMetaRefresh(WebResponse &res, const char *heading, const std::string &message)
    {
        std::string html = "<div class='container'>";
        html += "<meta http-equiv='refresh' content='3;url=/wis'>";
        html += "<h1>" + std::string(heading) + "</h1>";
        html += "<p>" + message + "</p>";
        html += "<p><a href='/wis'>Status jetzt öffnen</a></p></div>";
        sendHtml(res, html, "/wis");
    }
} // namespace

void WelcomeIPSipWebPage::setup()
{
    openknxNetwork.webserver.addMenuItem("WelcomeIP SIP", "/wis", 45);

    openknxNetwork.webserver.addRoute(WEB_GET, "/wis", [this](WebRequest &req, WebResponse &res) { handleStatus(req, res); });
    openknxNetwork.webserver.addRoute(WEB_GET, "/wis/pair", [this](WebRequest &req, WebResponse &res) { handlePair(req, res); });
    openknxNetwork.webserver.addRoute(WEB_GET, "/wis/unpair", [this](WebRequest &req, WebResponse &res) { handleUnpair(req, res); });

    openknxNetwork.webserver.addSocket(
        "/ws/wis",
        [](int id, WebSocketFrame *f) { (void)id; (void)f; }, // read-only: die Seite steuert nichts (wie Hue "Option C")
        [this](int id, bool connected) { handleWsConnect(id, connected); });
}

void WelcomeIPSipWebPage::handleStatus(WebRequest &req, WebResponse &res)
{
    (void)req;

    Wis::PairingState pairing;
    _module.pairing().snapshot(pairing, pairing.seq - 1);
    Wis::LinkState link;
    _module.link().snapshot(link, link.seq - 1);

    const bool connected = link.phase == Wis::LinkPhase::Registered;

    std::string html = "<div class='container'><h1>WelcomeIP SIP</h1>";

    html += "<h2>Verbindung</h2><table class='attribute-table'><tbody>";
    html += "<tr><td>SIP</td><td><span class='" + std::string(connected ? "green" : "red") + "'>" +
            (connected ? "Registriert" : "Nicht registriert") + "</span></td></tr>";
    html += "<tr><td>Panel-Adresse</td><td><code>" + htmlEscape(ParamWIS_WISPanelHostStr) + "</code></td></tr>";
    html += "<tr><td>Eigene IP</td><td><code>" + std::string(openknxNetwork.localIP().toString().c_str()) + "</code></td></tr>";
    if (connected) html += "<tr><td>Registriert</td><td>" + elapsedText(link.registeredSinceMs) + "</td></tr>";
    if (link.lastError[0]) html += "<tr><td>Letzter SIP-Fehler</td><td>" + htmlEscape(link.lastError) + "</td></tr>";
    html += "</tbody></table>";

    html += "<h2>Pairing</h2><table class='attribute-table'><tbody>";
    html += "<tr><td>Phase</td><td>" + std::string(phaseText(pairing.phase)) + "</td></tr>";
    html += "<tr><td>Client-Name</td><td><code>" + htmlEscape(ParamWIS_WISClientNameStr) + "</code></td></tr>";
    html += "<tr><td>App-Typ</td><td>" + std::string(ParamWIS_WISClientType == 1 ? "Welcome" : "free@home next") + "</td></tr>";
    if (pairing.configStale)
        html += "<tr><td colspan='2'><span class='yellow'>ETS-Konfiguration hat sich seit dem Pairing geändert.</span></td></tr>";
    if (pairing.phase == Wis::Phase::CONNECT_SENT && pairing.integrityDisplay[0])
        html += "<tr><td>Integritätscode</td><td><code>" + std::string(pairing.integrityDisplay) + "</code> (in der App bestätigen)</td></tr>";
    if (pairing.gatewayName[0]) html += "<tr><td>Gateway</td><td>" + htmlEscape(pairing.gatewayName) + "</td></tr>";
    if (pairing.lastError[0]) html += "<tr><td>Letzter Pairing-Fehler</td><td>" + htmlEscape(pairing.lastError) + "</td></tr>";
    html += "</tbody></table>";
    html += "<p>";
    html += pairing.phase == Wis::Phase::NONE ? "<a href='/wis/pair'>Pairing starten</a>"
                                              : "<a href='/wis/pair'>Pairing fortsetzen</a>";
    if (pairing.phase != Wis::Phase::NONE) html += "&nbsp;&nbsp;<a href='/wis/unpair'>Unpair</a>";
    html += "</p>";

    html += "<h2>Stationen</h2><table class='attribute-table'><tbody>";
    html += "<tr><th>Nr</th><th>Name</th><th>Kanal</th><th>Klingeln</th></tr>";
    Wis::DoorInfo door;
    for (uint8_t n = 1; _module.doorForStation(n, door); n++)
    {
        const int ch = _module.channelIndexForStation(n);
        bool ringing = false;
        uint32_t ringSince = 0;
        for (const auto &slot : link.ring)
            if (slot.active && !slot.internalCall && slot.stationId == door.stationId)
            {
                ringing = true;
                ringSince = slot.sinceMs;
                break;
            }
        html += "<tr data-station-id='" + htmlEscape(door.stationId) + "'>";
        html += "<td>" + std::to_string(n) + "</td>";
        html += "<td>" + htmlEscape(door.name) + "</td>";
        html += "<td>" + (ch >= 0 ? "Kanal " + std::to_string(ch + 1) : std::string("-")) + "</td>";
        html += "<td class='wis-ring'>" + std::string(ringing ? "<span class='yellow'>klingelt (" + elapsedText(ringSince) + ")</span>"
                                                              : "-") +
                "</td>";
        html += "</tr>";
    }
    html += "</tbody></table>";
    html += "<p id='wis-live-status' class='yellow'>Live-Anzeige verbindet…</p>";

    html += "<script>"
            "(function(){"
            "var st=document.getElementById('wis-live-status');"
            "var ws,reconn=2000;"
            "function connect(){"
            "ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws/wis');"
            "ws.onopen=function(){st.textContent='Live-Anzeige verbunden.';st.className='green';reconn=2000;};"
            "ws.onclose=function(){st.textContent='Live-Anzeige getrennt, erneuter Versuch...';st.className='yellow';setTimeout(connect,reconn);reconn=Math.min(reconn*1.5,15000);};"
            "ws.onerror=function(){st.textContent='Fehler bei der Live-Anzeige.';st.className='red';};"
            "ws.onmessage=function(e){"
            "try{"
            "var ring=JSON.parse(e.data);"
            "var byStation={};"
            "ring.forEach(function(r){if(r.active&&!r.internal)byStation[r.stationId]=r;});"
            "document.querySelectorAll('tr[data-station-id]').forEach(function(row){"
            "var cell=row.querySelector('.wis-ring');"
            "if(byStation[row.dataset.stationId]){cell.innerHTML=\"<span class='yellow'>klingelt</span>\";}"
            "else{cell.innerHTML='-';}"
            "});"
            "}catch(err){}"
            "};"
            "}"
            "connect();"
            "})();"
            "</script>";

    html += "</div>";
    sendHtml(res, html, "/wis");
}

void WelcomeIPSipWebPage::handlePair(WebRequest &req, WebResponse &res)
{
    (void)req;
    _module.pairing().enqueue(Wis::Pairing::CMD_PAIR);
    sendMetaRefresh(res, "Pairing angestoßen", "Weiterleitung auf die Statusseite in 3 Sekunden…");
}

void WelcomeIPSipWebPage::handleUnpair(WebRequest &req, WebResponse &res)
{
    (void)req;
    _module.pairing().enqueue(Wis::Pairing::CMD_UNPAIR);
    sendMetaRefresh(res, "Unpair angestoßen", "Der Portal-Eintrag bleibt bestehen und muss dort manuell entfernt werden.");
}

void WelcomeIPSipWebPage::handleWsConnect(int clientId, bool connected)
{
    if (connected) sendLiveSnapshot(clientId);
}

std::string WelcomeIPSipWebPage::buildRingJson(const Wis::LinkState &link) const
{
    std::string json = "[";
    for (uint8_t i = 0; i < Wis::kMaxOpenCalls; i++)
    {
        const auto &slot = link.ring[i];
        if (i > 0) json += ",";
        json += "{\"active\":";
        json += slot.active ? "true" : "false";
        json += ",\"internal\":";
        json += slot.internalCall ? "true" : "false";
        json += ",\"stationId\":\"";
        json += slot.stationId; // station_id is portal-assigned, plain alnum — no escaping needed
        json += "\"}";
    }
    json += "]";
    return json;
}

void WelcomeIPSipWebPage::sendLiveSnapshot(int clientId)
{
    Wis::LinkState link;
    _module.link().snapshot(link, link.seq - 1);
    const std::string json = buildRingJson(link);
    openknxNetwork.webserver.sendToClient("/ws/wis", clientId, json.c_str(), json.size());
}

void WelcomeIPSipWebPage::flushLiveUpdates()
{
    if (!openknxNetwork.webserver.hasClients("/ws/wis")) return;

    Wis::LinkState link;
    _module.link().snapshot(link, link.seq - 1);

    bool changed = false;
    for (uint8_t i = 0; i < Wis::kMaxOpenCalls; i++)
    {
        const auto &a = link.ring[i];
        const auto &b = _lastSentRing[i];
        if (a.active != b.active || a.internalCall != b.internalCall || strcmp(a.stationId, b.stationId) != 0)
        {
            changed = true;
            break;
        }
    }
    if (!changed) return;
    if (millis() - _lastLiveSentMs < 200UL) return; // throttle, wie beim Hue-Vorbild

    for (uint8_t i = 0; i < Wis::kMaxOpenCalls; i++) _lastSentRing[i] = link.ring[i];
    _lastLiveSentMs = millis();
    openknxNetwork.webserver.sendWebsocketMessage("/ws/wis", buildRingJson(link).c_str());
}

#endif // OPENKNX_WEBSERVER && KNX_IP
