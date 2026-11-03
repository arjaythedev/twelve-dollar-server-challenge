import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm") version "2.4.21"
}

repositories { mavenCentral() }

dependencies {
    implementation("org.xerial:sqlite-jdbc:3.53.4.0")
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_21)
        freeCompilerArgs.add("-Xno-param-assertions")
        freeCompilerArgs.add("-Xno-call-assertions")
        freeCompilerArgs.add("-Xno-receiver-assertions")
    }
}
java { targetCompatibility = JavaVersion.VERSION_21 }

// build/lib/*.jar = app + runtime deps; start.sh runs `java -cp 'build/lib/*'`.
val dist by tasks.registering(Copy::class) {
    from(tasks.jar)
    from(configurations.runtimeClasspath)
    into(layout.buildDirectory.dir("lib"))
}
tasks.build { dependsOn(dist) }
