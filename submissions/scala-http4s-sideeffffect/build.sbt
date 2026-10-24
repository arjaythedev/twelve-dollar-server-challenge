ThisBuild / scalaVersion := "3.10.0"

val http4sVersion = "0.23.38"
val doobieVersion = "1.0.0-RC12"
val circeVersion = "0.14.17"

lazy val writeClasspath = taskKey[Unit]("Writes the runtime classpath to target/classpath.txt for start.sh")

lazy val root = project
  .in(file("."))
  .settings(
    name := "feed-server",
    scalacOptions ++= Seq("-release", "27", "-deprecation", "-feature", "-Wunused:all"),
    libraryDependencies ++= Seq(
      "org.typelevel" %% "cats-effect" % "3.7.1",
      "co.fs2" %% "fs2-core" % "3.14.0",
      "org.http4s" %% "http4s-ember-server" % http4sVersion,
      "org.http4s" %% "http4s-dsl" % http4sVersion,
      "org.http4s" %% "http4s-circe" % http4sVersion,
      "io.circe" %% "circe-core" % circeVersion,
      "io.circe" %% "circe-parser" % circeVersion,
      "org.tpolecat" %% "doobie-core" % doobieVersion,
      "org.xerial" % "sqlite-jdbc" % "3.53.4.0"
    ),
    writeClasspath := {
      val converter = fileConverter.value
      val cp = (Runtime / fullClasspath).value
        .map(entry => converter.toPath(entry.data).toAbsolutePath)
        .mkString(java.io.File.pathSeparator)
      IO.write(baseDirectory.value / "target" / "classpath.txt", cp)
    }
  )
