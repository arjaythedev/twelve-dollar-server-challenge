plugins {
    application
}

repositories {
    mavenCentral()
}

dependencies {
    testImplementation(libs.junit.jupiter)

    testRuntimeOnly("org.junit.platform:junit-platform-launcher")

    implementation(enforcedPlatform(libs.helidon.bom))
    implementation(libs.helidon.webserver)
    implementation(libs.helidon.http.media.json)
    implementation(libs.helidon.json.binding)
    implementation(libs.helidon.http.media.json.binding)
    implementation(libs.sqlite.jdbc)

    annotationProcessor(enforcedPlatform(libs.helidon.bom))
    annotationProcessor(libs.helidon.codegen.apt)
    annotationProcessor(libs.helidon.service.codegen)
    annotationProcessor(libs.helidon.json.codegen)
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(27)
    }
}

application {
    mainClass = "com.dglynch.twelvedollar.App"
    applicationDefaultJvmArgs = listOf(
        "--show-version",
        "-XX:+PrintFlagsFinal",
        "-Xmx1280m",
        "-Xms1280m",
        "-XX:+AlwaysPreTouch",
        "-XX:+CrashOnOutOfMemoryError",
        "-XX:+HeapDumpOnOutOfMemoryError",
    )
}

tasks.withType<JavaCompile>().configureEach {
    options.compilerArgs.add("-Werror")
    options.compilerArgs.add("-Xlint:all,-processing")
}
tasks.named<Test>("test") {
    useJUnitPlatform()
}
