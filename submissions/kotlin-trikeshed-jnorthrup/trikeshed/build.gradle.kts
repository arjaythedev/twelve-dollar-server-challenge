import org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi
import org.jetbrains.kotlin.gradle.ExperimentalWasmDsl
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinVersion
import org.jetbrains.kotlin.gradle.plugin.mpp.DisableCacheInKotlinVersion
import org.jetbrains.kotlin.gradle.plugin.mpp.KotlinNativeCacheApi
import org.jetbrains.kotlin.gradle.plugin.mpp.TestExecutable

plugins {
    kotlin("multiplatform")
    kotlin("plugin.serialization")
}

// Opt-in targets for the leaf permutation, -Ptrike.targets=linuxX64,linuxArm64,macosArm64,wasmWasi; the box builds the
// JVM target only. The native targets reach liblz4/liblzo2 through cinterop: -Ptrike.cinclude=<dir holding lz4.h and
// lzo/> (macosArm64 defaults to Homebrew's include dir), -Ptrike.clib=<dir holding liblz4.so and liblzo2.so> when
// a Linux target is linked on another host; -Ptrike.cinclude.<target> / -Ptrike.clib.<target> override per target.
val extraTargets = providers.gradleProperty("trike.targets").orNull.orEmpty().split(',').map { it.trim() }.toSet()
val cinclude = providers.gradleProperty("trike.cinclude").orNull
val clib = providers.gradleProperty("trike.clib").orNull

// TrikeShed linkage: this module compiles TrikeShed's own files, read from the git subrepo TrikeShed/ at the commit
// git records for it (build.sh reifies the subrepo when a checkout leaves it empty). sources.txt lists the paths;
// each source set reads src/<set>/kotlin and src/<set>/resources of the subrepo, restricted to the listed files.
val subrepo = file("TrikeShed")
val linked = file("sources.txt").readLines().map { it.trim() }.filter { it.isNotEmpty() && !it.startsWith("#") }
check(File(subrepo, "src").isDirectory) { "trikeshed/TrikeShed is not reified: build.sh (or tester.sh git) does that" }

kotlin {
    @OptIn(ExperimentalKotlinGradlePluginApi::class)
    compilerOptions {
        apiVersion.set(KotlinVersion.KOTLIN_2_4)
        languageVersion.set(KotlinVersion.KOTLIN_2_4)
        freeCompilerArgs.addAll(
            "-opt-in=kotlin.RequiresOptIn",
            "-opt-in=kotlin.ExperimentalUnsignedTypes",
            "-opt-in=kotlinx.cinterop.UnsafeNumber",
            "-opt-in=kotlinx.cinterop.ExperimentalForeignApi",
            "-Xsuppress-version-warnings",
            "-Xexpect-actual-classes",
            "-Xallow-kotlin-package",
        )
    }

    jvm {
        @OptIn(ExperimentalKotlinGradlePluginApi::class)
        compilerOptions { jvmTarget.set(JvmTarget.JVM_25) }
    }

    val natives = listOfNotNull(
        if ("linuxX64" in extraTargets) linuxX64() else null,
        if ("linuxArm64" in extraTargets) linuxArm64() else null,
        if ("macosArm64" in extraTargets) macosArm64() else null,
    )
    for (target in natives) {
        val include = providers.gradleProperty("trike.cinclude.${target.name}").orNull ?: cinclude
        val lib = providers.gradleProperty("trike.clib.${target.name}").orNull ?: clib
        target.compilations.getByName("main").cinterops.create("lz") {
            definitionFile.set(File(subrepo, "src/nativeInterop/cinterop/lz.def"))
            (include ?: "/opt/homebrew/include".takeIf { target.name == "macosArm64" })?.let { includeDirs(it) }
        }
        target.binaries.executable("leafPermutation") { entryPoint = "borg.trikeshed.tilting.leaf.main" }
        if (lib != null) target.binaries.all { linkerOpts("-L$lib") }
        // Debug test binaries link without Kotlin/Native's compiler caches: on the macOS host the 2.4.21-RC
        // macos_arm64 stdlib cache in ~/.konan held objects built for iOS (LC_BUILD_VERSION platform 2), which ld rejected.
        if (target.name == "macosArm64") target.binaries.withType<TestExecutable>().configureEach {
            @OptIn(KotlinNativeCacheApi::class)
            disableNativeCache(DisableCacheInKotlinVersion.`2_4_21`, "macos_arm64 stdlib cache objects built for iOS")
        }
    }

    if ("wasmWasi" in extraTargets) {
        @OptIn(ExperimentalWasmDsl::class)
        wasmWasi {
            nodejs()
            binaries.executable()
        }
    }

    sourceSets.configureEach {
        val kotlinFiles = linked.filter { it.startsWith("src/$name/kotlin/") }.map { it.removePrefix("src/$name/kotlin/") }
        val resourceFiles =
            linked.filter { it.startsWith("src/$name/resources/") }.map { it.removePrefix("src/$name/resources/") }
        kotlin.setSrcDirs(if (kotlinFiles.isEmpty()) emptyList() else listOf(File(subrepo, "src/$name/kotlin")))
        kotlin.setIncludes(kotlinFiles)
        resources.setSrcDirs(if (resourceFiles.isEmpty()) emptyList() else listOf(File(subrepo, "src/$name/resources")))
        resources.setIncludes(resourceFiles)
    }

    sourceSets {
        // The userspace ring, reactor TLS and Confix closure: the versions TrikeShed's own build pins.
        val commonMain by getting {
            dependencies {
                // Hmac, Sha256Pure, Sha512Pure and KnownSchema compile in :trikecore (the native server's whole need).
                api(project(":trikecore"))
                implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.11.0")
                implementation("org.jetbrains.kotlinx:kotlinx-serialization-core:1.11.0")
                implementation("org.jetbrains.kotlinx:kotlinx-datetime:0.8.0-0.6.x-compat")
            }
        }
        val commonTest by getting {
            dependencies {
                implementation(kotlin("test"))
                implementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.11.0")
            }
        }
        val nonPosixMain by creating { dependsOn(commonMain) }
        val jvmMain by getting { dependsOn(nonPosixMain) }
        if (natives.isNotEmpty()) {
            val nativeMain by creating { dependsOn(commonMain) }
            val posixMain by creating { dependsOn(nativeMain) }
            // lzNative{Main,Test}: the cinterop specialization and its main, shared by every native target.
            val lzNativeMain by creating { dependsOn(posixMain) }
            val lzNativeTest by creating { dependsOn(commonTest) }
            for (target in natives) {
                getByName("${target.name}Main") { dependsOn(lzNativeMain) }
                getByName("${target.name}Test") { dependsOn(lzNativeTest) }
            }
        }
        if ("wasmWasi" in extraTargets) {
            val wasmWasiMain by getting { dependsOn(nonPosixMain) }
        }
    }
}

// FFM specializations (liblz4, liblzo2, libcrypto) need native access; -Pleaf.corpus=<pages file> feeds the permutation.
tasks.withType<Test>().configureEach {
    jvmArgs("--enable-native-access=ALL-UNNAMED")
    providers.gradleProperty("leaf.corpus").orNull?.let { systemProperty("leaf.corpus", it) }
    testLogging { showStandardStreams = true }
}
