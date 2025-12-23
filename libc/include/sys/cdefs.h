#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H 1

#define __lebirun_libc 1

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#ifndef __THROW
#ifdef __cplusplus
#define __THROW throw()
#define __THROWNL throw()
#define __NTH(fct) __LEAF_ATTR fct throw()
#define __NTHNL(fct) fct throw()
#else
#define __THROW __attribute__((__nothrow__, __leaf__))
#define __THROWNL __attribute__((__nothrow__))
#define __NTH(fct) __attribute__((__nothrow__, __leaf__)) fct
#define __NTHNL(fct) __attribute__((__nothrow__)) fct
#endif
#endif

#ifndef __GNUC_PREREQ
#if defined(__GNUC__) && defined(__GNUC_MINOR__)
#define __GNUC_PREREQ(maj, min) \
    ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#else
#define __GNUC_PREREQ(maj, min) 0
#endif
#endif

#ifndef __LEAF_ATTR
#if __GNUC_PREREQ(4, 6) && !defined(_LIBC)
#define __LEAF_ATTR __attribute__((__leaf__))
#else
#define __LEAF_ATTR
#endif
#endif

#ifndef __attribute_pure__
#define __attribute_pure__ __attribute__((__pure__))
#endif

#ifndef __attribute_const__
#define __attribute_const__ __attribute__((__const__))
#endif

#ifndef __attribute_malloc__
#define __attribute_malloc__ __attribute__((__malloc__))
#endif

#ifndef __attribute_noinline__
#define __attribute_noinline__ __attribute__((__noinline__))
#endif

#ifndef __attribute_used__
#define __attribute_used__ __attribute__((__used__))
#endif

#ifndef __attribute_deprecated__
#define __attribute_deprecated__ __attribute__((__deprecated__))
#endif

#ifndef __attribute_format_printf__
#define __attribute_format_printf__(a, b) __attribute__((__format__(__printf__, a, b)))
#endif

#ifndef __attribute_format_scanf__
#define __attribute_format_scanf__(a, b) __attribute__((__format__(__scanf__, a, b)))
#endif

#ifndef __attribute_nonnull__
#define __attribute_nonnull__(params) __attribute__((__nonnull__ params))
#endif

#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__ __attribute__((__warn_unused_result__))
#endif

#ifndef __restrict
#define __restrict restrict
#endif

#ifndef __restrict_arr
#define __restrict_arr restrict
#endif

#define __P(args) args
#define __PMT(args) args

#define __ptr_t void *
#define __long_double_t long double

#define __CONCAT(x, y) x ## y
#define __STRING(x) #x

#define __bos(ptr) __builtin_object_size(ptr, 0)
#define __bos0(ptr) __builtin_object_size(ptr, 0)

#define __glibc_clang_prereq(maj, min) 0

#ifndef __GLIBC_USE
#define __GLIBC_USE(F) 0
#endif

#define __USE_POSIX 1
#define __USE_POSIX2 1
#define __USE_POSIX199309 1
#define __USE_POSIX199506 1
#define __USE_XOPEN2K 1
#define __USE_XOPEN2K8 1
#define __USE_XOPEN 1
#define __USE_XOPEN_EXTENDED 1
#define __USE_MISC 1
#define __USE_ATFILE 1
#define __USE_GNU 1

#endif

