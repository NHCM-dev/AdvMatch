#include <jni.h>

#include "advmatch/advmatch.h"
#include "binding/AllocatorBenchmark.h"
#include <array>
#include <limits>
#include <vector>

namespace
{
    jlongArray JNICALL nativeAllocatorBenchmark(JNIEnv* env, jclass, jlong count)
    {
        if (count <= 0 || count > 100'000'000)
        {
            const auto type = env->FindClass("java/lang/IllegalArgumentException");
            if (type != nullptr)
            {
                env->ThrowNew(type, "objects must be in [1,100000000]");
                env->DeleteLocalRef(type);
            }

            return nullptr;
        }

        try
        {
            const auto pool = advmatch::binding::benchmark::runPool(static_cast<std::size_t>(count));
            const auto normal = advmatch::binding::benchmark::runNewDelete(static_cast<std::size_t>(count));
            const jlong values[] = {static_cast<jlong>(pool.seconds * 1'000'000'000), static_cast<jlong>(normal.seconds * 1'000'000'000), static_cast<jlong>(pool.checksum), static_cast<jlong>(normal.checksum)};
            const auto result = env->NewLongArray(4);
            if (result != nullptr)
            {
                env->SetLongArrayRegion(result, 0, 4, values);
            }

            return result;
        }
        catch (const std::exception&)
        {
            const auto type = env->FindClass("java/lang/OutOfMemoryError");
            if (type != nullptr)
            {
                env->ThrowNew(type, "Allocator benchmark could not allocate its workload");
                env->DeleteLocalRef(type);
            }

            return nullptr;
        }
    }

    jint JNICALL nativeAbiVersion(JNIEnv*, jclass)
    {
        return advmatch_abi_version();
    }

    void failure(JNIEnv* env, int status)
    {
        const char* type = status == -2 ? "java/util/concurrent/CancellationException" : "nhcm/advmatch/NativeException";
        const auto clazz = env->FindClass(type);
        if (clazz != nullptr)
        {
            // Native messages are ASCII; Unicode user values travel as UTF-8
            // byte packets, never through JNI's modified UTF-8 string API.
            if (status == -2)
            {
                env->ThrowNew(clazz, advmatch_last_error());
            }
            else
            {
                const auto constructor = env->GetMethodID(clazz, "<init>", "(ILjava/lang/String;)V");
                if (constructor == nullptr)
                {
                    // Older SDKs only expose the message constructor.
                    env->ExceptionClear();
                    env->ThrowNew(clazz, advmatch_last_error());
                }
                else
                {
                    const auto message = env->NewStringUTF(advmatch_last_error());
                    if (message != nullptr)
                    {
                        const auto error = env->NewObject(clazz, constructor, static_cast<jint>(status), message);
                        if (error != nullptr)
                        {
                            env->Throw(static_cast<jthrowable>(error));
                            env->DeleteLocalRef(error);
                        }

                        env->DeleteLocalRef(message);
                    }
                }
            }
            env->DeleteLocalRef(clazz);
        }
    }

    std::vector<uint8_t> packet(JNIEnv* env, jbyteArray data)
    {
        if (data == nullptr)
        {
            return {};
        }

        std::vector<uint8_t> bytes(static_cast<std::size_t>(env->GetArrayLength(data)));
        env->GetByteArrayRegion(data, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<jbyte*>(bytes.data()));
        return bytes;
    }

    jlong JNICALL nativeCreate(JNIEnv* env, jclass, jbyteArray config)
    {
        try
        {
            const auto bytes = packet(env, config);
            if (env->ExceptionCheck())
            {
                return 0;
            }

            uint64_t exchange = 0;
            const auto result = advmatch_exchange_create(bytes.data(), bytes.size(), &exchange);
            if (result != 0)
            {
                failure(env, result);
            }

            return static_cast<jlong>(exchange);
        }
        catch (...)
        {
            const auto clazz = env->FindClass("java/lang/OutOfMemoryError");
            if (clazz != nullptr)
            {
                env->ThrowNew(clazz, "cannot allocate native configuration packet");
                env->DeleteLocalRef(clazz);
            }

            return 0;
        }
    }

    jlong JNICALL nativeSubmit(JNIEnv* env, jclass, jlong exchange, jbyteArray command)
    {
        try
        {
            const auto bytes = packet(env, command);
            if (env->ExceptionCheck())
            {
                return 0;
            }

            uint64_t request = 0;
            const auto result = advmatch_exchange_submit(static_cast<uint64_t>(exchange), bytes.data(), bytes.size(), &request);
            if (result != 0)
            {
                failure(env, result);
            }

            return static_cast<jlong>(request);
        }
        catch (...)
        {
            const auto clazz = env->FindClass("java/lang/OutOfMemoryError");
            if (clazz != nullptr)
            {
                env->ThrowNew(clazz, "cannot allocate native command packet");
                env->DeleteLocalRef(clazz);
            }

            return 0;
        }
    }

    jbyteArray response(JNIEnv* env, int status, uint8_t* data, size_t size)
    {
        if (status < 0)
        {
            failure(env, status);
            advmatch_buffer_free(data);
            return nullptr;
        }

        if (status == 1)
        {
            return nullptr;
        }

        if (size > static_cast<size_t>(std::numeric_limits<jsize>::max()))
        {
            advmatch_buffer_free(data);
            const auto clazz = env->FindClass("java/lang/OutOfMemoryError");
            if (clazz != nullptr)
            {
                env->ThrowNew(clazz, "response exceeds Java array capacity");
                env->DeleteLocalRef(clazz);
            }

            return nullptr;
        }

        const auto result = env->NewByteArray(static_cast<jsize>(size));
        if (result != nullptr)
        {
            env->SetByteArrayRegion(result, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
        }

        advmatch_buffer_free(data);
        return result;
    }

    jbyteArray JNICALL nativePoll(JNIEnv* env, jclass, jlong exchange, jlong request)
    {
        uint8_t* data = nullptr;
        size_t size = 0;
        const auto status = advmatch_exchange_poll(static_cast<uint64_t>(exchange), static_cast<uint64_t>(request), &data, &size);
        return response(env, status, data, size);
    }

    jlongArray JNICALL nativeSubmitBatch(JNIEnv* env, jclass, jlong exchange, jbyteArray commands, jint count)
    {
        if (count <= 0 || count > 1024)
        {
            const auto type = env->FindClass("java/lang/IllegalArgumentException");
            if (type != nullptr)
            {
                env->ThrowNew(type, "batch count must be in [1,1024]");
                env->DeleteLocalRef(type);
            }

            return nullptr;
        }

        // Allocate the Java output before dispatch, so an allocation failure
        // cannot lose handles for commands already accepted by the engine.
        const auto result = env->NewLongArray(count);
        if (result == nullptr)
        {
            return nullptr;
        }

        try
        {
            const auto bytes = packet(env, commands);
            if (env->ExceptionCheck())
            {
                return nullptr;
            }

            std::vector<std::uint64_t> tickets(static_cast<std::size_t>(count));
            const auto status = advmatch_exchange_submit_batch(static_cast<std::uint64_t>(exchange), bytes.data(), bytes.size(), tickets.data(), tickets.size());
            if (status != 0)
            {
                failure(env, status);
                return nullptr;
            }

            std::array<jlong, 1024> values{};
            for (std::size_t index = 0; index < tickets.size(); ++index)
            {
                values[index] = static_cast<jlong>(tickets[index]);
            }

            env->SetLongArrayRegion(result, 0, count, values.data());
            return result;
        }
        catch (...)
        {
            const auto type = env->FindClass("java/lang/OutOfMemoryError");
            if (type != nullptr)
            {
                env->ThrowNew(type, "cannot allocate native batch packet");
                env->DeleteLocalRef(type);
            }

            return nullptr;
        }
    }

    jboolean JNICALL nativeAwaitResponses(JNIEnv* env, jclass, jlong exchange, jint timeoutMillis)
    {
        const auto status = advmatch_exchange_await(static_cast<std::uint64_t>(exchange), static_cast<unsigned int>(timeoutMillis));
        if (status < 0)
        {
            failure(env, status);
        }

        return status == 0;
    }

    jbyteArray JNICALL nativeDrain(JNIEnv* env, jclass, jlong exchange, jint limit)
    {
        std::uint8_t* data = nullptr;
        std::size_t size = 0;
        const auto status = advmatch_exchange_drain(static_cast<std::uint64_t>(exchange), static_cast<std::size_t>(limit), &data, &size);
        return response(env, status, data, size);
    }

    jbyteArray JNICALL nativeLoadSlot(JNIEnv* env, jclass, jlong exchange, jlong slot)
    {
        uint8_t* data = nullptr;
        size_t size = 0;
        const auto status = advmatch_slot_load(static_cast<uint64_t>(exchange), static_cast<uint64_t>(slot), &data, &size);
        return response(env, status, data, size);
    }

    jboolean JNICALL nativeCancel(JNIEnv* env, jclass, jlong exchange, jlong request)
    {
        int cancelled = 0;
        const auto status = advmatch_exchange_cancel(static_cast<uint64_t>(exchange), static_cast<uint64_t>(request), &cancelled);
        if (status != 0)
        {
            failure(env, status);
        }

        return cancelled != 0;
    }

    jlong JNICALL nativeClose(JNIEnv* env, jclass, jlong exchange)
    {
        uint64_t request = 0;
        const auto status = advmatch_exchange_close(static_cast<uint64_t>(exchange), &request);
        if (status != 0)
        {
            failure(env, status);
        }

        return static_cast<jlong>(request);
    }

    void JNICALL nativeRelease(JNIEnv* env, jclass, jlong exchange, jlong request)
    {
        const auto status = advmatch_request_release(static_cast<uint64_t>(exchange), static_cast<uint64_t>(request));
        if (status != 0)
        {
            failure(env, status);
        }
    }

    void JNICALL nativeReleaseSlot(JNIEnv* env, jclass, jlong exchange, jlong slot)
    {
        const auto status = advmatch_slot_release(static_cast<uint64_t>(exchange), static_cast<uint64_t>(slot));
        if (status != 0)
        {
            failure(env, status);
        }
    }

    void JNICALL nativeDestroy(JNIEnv* env, jclass, jlong exchange)
    {
        const auto status = advmatch_exchange_destroy(static_cast<uint64_t>(exchange));
        if (status != 0)
        {
            failure(env, status);
        }
    }

    bool registerClass(JNIEnv* env, const char* name, JNINativeMethod* methods, jint count)
    {
        const auto clazz = env->FindClass(name);
        if (clazz == nullptr)
        {
            // The legacy example and new SDK use different class paths.
            // Absence of one is expected; failures registering an existing
            // class are retained as Java exceptions and fail JNI_OnLoad.
            env->ExceptionClear();
            return false;
        }

        const auto result = env->RegisterNatives(clazz, methods, count);
        env->DeleteLocalRef(clazz);
        return result == JNI_OK;
    }
} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
    JNIEnv* env = nullptr;
    if (vm == nullptr || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr)
    {
        return JNI_ERR;
    }

    // Method names are registered explicitly; no Java_* named binding symbols.
    JNINativeMethod methods[] = {
        {const_cast<char*>("abiVersion"), const_cast<char*>("()I"), reinterpret_cast<void*>(&nativeAbiVersion)},
        {const_cast<char*>("create"), const_cast<char*>("([B)J"), reinterpret_cast<void*>(&nativeCreate)},
        {const_cast<char*>("submit"), const_cast<char*>("(J[B)J"), reinterpret_cast<void*>(&nativeSubmit)},
        {const_cast<char*>("poll"), const_cast<char*>("(JJ)[B"), reinterpret_cast<void*>(&nativePoll)},
        {const_cast<char*>("cancel"), const_cast<char*>("(JJ)Z"), reinterpret_cast<void*>(&nativeCancel)},
        {const_cast<char*>("close"), const_cast<char*>("(J)J"), reinterpret_cast<void*>(&nativeClose)},
        {const_cast<char*>("release"), const_cast<char*>("(JJ)V"), reinterpret_cast<void*>(&nativeRelease)},
        {const_cast<char*>("destroy"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(&nativeDestroy)},
        {const_cast<char*>("loadSlot"), const_cast<char*>("(JJ)[B"), reinterpret_cast<void*>(&nativeLoadSlot)},
        {const_cast<char*>("releaseSlot"), const_cast<char*>("(JJ)V"), reinterpret_cast<void*>(&nativeReleaseSlot)},
        {const_cast<char*>("submitBatch"), const_cast<char*>("(J[BI)[J"), reinterpret_cast<void*>(&nativeSubmitBatch)},
        {const_cast<char*>("drain"), const_cast<char*>("(JI)[B"), reinterpret_cast<void*>(&nativeDrain)},
        {const_cast<char*>("awaitResponses"), const_cast<char*>("(JI)Z"), reinterpret_cast<void*>(&nativeAwaitResponses)}
    };
    const bool current = registerClass(env, "nhcm/advmatch/internal/NativeBindings", methods, 10);
    if (env->ExceptionCheck())
    {
        return JNI_ERR;
    }

    // Optional additions preserve loading of older ABI-2 Java SDKs.
    if (current)
    {
        const auto clazz = env->FindClass("nhcm/advmatch/internal/NativeBindings");
        if (clazz != nullptr)
        {
            const auto batch = env->GetStaticMethodID(clazz, "submitBatch", "(J[BI)[J");
            if (batch == nullptr)
            {
                env->ExceptionClear();
            }
            else if (env->RegisterNatives(clazz, methods + 10, 3) != JNI_OK)
            {
                env->DeleteLocalRef(clazz);
                return JNI_ERR;
            }

            env->DeleteLocalRef(clazz);
        }
    }

    const bool legacy = registerClass(env, "org/advmatch/AdvMatch", methods, 1);
    if (env->ExceptionCheck())
    {
        return JNI_ERR;
    }

    JNINativeMethod benchmarks[] = {
        {const_cast<char*>("measureAllocator"), const_cast<char*>("(J)[J"), reinterpret_cast<void*>(&nativeAllocatorBenchmark)}
    };
    registerClass(env, "nhcm/advmatch/internal/BenchmarkNatives", benchmarks, 1);
    return !env->ExceptionCheck() && (current || legacy) ? JNI_VERSION_1_6 : JNI_ERR;
}
