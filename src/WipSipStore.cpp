#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipSipStore.h"
#include <Preferences.h>

namespace Wis
{
    namespace
    {
        const char *const kNvsNamespace = "wis-pairing";
        const char *const kGuardNamespace = "wis-guard";
    }

    bool Store::load(PersistedPairing &out)
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, true)) return false;

        out.phase = (Phase)prefs.getUChar("phase", (uint8_t)Phase::NONE);
        out.configHash = prefs.getString("cfgHash", "").c_str();
        out.portalUrl = prefs.getString("portalUrl", "").c_str();
        out.clientName = prefs.getString("clientName", "").c_str();
        out.privateKeyPem = prefs.getString("privKeyPem", "").c_str();
        out.certPem = prefs.getString("certPem", "").c_str();
        out.fingerprintSha1 = prefs.getString("sha1", "").c_str();
        out.gruu = prefs.getString("gruu", "").c_str();
        out.sipUsername = prefs.getString("sipUser", "").c_str();
        out.ownPortalUuid = prefs.getString("ownUuid", "").c_str();
        out.gatewayUuid = prefs.getString("gwUuid", "").c_str();
        out.gatewayName = prefs.getString("gwName", "").c_str();
        out.integrityEight = prefs.getString("intEight", "").c_str();
        out.integrityDisplay = prefs.getString("intDisp", "").c_str();
        out.sipPassword = prefs.getString("sipPass", "").c_str();
        out.sipDomain = prefs.getString("sipDomain", "").c_str();
        out.doorsJson = prefs.getString("doorsJson", "").c_str();

        prefs.end();
        return out.phase != Phase::NONE;
    }

    void Store::saveKey(const std::string &privateKeyPem)
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, false)) return;
        prefs.putString("privKeyPem", privateKeyPem.c_str());
        prefs.putUChar("phase", (uint8_t)Phase::KEY);
        prefs.end();
    }

    void Store::saveCert(const std::string &certPem, const std::string &portalUrl, const std::string &clientName)
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, false)) return;
        prefs.putString("certPem", certPem.c_str());
        prefs.putString("portalUrl", portalUrl.c_str());
        prefs.putString("clientName", clientName.c_str());
        prefs.putUChar("phase", (uint8_t)Phase::CERT);
        prefs.end();
    }

    void Store::saveDiscovered(const std::string &fingerprintSha1, const std::string &gruu, const std::string &sipUsername,
                                const std::string &ownPortalUuid, const std::string &gatewayUuid, const std::string &gatewayName)
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, false)) return;
        prefs.putString("sha1", fingerprintSha1.c_str());
        prefs.putString("gruu", gruu.c_str());
        prefs.putString("sipUser", sipUsername.c_str());
        prefs.putString("ownUuid", ownPortalUuid.c_str());
        prefs.putString("gwUuid", gatewayUuid.c_str());
        prefs.putString("gwName", gatewayName.c_str());
        prefs.putUChar("phase", (uint8_t)Phase::DISCOVERED);
        prefs.end();
    }

    void Store::saveConnectSent(const std::string &integrityEight, const std::string &integrityDisplay)
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, false)) return;
        prefs.putString("intEight", integrityEight.c_str());
        prefs.putString("intDisp", integrityDisplay.c_str());
        prefs.putUChar("phase", (uint8_t)Phase::CONNECT_SENT);
        prefs.end();
    }

    void Store::saveAclReceived(const std::string &sipPassword, const std::string &sipDomain, const std::string &doorsJson)
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, false)) return;
        prefs.putString("sipPass", sipPassword.c_str());
        prefs.putString("sipDomain", sipDomain.c_str());
        prefs.putString("doorsJson", doorsJson.c_str());
        prefs.putUChar("phase", (uint8_t)Phase::ACL_RECEIVED);
        prefs.end();
    }

    void Store::savePaired(const std::string &configHash)
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, false)) return;
        prefs.putString("cfgHash", configHash.c_str());
        prefs.putUChar("phase", (uint8_t)Phase::PAIRED);
        prefs.end();
    }

    void Store::clear()
    {
        Preferences prefs;
        if (!prefs.begin(kNvsNamespace, false)) return;
        prefs.clear();
        prefs.end();
    }

    uint32_t Store::lastCertRequestEpoch()
    {
        Preferences prefs;
        if (!prefs.begin(kGuardNamespace, true)) return 0;
        const uint32_t v = prefs.getUInt("certAt", 0);
        prefs.end();
        return v;
    }

    void Store::markCertRequested(uint32_t nowEpoch)
    {
        Preferences prefs;
        if (!prefs.begin(kGuardNamespace, false)) return;
        prefs.putUInt("certAt", nowEpoch);
        prefs.end();
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
