#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include <cstdint>
#include <string>

// Pure crypto helpers for the MyBuildings pairing flow (RSA-2048 keypair/CSR, identity
// derivation from the signed certificate, the portal's integrity-code formula, and the
// RSA-PKCS#1-v1.5 decrypt of the SIP password from the ACL-update payload). No networking,
// no NVS — everything here is a pure function over PEM/text in, result out, so it can run
// on any task (the caller supplies the 30-120s of wall time the RSA operations need).
namespace Wis
{
    struct KeyAndCsr
    {
        bool ok = false;
        std::string error;
        std::string privateKeyPem;
        std::string csrPem;
    };

    // CN=commonName, RSA-2048, SHA-256 signature — matches portal.py's generate_keypair_and_csr.
    KeyAndCsr generateKeyAndCsr(const std::string &commonName);

    // Rebuilds a CSR for an *already generated* key (e.g. after a reboot between the KEY
    // and CERT phases): parses the persisted private key and signs a fresh CSR with it —
    // no RSA keygen, so this takes well under a second, and the identity stays the same
    // as the keypair is reused, not replaced.
    KeyAndCsr buildCsrFromKey(const std::string &privateKeyPem, const std::string &commonName);

    struct Identity
    {
        bool ok = false;
        std::string error;
        std::string fingerprintSha1; // 40 hex chars, uppercase
        std::string gruu;            // first 5 chars of fingerprintSha1, lowercase
        std::string sipUsername;     // "<portalUsername>_<gruu>"
        std::string ownPortalUuid;   // from the cert's SAN URI ".../api/client/<uuid>"
    };

    // Derives SIP username, GRUU and own_portal_uuid from the portal-signed certificate.
    Identity deriveIdentity(const std::string &certPem, const std::string &portalUsername);

    struct IntegrityCode
    {
        std::string eight;   // 8 hex chars, sent to the portal in the connect event
        std::string display; // "NNNN HHHH", shown to the user for confirmation in the app
    };

    // rand must be 0..9999; a random value is used when the caller passes -1.
    IntegrityCode computeIntegrityCode(const std::string &fingerprintSha1, int32_t rand = -1);

    // Decrypts an RSA-PKCS#1-v1.5 ciphertext (already base64-decoded by the caller) with
    // our own private key. Returns false with outError set on any failure; never partially
    // fills outPlain.
    bool rsaDecryptPkcs1(const std::string &privateKeyPem, const std::string &cipher, std::string &outPlain,
                         std::string &outError);

    // MD5 hex digest (lowercase), used for HTTP Digest auth and the staleness config hash.
    std::string md5Hex(const std::string &data);

    // SHA-256 hex digest (lowercase), used for the GEO portal lookup's username hash.
    std::string sha256Hex(const std::string &data);

    std::string base64Encode(const std::string &data);
    // Returns false (outBytes left empty) on malformed input.
    bool base64Decode(const std::string &in, std::string &outBytes);
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
