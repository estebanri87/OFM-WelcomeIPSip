#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WelcomeIPSipChannel.h"
#include "WelcomeIPSipModule.h"

namespace
{
    // Plan: "Klingelsignal-Modus: Dauer während des Klingelns / Impuls" — no ETS
    // parameter for the pulse length was requested, so it is a fixed constant.
    const uint32_t kPulseMs = 1000;

    // Guards against one long button-press or a flaky KO re-triggering several RPC
    // calls in a row; no ETS parameter requested, fixed like kPulseMs.
    const uint32_t kUnlockBlockMs = 2000;
}

WelcomeIPSipChannel::WelcomeIPSipChannel(uint8_t index, WelcomeIPSipModule &module)
    : _module(module)
{
    _channelIndex = index;
}

const std::string WelcomeIPSipChannel::name()
{
    return "WelcomeIPSipChannel";
}

void WelcomeIPSipChannel::setup()
{
    _type = ParamWIS_CHType;
    if (_type == TYPE_NONE) return;

    _stationNumber = ParamWIS_CHStationNumber;
}

void WelcomeIPSipChannel::loop()
{
    if (_type == TYPE_NONE) return;

    Wis::DoorInfo door;
    if (!_module.doorForStation(_stationNumber, door))
    {
        if (!_warnedMissing)
        {
            _warnedMissing = true;
            logWarningP("Stationsnummer %u: keine passende Aussenstation in der Tuerliste", (unsigned)_stationNumber);
        }
        return;
    }
    _warnedMissing = false;
    if (_stationId.empty()) _stationId = door.stationId;

    if (!_nameSent && ParamWIS_WISUseStationName && !door.name.empty())
    {
        KoWIS_CHStationName.value(door.name.c_str(), DPT_String_8859_1);
        _nameSent = true;
    }

    Wis::LinkState link;
    _module.link().snapshot(link, link.seq - 1); // force a copy regardless of seq

    bool ringingNow = false;
    for (const auto &slot : link.ring)
        if (slot.active && !slot.internalCall && _stationId == slot.stationId)
        {
            ringingNow = true;
            break;
        }

    if (ParamWIS_WISRingMode == 1) // Dauer während des Klingelns
    {
        if (ringingNow != _ringActive)
        {
            _ringActive = ringingNow;
            KoWIS_CHRing.value(_ringActive, DPT_Switch);
        }
        return;
    }

    // Impuls: a fixed-length pulse on the rising edge, the SIP-level end is ignored.
    if (ringingNow && !_ringActive)
    {
        _ringActive = true;
        _pulseStartedAtMs = millis();
        if (_pulseStartedAtMs == 0) _pulseStartedAtMs = 1;
        KoWIS_CHRing.value(true, DPT_Switch);
    }
    if (!ringingNow) _ringActive = false; // re-arm once the SIP-level ring has ended
    if (_pulseStartedAtMs && delayCheck(_pulseStartedAtMs, kPulseMs))
    {
        _pulseStartedAtMs = 0;
        KoWIS_CHRing.value(false, DPT_Switch);
    }
}

bool WelcomeIPSipChannel::debounceUnlock(uint8_t lockIndex)
{
    uint32_t &last = _lastUnlockMs[lockIndex];
    if (last && !delayCheck(last, kUnlockBlockMs)) return false;
    last = millis();
    if (last == 0) last = 1; // 0 means "never triggered", see delayCheck's convention
    return true;
}

void WelcomeIPSipChannel::processInputKo(GroupObject &ko)
{
    if (_type == TYPE_NONE) return;
    const int index = WIS_KoCalcIndex(ko.asap());
    if (index != WIS_KoCHUnlock && index != WIS_KoCHUnlock2) return;
    // DPST-1-17 (Auslöser), same as the module-wide "Pairing starten" KO — any received
    // telegram triggers it, there is no level to check.

    Wis::DoorInfo door;
    if (!_module.doorForStation(_stationNumber, door))
    {
        logWarningP("Tueroeffner: Stationsnummer %u: keine passende Aussenstation in der Tuerliste",
                   (unsigned)_stationNumber);
        return;
    }

    const bool isSecondLock = index == WIS_KoCHUnlock2;
    const std::string &contactId = isSecondLock ? door.secondUnlockAddress : door.address;
    if (contactId.empty())
    {
        logWarningP("Zweitschloss: Station %u hat kein Zweitschloss konfiguriert", (unsigned)_stationNumber);
        return;
    }
    if (!debounceUnlock(isSecondLock ? 1 : 0)) return;

    if (!_module.xmppLink().enqueue(Wis::XmppLink::CMD_UNLOCK, contactId, door.stationId, isSecondLock ? 2 : 1))
        logWarningP("Tueroeffner: Anfrage verworfen (Link noch nicht bereit?)");
}

#endif // OPENKNX_WELCOMEIP_SIP
