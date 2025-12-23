#ifndef _FEATURES_H
#define _FEATURES_H 1

#ifdef _GNU_SOURCE
#define _POSIX_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _BSD_SOURCE 1
#define _SVID_SOURCE 1
#define _ATFILE_SOURCE 1
#define _DEFAULT_SOURCE 1
#endif

#ifdef _POSIX_SOURCE
#define _POSIX_C_SOURCE 1
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#define __GLIBC__ 2
#define __GLIBC_MINOR__ 17

#define __GLIBC_PREREQ(maj, min) \
    ((__GLIBC__ << 16) + __GLIBC_MINOR__ >= ((maj) << 16) + (min))

#ifdef __GNUC__
#define __GNUC_PREREQ(maj, min) \
    ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#else
#define __GNUC_PREREQ(maj, min) 0
#endif

#define __USE_POSIX 1
#define __USE_POSIX2 1
#define __USE_POSIX199309 1
#define __USE_POSIX199506 1
#define __USE_XOPEN2K 1
#define __USE_XOPEN2K8 1
#define __USE_MISC 1
#define __USE_BSD 1
#define __USE_GNU 1

#ifndef __THROW
#ifdef __cplusplus
#define __THROW throw()
#define __THROWNL throw()
#define __NTH(fct) fct throw()
#else
#define __THROW __attribute__((__nothrow__))
#define __THROWNL __attribute__((__nothrow__))
#define __NTH(fct) __attribute__((__nothrow__)) fct
#endif
#endif

#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif

#endif
