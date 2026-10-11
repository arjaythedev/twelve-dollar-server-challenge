
plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(ktorLibs.plugins.ktor)
    alias(libs.plugins.kotlin.serialization)
}

group = "dev.syorito-hatsuki"
version = "2026.10.1"

application {
    mainClass = "dev.syorito-hatsuki.MainKt"
}

kotlin {
    jvmToolchain(21)
}
dependencies {
    implementation(ktorLibs.serialization.kotlinx.json)
    implementation(ktorLibs.server.auth)
    implementation(ktorLibs.server.auth.jwt)
    implementation(ktorLibs.server.cio)
    implementation(ktorLibs.server.contentNegotiation)
    implementation(ktorLibs.server.core)
    implementation(ktorLibs.server.statusPages)

    implementation(libs.logback.classic)

    implementation(libs.dotenv)

    implementation(libs.hikari)
    implementation(libs.sqlite)
}
