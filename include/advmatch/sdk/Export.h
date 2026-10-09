#ifndef ADVMATCH_SDK_EXPORT_H
#define ADVMATCH_SDK_EXPORT_H

#if defined(_WIN32) && defined(ADVMATCH_SDK_BUILD)
#define ADVMATCH_SDK_API __declspec(dllexport)
#elif defined(_WIN32) && defined(ADVMATCH_SDK_SHARED)
#define ADVMATCH_SDK_API __declspec(dllimport)
#elif defined(ADVMATCH_SDK_BUILD)
#define ADVMATCH_SDK_API __attribute__((visibility("default")))
#else
#define ADVMATCH_SDK_API
#endif

#endif // ADVMATCH_SDK_EXPORT_H
