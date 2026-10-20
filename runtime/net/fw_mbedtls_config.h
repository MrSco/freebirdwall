#ifndef FLIGHTWALL_SOURCE_TLS_CONFIG_H
#define FLIGHTWALL_SOURCE_TLS_CONFIG_H
#include "fw_mbedtls_symbols.h"
/* Dedicated source-built TLS library. Never use these headers with stock
 * binaries: every library translation unit must use this configuration. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
#define MBEDTLS_MEMORY_DEBUG
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_AES_C
/* Constant tables in flash: 8.4 KiB less RAM on the board; slightly slower AES. */
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
#define MBEDTLS_MPI_WINDOW_SIZE 1
#define MBEDTLS_MPI_MAX_SIZE 512
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
/* Preserve full-sized incoming records. A small application read buffer does
 * not establish a smaller record limit on an arbitrary Internet server. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#ifndef FW_TLS_TX_BYTES
#define FW_TLS_TX_BYTES 2048
#endif
#define MBEDTLS_SSL_OUT_CONTENT_LEN FW_TLS_TX_BYTES
/* No OS entropy fallback: the transport must supply a trusted RNG callback. */
#endif
