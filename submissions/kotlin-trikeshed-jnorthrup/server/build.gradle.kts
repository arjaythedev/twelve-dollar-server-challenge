import org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinVersion

plugins {
    kotlin("multiplatform")
}

val nativeTargets = providers.gradleProperty("trike.targets").orNull.orEmpty().split(',').map { it.trim() }.toSet()
val sqliteDir = layout.buildDirectory.dir("sqlite")
val liburingDir = layout.buildDirectory.dir("liburing")

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

    // -Ptrike.targets=linuxX64 (the same switch as the trikeshed module): the Kotlin/Native server over io_uring direct
    // descriptors, build/bin/linuxX64/feedReleaseExecutable/feed.kexe. Linux host only (liburing builds here).
    if ("linuxX64" in nativeTargets) {
        linuxX64 {
            compilerOptions { optIn.add("kotlinx.cinterop.ExperimentalForeignApi") }
            compilations.getByName("main").cinterops.create("feedc") {
                definitionFile.set(project.file("src/nativeInterop/cinterop/feedc.def"))
                // io_uring_interop: kernel headers (openat2.h, time_types.h) that Kotlin/Native's glibc 2.19 sysroot lacks.
                includeDirs(sqliteDir.get().asFile, liburingDir.get().dir("src/include").asFile,
                    rootProject.file("trikeshed/TrikeShed/src/linuxMain/resources/io_uring_interop"))
                extraOpts("-libraryPath", sqliteDir.get().asFile.path, "-libraryPath", liburingDir.get().dir("src").asFile.path)
                tasks.named(interopProcessingTaskName).configure { dependsOn("sqliteArchive", "liburingLib") }
            }
            binaries.executable("feed") { entryPoint = "borg.trikeshed.feed.main" }
        }
    }

    sourceSets {
        val commonMain by getting {
            dependencies { implementation(project(":trikeshed")) }
        }
        val jvmTest by getting {
            dependencies { implementation(kotlin("test")) }
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

// walFrames' gather: a run of up to WAL_GATHER appended WAL frames leaves in one pwritev() (frame header and page as
// separate iovecs) in place of two pwrite() calls per frame. Each edit must match exactly once, or the build stops.
val sqliteGather = tasks.register("sqliteGather") {
    dependsOn(sqliteSource)
    val dir = sqliteDir.get().asFile
    inputs.file(File(dir, "sqlite3.c"))
    outputs.file(File(dir, "sqlite3g.c"))
    doLast {
        fun String.once(old: String, new: String): String {
            val n = split(old).size - 1
            check(n == 1) { "sqliteGather: ${n}x <$old>" }
            return replace(old, new)
        }
        val helper = """
#if SQLITE_OS_UNIX
#include <sys/uio.h>
#define WAL_GATHER 128
/* TRIKESHED: a run of up to WAL_GATHER appended frames in one pwritev(); error codes follow unixWrite(). */
static int walGatherFlush(sqlite3_file *pFd, struct iovec *aIov, int nIov, i64 iOff){
  unixFile *pFile = (unixFile*)pFd;
  int i = 0;
  while( i<nIov ){
    ssize_t n = pwritev(pFile->h, &aIov[i], nIov-i, iOff);
    if( n<0 ){
      int e = errno;
      if( e==EINTR ) continue;
      if( e==ENOSPC ){ storeLastErrno(pFile, 0); return SQLITE_FULL; }
      storeLastErrno(pFile, e);
      return SQLITE_IOERR_WRITE;
    }
    if( n==0 ){ storeLastErrno(pFile, 0); return SQLITE_FULL; }
    iOff += n;
    while( i<nIov && (size_t)n>=aIov[i].iov_len ){ n -= (ssize_t)aIov[i].iov_len; i++; }
    if( i<nIov && n>0 ){
      aIov[i].iov_base = (char*)aIov[i].iov_base + n;
      aIov[i].iov_len -= (size_t)n;
    }
  }
  return SQLITE_OK;
}
#endif /* SQLITE_OS_UNIX */
"""
        val locals = """#if SQLITE_OS_UNIX
  struct iovec aGv[2*WAL_GATHER];  /* TRIKESHED: gathered headers and pages */
  u8 aGh[WAL_GATHER][WAL_FRAME_HDRSIZE];
  int nG = 0;                      /* Frames gathered, not yet written */
  i64 iGOff = 0;                   /* WAL offset of the first gathered frame */
  const int bGather = pWal->pWalFd->pMethods!=0
                   && pWal->pWalFd->pMethods->xWrite==unixWrite;
#endif
"""
        fun flush(indent: String) =
            "#if SQLITE_OS_UNIX\n${indent}if( nG ){ rc = walGatherFlush(pWal->pWalFd, aGv, 2*nG, iGOff); nG = 0; if( rc ) return rc; }\n#endif\n"
        val gather = """#if SQLITE_OS_UNIX
    if( bGather ){
      if( nG==0 ) iGOff = iOffset;
      walEncodeFrame(pWal, p->pgno, nDbSize, p->pData, aGh[nG]);
      aGv[2*nG].iov_base = aGh[nG];
      aGv[2*nG].iov_len = WAL_FRAME_HDRSIZE;
      aGv[2*nG+1].iov_base = p->pData;
      aGv[2*nG+1].iov_len = (size_t)szPage;
      nG++;
      rc = SQLITE_OK;
      if( nG==WAL_GATHER ){
        rc = walGatherFlush(pWal->pWalFd, aGv, 2*nG, iGOff);
        nG = 0;
      }
    }else
#endif
"""
        val src = File(dir, "sqlite3.c").readText()
        val s0 = src.indexOf("static int walFrames(\n")
        val s1 = src.indexOf("int sqlite3WalFrames(", s0)
        check(s0 >= 0 && s1 > s0) { "sqliteGather: walFrames not found" }
        val live = "  WalIndexHdr *pLive;             /* Pointer to shared header */\n"
        val overwrite = "        rc = sqlite3OsWrite(pWal->pWalFd, pData, szPage, iOff);\n"
        val one = "    rc = walWriteOneFrame(&w, p, nDbSize, iOffset);\n"
        val recksum = "  if( isCommit && pWal->iReCksum ){\n"
        val f = src.substring(s0, s1)
            .once(live, live + locals)
            .once(overwrite, flush("        ") + overwrite)
            .once(one, gather + one)
            .once(recksum, flush("  ") + recksum)
        val head = "static int walWriteOneFrame(\n"
        val out = (src.substring(0, s0) + f + src.substring(s1)).once(head, helper + "\n" + head)
        File(dir, "sqlite3g.c").writeText(out)
    }
}

// The KN server links SQLite statically: the same options, one object in libsqlite3.a, built from sqlite3g.c (the
// amalgamation with walFrames' gather, above). SQLITE_DISABLE_LFS: the
// amalgamation's _FILE_OFFSET_BITS=64 makes glibc >= 2.28 name fcntl fcntl64, which Kotlin/Native's glibc 2.19 link
// sysroot lacks; off_t is 64 bits on x86_64 either way.
tasks.register<Exec>("sqliteArchive") {
    dependsOn(sqliteGather)
    val dir = sqliteDir.get().asFile
    inputs.file(File(dir, "sqlite3g.c"))
    outputs.file(File(dir, "libsqlite3.a"))
    workingDir(dir)
    commandLine("sh", "-c", (listOf("cc", "-O2", "-fPIC", "-c", "-DSQLITE_DISABLE_LFS") + sqliteOptions + listOf("sqlite3g.c", "-o", "sqlite3.o")).joinToString(" ") +
        " && rm -f libsqlite3.a && ar rcs libsqlite3.a sqlite3.o && rm sqlite3.o")
}

// liburing from the TrikeShed subrepo's pinned source tree, configured and built here (static only).
val liburingCopy = tasks.register<Copy>("liburingCopy") {
    from(rootProject.file("trikeshed/TrikeShed/src/linuxMain/resources/liburing")) {
        include("configure", "Makefile", "Makefile.common", "Makefile.quiet", "liburing.spec", "*.pc.in", "src/**")
    }
    into(liburingDir)
}
tasks.register<Exec>("liburingLib") {
    dependsOn(liburingCopy)
    inputs.dir(rootProject.file("trikeshed/TrikeShed/src/linuxMain/resources/liburing/src"))
    outputs.file(liburingDir.map { it.file("src/liburing.a") })
    workingDir(liburingDir)
    commandLine("sh", "-c", "sh ./configure --use-libc > /dev/null && make library ENABLE_SHARED=0 -j2 > /dev/null")
}

// RingServerTest: the built libsqlite3 and the challenge's seed database and tokens.
tasks.withType<Test>().configureEach {
    dependsOn(sqliteLib)
    jvmArgs("--enable-native-access=ALL-UNNAMED")
    systemProperty("sqlite.lib", File(sqliteDir.get().asFile, if (macHost) "libsqlite3.dylib" else "libsqlite3.so").path)
    systemProperty("feed.seed", rootProject.file("../../seed").path)
    testLogging { showStandardStreams = true }
}
