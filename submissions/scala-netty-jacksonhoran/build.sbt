ThisBuild / scalaVersion := "3.9.0"

val nettyVersion = "4.2.18.Final"

lazy val writeClasspath = taskKey[Unit]("Writes the runtime classpath to target/classpath.txt for start.sh")

lazy val root = (project in file("."))
  .settings(
    name := "twelve-dollar-server",
    scalacOptions ++= Seq("-release", "25", "-deprecation", "-feature"),
    libraryDependencies ++= Seq(
      "io.netty" % "netty-transport" % nettyVersion,
      "io.netty" % "netty-transport-native-epoll" % nettyVersion classifier "linux-x86_64",
      "io.netty" % "netty-transport-native-epoll" % nettyVersion classifier "linux-aarch_64",
    ),
    writeClasspath := {
      val cp = (Runtime / fullClasspath).value.files.map(_.getAbsolutePath).mkString(java.io.File.pathSeparator)
      IO.write(target.value / "classpath.txt", cp)
    },
  )
