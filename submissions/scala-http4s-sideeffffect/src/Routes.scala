package feed

import cats.data.Kleisli
import cats.effect.IO
import io.circe.Json
import io.circe.syntax.*
import org.http4s.*
import org.http4s.circe.*
import org.http4s.dsl.io.*
import org.http4s.headers.Authorization

import scala.concurrent.duration.FiniteDuration

final class Routes(db: Database, jwt: Jwt, startedAt: FiniteDuration):
  import Routes.*

  private val routes: HttpRoutes[IO] = HttpRoutes.of[IO] {
    case GET -> Root / "health"     => health
    case GET -> Root / "feed"       => db.read(Repository.feed).flatMap(posts => Ok(Json.obj("posts" -> posts.asJson)))
    case GET -> Root / "posts" / id => withPostId(id)(getPost)
    case req @ POST -> Root / "posts"               => authenticated(req)(createPost(req, _))
    case req @ POST -> Root / "posts" / id / "like" =>
      authenticated(req)(user => withPostId(id)(like(user, _)))
  }

  /** Unknown paths and unexpected failures get the JSON bodies from SPEC.md. */
  val httpApp: HttpApp[IO] =
    Kleisli(req => routes(req).getOrElse(notFound).handleError(_ => internalError))

  private def health: IO[Response[IO]] =
    db.read(Repository.ping).attempt.flatMap {
      case Right(_) =>
        IO.monotonic.flatMap(now =>
          Ok(Json.obj("status" -> "ok".asJson, "db" -> "ok".asJson, "uptime_s" -> (now - startedAt).toSeconds.asJson))
        )
      case Left(e) =>
        ServiceUnavailable(
          Json.obj(
            "status" -> "degraded".asJson,
            "db" -> "unreachable".asJson,
            "error" -> Option(e.getMessage).getOrElse(e.toString).asJson
          )
        )
    }

  private def getPost(id: Long): IO[Response[IO]] =
    db.read(Repository.post(id)).flatMap {
      case Some(post) => Ok(Json.obj("post" -> post.asJson))
      case None       => error(Status.NotFound, "post not found")
    }

  private def createPost(req: Request[IO], user: User): IO[Response[IO]] =
    req.attemptAs[Json].value.flatMap {
      case Left(_)     => error(Status.BadRequest, "malformed JSON body")
      case Right(json) =>
        validBody(json) match
          case Left(message) => error(Status.BadRequest, message)
          case Right(body)   =>
            db.write(Repository.createPost(user, body)).flatMap(p => Created(Json.obj("post" -> p.asJson)))
    }

  private def like(user: User, postId: Long): IO[Response[IO]] =
    db.write(Repository.like(user, postId)).flatMap {
      case LikeResult.Liked        => Created(liked(alreadyLiked = false, postId))
      case LikeResult.AlreadyLiked => Ok(liked(alreadyLiked = true, postId))
      case LikeResult.NoSuchPost   => error(Status.NotFound, "post not found")
    }

  private def authenticated(req: Request[IO])(handle: User => IO[Response[IO]]): IO[Response[IO]] =
    IO.realTime.flatMap { now =>
      // The raw header: http4s's typed `Authorization` would parse (and reject) credentials before we see them.
      val header = req.headers.get(Authorization.name).map(_.head.value)
      jwt.verify(header, now.toSeconds) match
        case Right(user) => handle(user)
        case Left(e)     => error(Status.Unauthorized, e.message)
    }

object Routes:
  private val PostId = "[1-9][0-9]{0,15}".r

  private def withPostId(raw: String)(handle: Long => IO[Response[IO]]): IO[Response[IO]] =
    raw match
      case PostId() => handle(raw.toLong)
      case _        => error(Status.BadRequest, "invalid post id")

  /** `body` must be a string, non-empty and at most 500 characters once trimmed. */
  private def validBody(json: Json): Either[String, String] =
    json.hcursor.get[String]("body").toOption.map(_.strip) match
      case None | Some("")                                         => Left("body is required")
      case Some(body) if body.codePointCount(0, body.length) > 500 => Left("body must be at most 500 characters")
      case Some(body)                                              => Right(body)

  private def liked(alreadyLiked: Boolean, postId: Long): Json =
    Json.obj("liked" -> true.asJson, "already_liked" -> alreadyLiked.asJson, "post_id" -> postId.asJson)

  private def error(status: Status, message: String): IO[Response[IO]] =
    IO.pure(Response[IO](status).withEntity(Json.obj("error" -> message.asJson)))

  private val notFound: Response[IO] = Response[IO](Status.NotFound).withEntity(Json.obj("error" -> "not found".asJson))

  val internalError: Response[IO] =
    Response[IO](Status.InternalServerError).withEntity(Json.obj("error" -> "internal server error".asJson))
