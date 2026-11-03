import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.springframework.boot.gradle.plugin.SpringBootPlugin

plugins {
    kotlin("jvm") version "2.3.21"
    kotlin("plugin.spring") version "2.3.21"
    kotlin("plugin.serialization") version "2.3.21"
    id("org.springframework.boot") version "4.1.1"
}

group = "challenge"
version = "1.0.0"

java {
    toolchain { languageVersion = JavaLanguageVersion.of(25) }
}

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_25
        freeCompilerArgs.addAll("-Xjsr305=strict", "-Xannotation-default-target=param-property")
    }
}

repositories {
    mavenCentral()
}

dependencies {
    implementation(platform(SpringBootPlugin.BOM_COORDINATES))
    implementation("org.springframework.boot:spring-boot-starter-webmvc") {
        // kotlinx.serialization is the only JSON library; keeping Jackson out guarantees it handles every body.
        exclude(group = "org.springframework.boot", module = "spring-boot-starter-jackson")
        // No WebSockets: without this jar Tomcat doesn't add its WebSocket filter to every request.
        exclude(group = "org.apache.tomcat.embed", module = "tomcat-embed-websocket")
    }
    implementation("org.springframework.boot:spring-boot-starter-kotlinx-serialization-json")
    implementation("org.springframework.boot:spring-boot-starter-jdbc")
    implementation("org.xerial:sqlite-jdbc")
    // No kotlin-reflect, on purpose: when it is present, Spring MVC calls every Kotlin handler through
    // KFunction.callBy (and its soft-referenced metadata caches) instead of plain Method.invoke.

    testImplementation("org.springframework.boot:spring-boot-starter-test")
    testImplementation("org.jetbrains.kotlin:kotlin-test-junit5")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

tasks.test {
    useJUnitPlatform()
}
