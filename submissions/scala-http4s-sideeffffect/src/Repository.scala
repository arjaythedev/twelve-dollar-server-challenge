package feed

import cats.syntax.all.*
import doobie.*
import doobie.implicits.*
import io.circe.Encoder

final case class Post(id: Long, body: String, createdAt: String, author: String, likeCount: Long) derives Read

object Post:
  given Encoder[Post] =
    Encoder.forProduct5("id", "body", "created_at", "author", "like_count")(p =>
      (p.id, p.body, p.createdAt, p.author, p.likeCount)
    )

enum LikeResult:
  case Liked, AlreadyLiked, NoSuchPost

/** The SQL, one statement per request in the common case. These are the reference queries from SPEC.md. */
object Repository:
  private val postSelect =
    fr"""SELECT p.id, p.body, p.created_at, u.username,
                (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
           FROM posts p JOIN users u ON u.id = p.user_id"""

  val ping: ConnectionIO[Int] = sql"SELECT 1".query[Int].unique

  val feed: ConnectionIO[List[Post]] =
    (postSelect ++ fr"ORDER BY p.created_at DESC, p.id DESC LIMIT 20").query[Post].to[List]

  def post(id: Long): ConnectionIO[Option[Post]] =
    (postSelect ++ fr"WHERE p.id = $id").query[Post].option

  def createPost(user: User, body: String): ConnectionIO[Post] =
    sql"INSERT INTO posts (user_id, body) VALUES (${user.id}, $body) RETURNING id, created_at"
      .query[(Long, String)]
      .unique
      .map((id, createdAt) => Post(id, body, createdAt, user.username, 0))

  /** Inserts only if the post exists (foreign keys aren't enforced). The existence check runs on its own only when
    * nothing was inserted, to tell "already liked" from "no such post".
    */
  def like(user: User, postId: Long): ConnectionIO[LikeResult] =
    sql"""INSERT INTO likes (user_id, post_id)
          SELECT ${user.id}, $postId WHERE EXISTS (SELECT 1 FROM posts WHERE id = $postId)
          ON CONFLICT (user_id, post_id) DO NOTHING""".update.run.flatMap {
      case 0 =>
        sql"SELECT EXISTS (SELECT 1 FROM posts WHERE id = $postId)"
          .query[Boolean]
          .unique
          .map(if _ then LikeResult.AlreadyLiked else LikeResult.NoSuchPost)
      case _ => LikeResult.Liked.pure[ConnectionIO]
    }
