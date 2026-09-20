#include <jni.h>

#include "advmatch/advmatch.h"

namespace {

jint JNICALL native_abi_version(JNIEnv*, jclass) {
    return advmatch_abi_version();
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm == nullptr ||
        vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
        env == nullptr) {
        return JNI_ERR;
    }

    // JNI_OnLoad uses the class loader that called System.loadLibrary.
    jclass clazz = env->FindClass("org/advmatch/AdvMatch");
    if (clazz == nullptr) {
        return JNI_ERR;
    }

    char method_name[] = "abiVersion";
    char method_signature[] = "()I";
    JNINativeMethod methods[] = {
        {method_name, method_signature, reinterpret_cast<void*>(&native_abi_version)}
    };
    const jint result = env->RegisterNatives(clazz, methods, 1);
    env->DeleteLocalRef(clazz);
    return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
