
#ifndef MATROSKA_DLL_API_H
#define MATROSKA_DLL_API_H

#ifdef MATROSKA_STATIC_DEFINE
#  define MATROSKA_DLL_API
#  define MATROSKA_NO_EXPORT
#else
#  ifndef MATROSKA_DLL_API
#    ifdef matroska_EXPORTS
        /* We are building this library */
#      define MATROSKA_DLL_API 
#    else
        /* We are using this library */
#      define MATROSKA_DLL_API 
#    endif
#  endif

#  ifndef MATROSKA_NO_EXPORT
#    define MATROSKA_NO_EXPORT 
#  endif
#endif

#ifndef MATROSKA_DEPRECATED
#  define MATROSKA_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef MATROSKA_DEPRECATED_EXPORT
#  define MATROSKA_DEPRECATED_EXPORT MATROSKA_DLL_API MATROSKA_DEPRECATED
#endif

#ifndef MATROSKA_DEPRECATED_NO_EXPORT
#  define MATROSKA_DEPRECATED_NO_EXPORT MATROSKA_NO_EXPORT MATROSKA_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef MATROSKA_NO_DEPRECATED
#    define MATROSKA_NO_DEPRECATED
#  endif
#endif

#endif /* MATROSKA_DLL_API_H */
