import org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinVersion

plugins {
    kotlin("multiplatform")
}

// The load generator: bench/load.js's workload over the same central ring and TLS record path as the server.
kotlin {
    @OptIn(ExperimentalKotlinGradlePluginApi::class)
    compilerOptions {
        apiVersion.set(KotlinVersion.KOTLIN_2_4)
        languageVersion.set(KotlinVersion.KOTLIN_2_4)
        freeCompilerArgs.addAll("-opt-in=kotlin.RequiresOptIn", "-Xsuppress-version-warnings")
    }

    jvm {
        @OptIn(ExperimentalKotlinGradlePluginApi::class)
        compilerOptions { jvmTarget.set(JvmTarget.JVM_25) }
    }

    sourceSets {
        val jvmMain by getting {
            dependencies { implementation(project(":trikeshed")) }
        }
    }
}

// The generator's jars, flat in build/app.
tasks.register<Sync>("app") {
    from(tasks.named("jvmJar"))
    from(configurations.named("jvmRuntimeClasspath"))
    into(layout.buildDirectory.dir("app"))
}
