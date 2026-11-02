
plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(ktorLibs.plugins.ktor)
    alias(libs.plugins.kotlin.serialization)
}

group = "dev.noamm9.velox"
version = "2026.10.1"

application {
    mainClass = "dev.noamm9.velox.MainKt"
}

kotlin {
    jvmToolchain(25)
}

// pulled in by Ktor but unused here; without it typeOf() stays in the stdlib and the native image analyses far less code
configurations.runtimeClasspath {
    exclude(group = "org.jetbrains.kotlin", module = "kotlin-reflect")
}

dependencies {
    implementation(ktorLibs.serialization.kotlinx.json)
    implementation(ktorLibs.server.cio)
    implementation(ktorLibs.server.contentNegotiation)
    implementation(ktorLibs.server.core)
    implementation(ktorLibs.server.statusPages)

    implementation(libs.slf4j.simple)

    implementation(libs.dotenv)

    implementation(libs.sqlite)
}
