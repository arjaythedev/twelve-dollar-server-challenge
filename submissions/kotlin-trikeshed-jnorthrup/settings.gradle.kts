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
        maven {
            url = uri("https://raw.githubusercontent.com/jnorthrup/twelve-dollar-server-challenge-trikeshed/3aa4d11437fc88d7e9943b89d956b1d4e2288c71/")
            content { includeGroup("borg.trikeshed") }
        }
        mavenCentral()
    }
}

rootProject.name = "kotlin-trikeshed-jnorthrup"

include(":trikeshed", ":server", ":loadgen", ":components")
