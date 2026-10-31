/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file ml_x509_wrap.c
 * @brief WiCAN (2026-10-31, issue 1024): the TLS chain parser's tolerance
 *        for a certificate it cannot parse, restored for mbedTLS 4.
 *
 * Every public Tailscale DERP relay appends a self-signed Ed25519 "derpkey"
 * meta certificate to its TLS chain, as its LAST entry (the DERP protocol's
 * way of publishing the relay key). mbedTLS has no Ed25519 in X.509:
 * parsing that certificate ends in mbedtls_x509_get_sig_alg() with the
 * bare UNKNOWN_OID (-0x2100 in mbedTLS 4, whose error composition keeps
 * the lower code). The TLS client's chain parsers carry an "ignore a
 * certificate with an unknown algorithm" case for exactly that code, and
 * the TLS 1.2 one (ssl_parse_certificate_chain) then returns 0, but the
 * TLS 1.3 one (mbedtls_ssl_tls13_parse_certificate) returns its loop's
 * last `ret` from its exit path: a tolerated code on the last certificate
 * of the chain leaves the function as the handshake's error. The relays
 * negotiate TLS 1.3, so every relay handshake on v6 failed with
 * `TLS handshake failed: X509 - Requested OID is unknown` (bench
 * 2026-10-31: the parse code of the 359-byte meta certificate read
 * -0x2100 and the handshake failed in the same millisecond). Firmware 4.x
 * ran TLS 1.2 and never met it.
 *
 * The two parse entry points the TLS layer uses are wrapped at link time
 * (CMakeLists.txt, the ip4_routes pattern): a certificate that parses is
 * untouched, one that fails for any other reason still fails, and the
 * unknown-signature-algorithm outcome in a chain that already holds a
 * parsed certificate answers 0, which is what a tolerated skip leaves
 * behind (the parser has unlinked and freed the node it allocated for the
 * failed certificate). The skipped certificate takes no part in anything
 * after the parse: DERP runs with authmode NONE, and a verified
 * connection whose chain meets an unparseable intermediate fails its
 * verification exactly as it did on mbedTLS 3.6. A chain's first
 * certificate (the leaf) is never skipped. The first failed certificate
 * of a boot is reported at W with the raw parse code so a bench can read
 * it; later ones at D.
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "ml_x509";

static int s_reported;

static int map_chain_error(const mbedtls_x509_crt *chain, int ret, size_t buflen)
{
    if (ret == 0)
    {
        return 0;
    }

    /* the unknown-signature-algorithm outcome of the parse: the bare
       UNKNOWN_OID (mbedTLS 4's error composition keeps the lower code) or
       the pair, in a chain that already holds a parsed certificate (a
       chain's first certificate is never skipped: that is the leaf) */
    bool skip = (ret == MBEDTLS_ERR_X509_UNKNOWN_OID ||
                 ret == MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG + MBEDTLS_ERR_X509_UNKNOWN_OID ||
                 ret == MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG) &&
                chain != NULL && chain->version != 0;

    /* the first failed certificate of a boot at W with its raw code (a
       bench reads it), the rest at D */
    if (s_reported == 0)
    {
        s_reported = 1;
        ESP_LOGW(TAG, "a certificate in a server's chain did not parse "
                 "(%u bytes, code -0x%04x%s)", (unsigned)buflen, (unsigned)-ret,
                 skip ? ", unknown signature algorithm: skipped" : "");
    }
    else
    {
        ESP_LOGD(TAG, "a certificate in a server's chain did not parse "
                 "(%u bytes, code -0x%04x%s)", (unsigned)buflen, (unsigned)-ret,
                 skip ? ", unknown signature algorithm: skipped" : "");
    }

    /* 0, not the tolerated code: the parser has already unlinked and freed
       the node it allocated for the failed certificate, so the chain is
       exactly what it would be after a tolerated skip; and the TLS 1.3
       certificate parser returns its loop's last `ret` from its exit path,
       so a tolerated code on the LAST certificate of a chain (where a DERP
       relay puts its meta certificate) still failed the handshake. */
    return skip ? 0 : ret;
}

int __real_mbedtls_x509_crt_parse_der(mbedtls_x509_crt *chain, const unsigned char *buf,
                                      size_t buflen);
int __real_mbedtls_x509_crt_parse_der_nocopy(mbedtls_x509_crt *chain, const unsigned char *buf,
                                             size_t buflen);

int __wrap_mbedtls_x509_crt_parse_der(mbedtls_x509_crt *chain, const unsigned char *buf,
                                      size_t buflen)
{
    return map_chain_error(chain, __real_mbedtls_x509_crt_parse_der(chain, buf, buflen),
                           buflen);
}

int __wrap_mbedtls_x509_crt_parse_der_nocopy(mbedtls_x509_crt *chain, const unsigned char *buf,
                                             size_t buflen)
{
    return map_chain_error(chain,
                           __real_mbedtls_x509_crt_parse_der_nocopy(chain, buf, buflen),
                           buflen);
}
