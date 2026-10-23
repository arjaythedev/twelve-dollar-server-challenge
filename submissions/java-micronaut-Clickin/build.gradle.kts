plugins { java; application; id("com.gradleup.shadow") version "9.6.1" }
repositories { mavenCentral() }
java { toolchain { languageVersion.set(JavaLanguageVersion.of(25)) } }
val micronaut = "5.2.2"
dependencies {
    annotationProcessor(platform("io.micronaut.platform:micronaut-platform:$micronaut"))
    annotationProcessor("io.micronaut:micronaut-inject-java")
    annotationProcessor("io.micronaut.serde:micronaut-serde-processor")
    implementation(platform("io.micronaut.platform:micronaut-platform:$micronaut"))
    implementation("io.micronaut:micronaut-http-server-netty")
    implementation("io.micronaut.serde:micronaut-serde-jackson")
    implementation("org.xerial:sqlite-jdbc:3.53.4.0")
    runtimeOnly("org.slf4j:slf4j-simple:2.0.18")
    testImplementation("org.junit.jupiter:junit-jupiter:5.13.4")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher:1.13.4")
}
application { mainClass.set("challenge.Application") }
tasks.shadowJar {
    archiveFileName.set("twelve-dollar-micronaut.jar")
    mergeServiceFiles()
    filesMatching(listOf("META-INF/services/**", "META-INF/*.kotlin_module")) {
        duplicatesStrategy = DuplicatesStrategy.INCLUDE
    }
}
tasks.withType<JavaCompile>().configureEach {
    options.compilerArgs.addAll(listOf("-parameters", "-Amicronaut.processing.group=challenge", "-Amicronaut.processing.module=feed"))
}
tasks.test { useJUnitPlatform() }
dependencyLocking { lockAllConfigurations() }
