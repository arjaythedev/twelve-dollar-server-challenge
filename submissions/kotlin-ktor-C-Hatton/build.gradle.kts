plugins {
    kotlin("jvm") version "2.4.21"
    application
}

repositories { mavenCentral() }

val ktor = "3.6.0"
val netty = "4.2.17.Final" // the version ktor-server-netty 3.6.0 depends on

dependencies {
    implementation("io.ktor:ktor-server-core-jvm:$ktor")
    implementation("io.ktor:ktor-server-netty-jvm:$ktor")
    implementation("io.netty:netty-transport-native-epoll:$netty:linux-x86_64")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json-jvm:1.11.0")
    runtimeOnly("org.slf4j:slf4j-nop:2.0.17")
}

kotlin { jvmToolchain(25) }

application { mainClass.set("server.MainKt") }
