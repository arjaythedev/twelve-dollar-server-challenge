import org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinVersion

plugins {
    kotlin("multiplatform")
}

kotlin {
    @OptIn(ExperimentalKotlinGradlePluginApi::class)
    compilerOptions {
        apiVersion.set(KotlinVersion.KOTLIN_2_4)
        languageVersion.set(KotlinVersion.KOTLIN_2_4)
        freeCompilerArgs.addAll("-opt-in=kotlin.RequiresOptIn", "-Xsuppress-version-warnings", "-Xexpect-actual-classes")
    }

    jvm {
        @OptIn(ExperimentalKotlinGradlePluginApi::class)
        compilerOptions { jvmTarget.set(JvmTarget.JVM_25) }
    }

    sourceSets {
        val commonMain by getting {
            dependencies { implementation(project(":trikeshed")) }
        }
    }
}

// One jar of the app and every runtime jar, flat in build/app: what start.sh puts on -cp.
val app = tasks.register<Sync>("app") {
    from(tasks.named("jvmJar"))
    from(configurations.named("jvmRuntimeClasspath"))
    into(layout.buildDirectory.dir("app"))
}

// The driver: SQLite 3.54.0 from its amalgamation, built here for this one-thread process.
val sqliteUrl = "https://sqlite.org/2026/sqlite-amalgamation-3540000.zip"
val sqliteSha3 = "7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93"
val sqliteDir = layout.buildDirectory.dir("sqlite")
val macHost = System.getProperty("os.name").startsWith("Mac")

val sqliteSource = tasks.register<JavaExec>("sqliteSource") {
    dependsOn("jvmMainClasses")
    mainClass.set("borg.trikeshed.feed.SqliteSourceKt")
    classpath(tasks.named("jvmJar"), configurations.named("jvmRuntimeClasspath"))
    args(sqliteUrl, sqliteSha3, sqliteDir.get().asFile.path)
    outputs.files(sqliteDir.map { it.file("sqlite3.c") }, sqliteDir.map { it.file("sqlite3.h") })
}

// compile.html#rcmd, plus no extension loading; SQLITE_THREADSAFE=0: one thread owns the connection.
// SQLITE_ENABLE_QPSG: binding a new post id no longer re-prepares the post statement (sqlite3ExprIsInteger).
val sqliteOptions = listOf(
    "-DSQLITE_DQS=0", "-DSQLITE_THREADSAFE=0", "-DSQLITE_DEFAULT_MEMSTATUS=0", "-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1",
    "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS", "-DSQLITE_MAX_EXPR_DEPTH=0", "-DSQLITE_OMIT_DECLTYPE", "-DSQLITE_OMIT_DEPRECATED",
    "-DSQLITE_OMIT_PROGRESS_CALLBACK", "-DSQLITE_OMIT_SHARED_CACHE", "-DSQLITE_USE_ALLOCA", "-DSQLITE_OMIT_LOAD_EXTENSION",
    "-DSQLITE_ENABLE_QPSG",
)

val sqliteLib = tasks.register<Exec>("sqliteLib") {
    dependsOn(sqliteSource)
    val dir = sqliteDir.get().asFile
    val lib = File(dir, if (macHost) "libsqlite3.dylib" else "libsqlite3.so")
    inputs.file(File(dir, "sqlite3.c"))
    outputs.file(lib)
    workingDir(dir)
    commandLine(listOf("cc", "-O2", "-fPIC", if (macHost) "-dynamiclib" else "-shared") + sqliteOptions +
        listOf("sqlite3.c", "-o", lib.path) + (if (macHost) emptyList() else listOf("-lm")))
}
