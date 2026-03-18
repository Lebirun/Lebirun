#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

#define WOLFSSL_GENERAL_ALIGNMENT 4
#define SINGLE_THREADED
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_USER_IO
#define NO_FILESYSTEM
#define NO_WRITEV
#define NO_MAIN_DRIVER
#define WOLFSSL_NO_SOCK
#define NO_DEV_RANDOM
#define NO_WOLFSSL_DIR
#define NO_SIGNAL

#define WOLFSSL_TLS13
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_SNI

#define SIZEOF_LONG      8
#define SIZEOF_LONG_LONG 8

#define USE_FAST_MATH
#define TFM_TIMING_RESISTANT

#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define HAVE_AESGCM
#define HAVE_AESCCM
#define HAVE_CHACHA
#define HAVE_POLY1305
#define HAVE_HKDF
#define HAVE_FFDHE_2048
#define WC_RSA_PSS
#define WC_RSA_BLINDING
#define WOLFSSL_DH_CONST

#define NO_DSA
#define NO_RC4
#define NO_HC128
#define NO_RABBIT
#define NO_PSK
#define NO_MD4
#define NO_DES3
#define NO_OLD_TLS
#define NO_OLD_EXTRA
#define NO_PWDBASED
#define NO_PKCS8
#define NO_PKCS12

#define WOLFSSL_CERT_GEN
#define WOLFSSL_ASN_TEMPLATE

#define CUSTOM_RAND_GENERATE_SEED kernel_wolfssl_seed
#define CUSTOM_RAND_GENERATE_BLOCK kernel_wolfssl_random_block

#define XMALLOC_USER

#define HAVE_TIME_T_TYPE
#define TIME_OVERRIDES
#define USE_WOLF_TM

#define NO_STDIO_FILESYSTEM
#define WOLFSSL_NO_ABORT

#define XPRINTF kernel_wolfssl_printf

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef long time_t;

int kernel_wolfssl_seed(unsigned char *output, unsigned int sz);
int kernel_wolfssl_random_block(unsigned char *output, unsigned int sz);
int kernel_wolfssl_printf(const char *fmt, ...);

#endif
