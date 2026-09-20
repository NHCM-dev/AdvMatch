#ifndef ADVMATCH_ADVMATCH_H
#define ADVMATCH_ADVMATCH_H

#if defined(_WIN32)
#  if defined(advmatch_EXPORTS)
#    define ADVMATCH_API __declspec(dllexport)
#  else
#    define ADVMATCH_API __declspec(dllimport)
#  endif
#else
#  define ADVMATCH_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. This is infrastructure only; no matching API is defined yet. */
ADVMATCH_API int advmatch_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif
