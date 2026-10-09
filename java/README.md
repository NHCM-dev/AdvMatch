# AdvMatch Java

JDK 21 library using Gradle, JNI RegisterNatives and CompletableFuture.
Source root: `advmatch/src/main`, package: `nhcm.advmatch`.

```powershell
.\gradlew.bat :advmatch:build :advmatch:runExample
```

Gradle builds native libraries using the repository release preset and embeds
the current OS/CPU libraries in `advmatch/build/libs/advmatch-0.0.1.jar`.
The Wrapper downloads Gradle on its first use; CMake obtains other configured
dependencies as usual. The Java module has no Maven runtime dependencies.

For an existing native build:

```powershell
.\gradlew.bat :advmatch:build -PskipNativeBuild -PnativeDir=D:/CppWorks/AdvMatch/build/release
```

Always close the Exchange (try-with-resources). Cancellation only prevents a
command that has not started. Native DLLs are platform-specific; use matching
64-bit JVM and libraries. Full API/lifecycle guide: `../docs/bindings.md`.

`CreateAsset/CreateUser/CreateSymbol` return `CreationResult.id()/handle()`.
`MatchingExchange` adds typed entity wrappers and managers (`assets`, `users`,
`symbols`, `accounts`, `orders`, `positions`, `books`, `matching`, `contracts`).
`runExample` demonstrates the object API.

All six C++ benchmark categories have Java mains and Gradle tasks:
`benchmarkTps`, `benchmarkScale`, `benchmarkPerpetual`, `benchmarkLiquidation`,
`benchmarkObserve`, `benchmarkBigAllocs`; `benchmarkSmoke` validates all six.
Arguments: `-PbenchmarkArgs="--orders 100000 --producers 4"`.
Guide: `../docs/java-benchmarks.md`.

## Runnable tests JAR

`build` also produces `advmatch/build/libs/advmatch-0.0.1-tests.jar`, containing
the SDK, integration tests, all benchmark mains and native libraries.
Build only this JAR and run it from the repository root:

```powershell
.\java\gradlew.bat -p java :advmatch:testJar
java -jar java/advmatch/build/libs/advmatch-0.0.1-tests.jar
```

The default entry point runs the integration tests. Select a benchmark with
its main class (arguments are passed directly, without `-PbenchmarkArgs`):

```powershell
java -cp java/advmatch/build/libs/advmatch-0.0.1-tests.jar nhcm.advmatch.benchmark.TpsBenchmark --orders 100000 --producers 4 --batch-size 256 --ring-size 32
java -cp java/advmatch/build/libs/advmatch-0.0.1-tests.jar nhcm.advmatch.benchmark.BenchmarkSmoke
```

Use `-PskipNativeBuild` to reuse existing native binaries when packaging.
Running requires Java 21 or later on the OS/CPU used to build the JAR;
native libraries are extracted automatically, so no Gradle or CMake is needed
on the machine running the tests.

High throughput command submission uses `Exchange.submitBatch` (up to 1024
typed commands), native batched response drain, slice decoding and completion
notification. `TransportOptions` configures response batches and bounds Java
outstanding work independently of native queue capacity. Keep the JAR and both
native libraries from the same build. Guide: `../docs/performance.md`.
