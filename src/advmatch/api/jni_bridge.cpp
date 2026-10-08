#include <jni.h>

#include "advmatch/advmatch.h"

namespace
{
    jint JNICALL nativeAbiVersion(JNIEnv*, jclass)
    {
        return advmatch_abi_version();
    }
} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
    JNIEnv* env = nullptr;
    if (vm == nullptr || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr)
    {
        return JNI_ERR;
    }

    // JNI_OnLoad uses the class loader that called System.loadLibrary.
    jclass clazz = env->FindClass("org/advmatch/AdvMatch");
    if (clazz == nullptr)
    {
        return JNI_ERR;
    }

    char methodName[] = "abiVersion";
    char methodSignature[] = "()I";
    JNINativeMethod methods[] = {{methodName, methodSignature, reinterpret_cast<void*>(&nativeAbiVersion)}};
    const jint result = env->RegisterNatives(clazz, methods, 1);
    env->DeleteLocalRef(clazz);
    return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
