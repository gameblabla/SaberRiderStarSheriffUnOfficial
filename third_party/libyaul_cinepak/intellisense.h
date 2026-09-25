// We can't replace the content of this file because it is part of the
// toolchain so we just ignore it :)
#ifdef __INTELLISENSE__
#  ifndef _LIB_SYS_CDEFS_H_
#    define _LIB_SYS_CDEFS_H_
#  endif
#endif

#ifdef __INTELLISENSE__
#   undef __packed
#  define __packed
#   undef __always_inline
#  define __always_inline
#   undef __STDC_HOSTED__
#  define __STDC_HOSTED__ 0
#   undef __aligned(X)
#  define __aligned(X)
#   undef __noinline
#  define __noinline
#   undef __no_reorder
#  define __no_reorder
#   undef __noreturn
#  define __noreturn
#   undef __hidden
#  define __hidden
#   undef __uncached_function
#  define __uncached_function
#   undef __unused
#  define __unused
#   undef __used
#  define __used
#   undef __weak
#  define __weak
#   undef __printflike
#  define __printflike(X,Y)
#   undef __section
#  define __section(X)
#   undef __may_alias
#  define __may_alias
#   undef __asm__
#  define __asm__ __asm
#   undef __attribute__
#  define __attribute__(X)
#   undef FORCE_INLINE
#  define FORCE_INLINE
#   undef NO_INLINE
#  define NO_INLINE
#  ifdef __cplusplus
#    define __BEGIN_DECLS   extern "C" {
#    define __END_DECLS     }
#  else
#    define __BEGIN_DECLS
#    define __END_DECLS
#  endif
#else
#  define FORCE_INLINE __attribute__((always_inline)) inline
#  define NO_INLINE __attribute__((noinline))
#endif
