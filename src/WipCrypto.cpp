#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "WipCrypto.h"
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md5.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Wis
{
    namespace
    {
        // RSA-2048 key generation and the later RSA decrypt both call the RNG many
        // times in a tight loop (Miller-Rabin, blinding); on a single-core FreeRTOS
        // task that starves the watchdog. This wraps mbedtls_ctr_drbg_random and
        // yields every 64 calls, same idea as the plan's "vTaskDelay(1) alle N Aufrufe".
        struct YieldingRng
        {
            mbedtls_ctr_drbg_context *drbg;
            uint32_t calls = 0;
        };

        int rngWithYield(void *ctx, unsigned char *buf, size_t len)
        {
            auto *self = static_cast<YieldingRng *>(ctx);
            if ((++self->calls & 0x3F) == 0) vTaskDelay(1);
            return mbedtls_ctr_drbg_random(self->drbg, buf, len);
        }

        // Seeds a fresh ctr_drbg from mbedtls' platform entropy source, which on
        // ESP-IDF already draws from the hardware RNG (same mechanism WiFiClientSecure
        // relies on for TLS) — no custom entropy source needed.
        bool seedDrbg(mbedtls_entropy_context &entropy, mbedtls_ctr_drbg_context &drbg, std::string &outError)
        {
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&drbg);
            const char *pers = "wis-sip";
            const int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers));
            if (rc != 0)
            {
                char buf[80];
                mbedtls_strerror(rc, buf, sizeof(buf));
                outError = std::string("RNG seed failed: ") + buf;
                return false;
            }
            return true;
        }

        std::string toHexUpper(const unsigned char *data, size_t len)
        {
            static const char hex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(len * 2);
            for (size_t i = 0; i < len; i++)
            {
                out += hex[data[i] >> 4];
                out += hex[data[i] & 0x0F];
            }
            return out;
        }

        std::string toHexLower(const unsigned char *data, size_t len)
        {
            static const char hex[] = "0123456789abcdef";
            std::string out;
            out.reserve(len * 2);
            for (size_t i = 0; i < len; i++)
            {
                out += hex[data[i] >> 4];
                out += hex[data[i] & 0x0F];
            }
            return out;
        }

        std::string mbedtlsErrorString(int rc)
        {
            char buf[100];
            mbedtls_strerror(rc, buf, sizeof(buf));
            return std::string(buf);
        }

        // Signs a CSR for an already-set-up key. Shared by generateKeyAndCsr (fresh
        // key) and buildCsrFromKey (key parsed back from NVS).
        bool signCsr(mbedtls_pk_context &key, const std::string &commonName, YieldingRng &rng, std::string &outCsrPem,
                     std::string &outError)
        {
            mbedtls_x509write_csr csr;
            mbedtls_x509write_csr_init(&csr);
            mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
            mbedtls_x509write_csr_set_key(&csr, &key);
            const std::string subject = "CN=" + commonName;
            int rc = mbedtls_x509write_csr_set_subject_name(&csr, subject.c_str());
            std::vector<unsigned char> buf(4096);
            if (rc == 0) rc = mbedtls_x509write_csr_pem(&csr, buf.data(), buf.size(), rngWithYield, &rng);
            if (rc == 0) outCsrPem.assign((char *)buf.data());
            mbedtls_x509write_csr_free(&csr);
            if (rc != 0)
            {
                outError = "CSR generation failed: " + mbedtlsErrorString(rc);
                return false;
            }
            return true;
        }
    } // namespace

    KeyAndCsr generateKeyAndCsr(const std::string &commonName)
    {
        KeyAndCsr result;

        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        if (!seedDrbg(entropy, drbg, result.error))
        {
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
            return result;
        }
        YieldingRng rng{&drbg};

        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
        if (rc == 0) rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), rngWithYield, &rng, 2048, 65537);
        if (rc != 0)
        {
            result.error = "RSA keygen failed: " + mbedtlsErrorString(rc);
            mbedtls_pk_free(&key);
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
            return result;
        }

        std::vector<unsigned char> buf(4096);
        rc = mbedtls_pk_write_key_pem(&key, buf.data(), buf.size());
        if (rc == 0) result.privateKeyPem.assign((char *)buf.data());

        bool csrOk = false;
        if (rc == 0) csrOk = signCsr(key, commonName, rng, result.csrPem, result.error);

        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);

        if (rc != 0)
        {
            result.error = "private key export failed: " + mbedtlsErrorString(rc);
            return result;
        }
        if (!csrOk) return result; // error already set by signCsr
        result.ok = true;
        return result;
    }

    KeyAndCsr buildCsrFromKey(const std::string &privateKeyPem, const std::string &commonName)
    {
        KeyAndCsr result;

        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        if (!seedDrbg(entropy, drbg, result.error))
        {
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
            return result;
        }
        YieldingRng rng{&drbg};

        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        // +1: mbedtls_pk_parse_key needs the PEM including its trailing NUL terminator.
        int rc = mbedtls_pk_parse_key(&key, (const unsigned char *)privateKeyPem.c_str(), privateKeyPem.size() + 1,
                                       nullptr, 0, rngWithYield, &rng);
        if (rc != 0)
        {
            result.error = "stored private key parse failed: " + mbedtlsErrorString(rc);
            mbedtls_pk_free(&key);
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
            return result;
        }

        result.privateKeyPem = privateKeyPem;
        result.ok = signCsr(key, commonName, rng, result.csrPem, result.error);

        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        return result;
    }

    Identity deriveIdentity(const std::string &certPem, const std::string &portalUsername)
    {
        Identity result;

        mbedtls_x509_crt cert;
        mbedtls_x509_crt_init(&cert);
        // +1: mbedtls_x509_crt_parse needs the PEM including its trailing NUL terminator.
        int rc = mbedtls_x509_crt_parse(&cert, (const unsigned char *)certPem.c_str(), certPem.size() + 1);
        if (rc != 0)
        {
            result.error = "certificate parse failed: " + mbedtlsErrorString(rc);
            mbedtls_x509_crt_free(&cert);
            return result;
        }

        unsigned char sha1[20];
        mbedtls_sha1(cert.raw.p, cert.raw.len, sha1);
        result.fingerprintSha1 = toHexUpper(sha1, sizeof(sha1));
        // gruu = first 5 chars of the *lowercase* hex fingerprint (portal.py: sha1[:5].lower()).
        result.gruu = result.fingerprintSha1.substr(0, 5);
        for (auto &c : result.gruu) c = (char)tolower((unsigned char)c);

        for (const mbedtls_x509_sequence *cur = &cert.subject_alt_names; cur != nullptr; cur = cur->next)
        {
            mbedtls_x509_subject_alternative_name san;
            if (mbedtls_x509_parse_subject_alt_name(&cur->buf, &san) != 0) continue;
            if (san.type == MBEDTLS_X509_SAN_UNIFORM_RESOURCE_IDENTIFIER)
            {
                const std::string uri((const char *)san.san.unstructured_name.p, san.san.unstructured_name.len);
                const size_t marker = uri.find("/api/client/");
                if (marker != std::string::npos)
                {
                    result.ownPortalUuid = uri.substr(marker + strlen("/api/client/"));
                    mbedtls_x509_free_subject_alt_name(&san);
                    break;
                }
            }
            mbedtls_x509_free_subject_alt_name(&san);
        }

        mbedtls_x509_crt_free(&cert);

        if (result.ownPortalUuid.empty())
        {
            result.error = "could not find own_portal_uuid in certificate SAN";
            return result;
        }
        result.sipUsername = portalUsername + "_" + result.gruu;
        result.ok = true;
        return result;
    }

    IntegrityCode computeIntegrityCode(const std::string &fingerprintSha1, int32_t rand)
    {
        IntegrityCode result;
        if (rand < 0 || rand > 9999) rand = (int32_t)(esp_random() % 10000);

        // "AA:BB:CC:..." from the 40 uppercase hex chars of the fingerprint.
        std::string decorated;
        decorated.reserve(59);
        for (size_t i = 0; i < fingerprintSha1.size(); i += 2)
        {
            if (i > 0) decorated += ':';
            decorated += fingerprintSha1.substr(i, 2);
        }

        char randStr[5];
        snprintf(randStr, sizeof(randStr), "%04d", (int)rand);
        const std::string hhhhInput = std::string(randStr) + ":" + decorated;
        const std::string hhhh = md5Hex(hhhhInput).substr(0, 4);

        std::string hhhhUpper = hhhh;
        for (auto &c : hhhhUpper) c = (char)toupper((unsigned char)c);

        result.eight = std::string(randStr) + hhhhUpper;
        result.display = std::string(randStr) + " " + hhhhUpper;
        return result;
    }

    bool rsaDecryptPkcs1(const std::string &privateKeyPem, const std::string &cipher, std::string &outPlain,
                         std::string &outError)
    {
        outPlain.clear();

        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        if (!seedDrbg(entropy, drbg, outError))
        {
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
            return false;
        }
        YieldingRng rng{&drbg};

        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        // +1: mbedtls_pk_parse_key needs the PEM including its trailing NUL terminator.
        int rc = mbedtls_pk_parse_key(&key, (const unsigned char *)privateKeyPem.c_str(), privateKeyPem.size() + 1,
                                       nullptr, 0, rngWithYield, &rng);
        if (rc != 0)
        {
            outError = "private key parse failed: " + mbedtlsErrorString(rc);
            mbedtls_pk_free(&key);
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
            return false;
        }

        std::vector<unsigned char> out(512); // RSA-2048 ciphertext decrypts to at most 256 bytes
        size_t outLen = 0;
        rc = mbedtls_pk_decrypt(&key, (const unsigned char *)cipher.data(), cipher.size(), out.data(), &outLen,
                                 out.size(), rngWithYield, &rng);

        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);

        if (rc != 0)
        {
            outError = "RSA decrypt failed: " + mbedtlsErrorString(rc);
            return false;
        }
        outPlain.assign((char *)out.data(), outLen);
        return true;
    }

    std::string md5Hex(const std::string &data)
    {
        unsigned char digest[16];
        mbedtls_md5((const unsigned char *)data.data(), data.size(), digest);
        return toHexLower(digest, sizeof(digest));
    }

    std::string sha256Hex(const std::string &data)
    {
        unsigned char digest[32];
        mbedtls_sha256((const unsigned char *)data.data(), data.size(), digest, 0);
        return toHexLower(digest, sizeof(digest));
    }

    std::string base64Encode(const std::string &data)
    {
        size_t outLen = 0;
        mbedtls_base64_encode(nullptr, 0, &outLen, (const unsigned char *)data.data(), data.size());
        std::vector<unsigned char> buf(outLen);
        if (mbedtls_base64_encode(buf.data(), buf.size(), &outLen, (const unsigned char *)data.data(), data.size()) != 0)
            return std::string();
        return std::string((char *)buf.data(), outLen);
    }

    bool base64Decode(const std::string &in, std::string &outBytes)
    {
        outBytes.clear();
        size_t outLen = 0;
        mbedtls_base64_decode(nullptr, 0, &outLen, (const unsigned char *)in.data(), in.size());
        if (outLen == 0) return !in.empty() ? false : true;
        std::vector<unsigned char> buf(outLen);
        if (mbedtls_base64_decode(buf.data(), buf.size(), &outLen, (const unsigned char *)in.data(), in.size()) != 0)
            return false;
        outBytes.assign((char *)buf.data(), outLen);
        return true;
    }
} // namespace Wis

#endif // OPENKNX_WELCOMEIP_SIP
