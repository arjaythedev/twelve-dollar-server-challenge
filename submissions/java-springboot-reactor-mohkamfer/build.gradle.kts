plugins {
    java
    id("org.springframework.boot") version "4.1.1"
    id("io.spring.dependency-management") version "1.1.7"
}

group = "challenge"
version = "1.0.0"

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(25)
    }
}

repositories {
    mavenCentral()
}

dependencies {
    // Spring Boot's reactive web server on Reactor Netty; requests are routed to a raw Reactor Netty handler
    implementation("org.springframework.boot:spring-boot-starter-reactor-netty")
    // Native epoll transport for Reactor Netty (version managed by the Spring Boot BOM)
    implementation("io.netty:netty-transport-native-epoll::linux-x86_64")
    // SQLite itself is compiled from source by build.sh and called through the FFM API (Sqlite.java)
}

tasks.bootJar {
    archiveFileName = "app.jar"
}

tasks.jar {
    enabled = false
}
