/* sb_mbedtls_config.h - mbedTLS 2.28.10 settings for the Xbox 360 (used via MBEDTLS_CONFIG_FILE).

   Starts from mbedTLS's default settings, then switches off what doesn't fit a 360 program:
     - things that call the operating system directly: we bring our own versions
         NET_C      network sockets   -> our own send/receive functions (with the 360's "insecure socket" 0x5801)
         TIMING_C   timers            -> not needed for TLS over TCP
         FS_IO      loading files     -> we read the certificate file ourselves and hand mbedTLS the bytes
         platform entropy (CryptGenRandom, which the 360 lacks) -> our program adds XNetRandom as random source
     - x86-only speed-ups (AESNI, PADLOCK), inline assembler
     - the PSA key storage parts and everything only a TLS SERVER needs */
#ifndef SB_MBEDTLS_CONFIG_H
#define SB_MBEDTLS_CONFIG_H

#include "mbedtls/config.h"

#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#define MBEDTLS_NO_PLATFORM_ENTROPY

#undef MBEDTLS_AESNI_C
#undef MBEDTLS_PADLOCK_C
#undef MBEDTLS_HAVE_ASM

#undef MBEDTLS_PSA_CRYPTO_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_CACHE_C

#undef MBEDTLS_SELF_TEST

#endif
