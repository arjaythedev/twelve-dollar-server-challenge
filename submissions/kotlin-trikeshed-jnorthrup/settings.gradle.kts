pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        // TrikeShed 20280e867, every target, published by TrikeShed's own build onto this fork's branch trikeshed-m2.
        // Verified at this commit, every file and every member of every jar, klib and zip: no ELF, Mach-O or PE file,
        // no ar archive, no .a/.so/.dylib/.dll/.exe/.kexe/.o/.node, no staticLibraries in any klib manifest. It holds
        // Kotlin IR (klib), JVM bytecode (jar), cinterop glue as LLVM bitcode, sources and metadata; this build compiles
        // all machine code on the box and links liburing and SQLite it builds itself (feedc.def staticLibraries).
        maven {
            url = uri("https://raw.githubusercontent.com/jnorthrup/twelve-dollar-server-challenge-trikeshed/c3a918c9e18c8d064269bde0f14c9e0147342365/")
            content { includeGroup("borg.trikeshed") }
        }
        mavenCentral()
    }
}

rootProject.name = "kotlin-trikeshed-jnorthrup"

include(":trikeshed", ":server", ":loadgen", ":components")
