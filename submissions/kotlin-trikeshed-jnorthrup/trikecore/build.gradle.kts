import org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinVersion

plugins {
    kotlin("multiplatform")
}

// The four TrikeShed files every target of the server needs (HMAC-SHA-256 for JWT, the known-schema JSON reader),
// read from the same subrepo as :trikeshed. They depend on nothing else, so the native server links only these.
val subrepo = rootProject.file("trikeshed/TrikeShed/src/commonMain/kotlin")
val files = listOf(
    "borg/trikeshed/job/Hmac.kt",
    "borg/trikeshed/job/Sha256Pure.kt",
    "borg/trikeshed/job/Sha512Pure.kt",
    "borg/trikeshed/parse/confix/KnownSchema.kt",
)
val nativeTargets = providers.gradleProperty("trike.targets").orNull.orEmpty().split(',').map { it.trim() }.toSet()

kotlin {
    @OptIn(ExperimentalKotlinGradlePluginApi::class)
    compilerOptions {
        apiVersion.set(KotlinVersion.KOTLIN_2_4)
        languageVersion.set(KotlinVersion.KOTLIN_2_4)
        freeCompilerArgs.addAll("-Xsuppress-version-warnings")
    }
    jvm {
        @OptIn(ExperimentalKotlinGradlePluginApi::class)
        compilerOptions { jvmTarget.set(JvmTarget.JVM_25) }
    }
    if ("linuxX64" in nativeTargets) linuxX64()
    sourceSets.getByName("commonMain") {
        kotlin.setSrcDirs(listOf(subrepo))
        kotlin.setIncludes(files)
    }
}
