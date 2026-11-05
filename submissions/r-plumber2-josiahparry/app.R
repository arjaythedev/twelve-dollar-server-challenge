library(DBI)
library(yyjsonr)
library(plumber2)

# define the query strings
post_select <- "SELECT p.id, p.body, p.created_at, u.username as author, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) as like_count FROM posts p JOIN users u ON u.id = p.user_id"
post_query <- sprintf("%s where p.id = ?", post_select)
feed_sql <- sprintf(
  "%s order by p.created_at desc, p.id desc limit 20",
  post_select
)
insert_like <- "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING RETURNING post_id"


# load our auth handlers
source("auth.R")

# we use yyjsonr for serializing our json always
yyjsonr_serializing <- function(...) {
  function(x) {
    write_json_str(x, auto_unbox = TRUE)
  }
}

register_serializer("json", yyjsonr_serializing, "application/json")

# parse json with yyjsonr, marking bad json so the handler can return the error
yyjsonr_parsing <- function(...) {
  function(raw, directive) {
    rlang::try_fetch(
      read_json_raw(raw),
      error = \(cnd) structure(list(), class = "malformed_json")
    )
  }
}

register_parser("json", yyjsonr_parsing, "application/json")

# Open a new connection to a database
con <- dbConnect(
  RSQLite::SQLite(),
  Sys.getenv("SQLITE_PATH", unset = "seed/feed.db"),
  bigint = "integer"
)

# added for parity with the python example
dbExecute(con, "PRAGMA journal_mode = WAL")
dbExecute(con, "PRAGMA synchronous = NORMAL")
dbExecute(con, "PRAGMA busy_timeout = 5000")
dbExecute(con, "PRAGMA mmap_size = 1073741824")
dbExecute(con, "PRAGMA cache_size = -65536")
dbExecute(con, "PRAGMA temp_store = MEMORY")

start <- Sys.time()

health <- function(con) {
  res <- rlang::try_fetch(dbGetQuery(con, "select 1"), error = function(cnd) {
    list(
      status = "degraded",
      "db" = "unreachable",
      error = as.character(cnd)
    )
  })

  if (is.data.frame(res)) {
    list(
      "status" = "ok",
      "db" = "ok",
      uptime_s = as.integer(difftime(Sys.time(), start))
    )
  } else {
    res
  }
}


get_post <- function(con, id, response) {
  resp <- dbGetQuery(con, post_query, params = list(id))

  if (nrow(resp) == 0L) {
    response$status <- 404L
    return(list(error = "post not found"))
  }

  list(post = unclass(resp))
}

get_feed <- function(con) {
  list(posts = dbGetQuery(con, feed_sql))
}


create_post <- function(con, user_id, body, response) {
  resp <- dbGetQuery(
    con,
    "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
    params = list(user_id, body)
  )
  get_post(con, resp$id, response)
}

like_post <- function(request, response, id) {
  # authorize the user
  user <- authorize(request, response, secret = secret)
  if (is.null(user$sub)) {
    return(user)
  }
  # validate second according to spec
  id <- validate_post_id(id, response)
  if (is.list(id)) {
    return(id)
  }

  # try inserting the like, return an error if we one.
  res <- rlang::try_fetch(
    dbGetQuery(con, insert_like, params = list(user$sub, id)),
    error = \(err) {
      response$status <- 404L
      list(error = "post not found")
    }
  )

  if (!is.null(res$error)) {
    return(res)
  }

  # when we have no rows that could be a missing post OR the post doesn't exist
  if (nrow(res) == 0) {
    does_it_exist <- dbGetQuery(
      con,
      "SELECT 1 FROM posts WHERE id = ?",
      params = list(id)
    )

    if (nrow(does_it_exist) == 0L) {
      response$status <- 404L
      return(list(error = "post not found"))
    }

    response$status <- 200L
    return(list(liked = TRUE, already_liked = TRUE, post_id = id))
  }

  response$status <- 201L
  list(liked = TRUE, already_liked = FALSE, post_id = id)
}


validate_post_id <- function(id, response) {
  id <- rlang::try_fetch(as.numeric(id), warning = \(cnd) {
    response$status <- 400L
    list(error = "invalid post id")
  })

  if (is.list(id)) {
    return(id)
  }

  if (!rlang::is_integerish(id) || id < 1L) {
    response$status <- 400L
    return(list(error = "invalid post id"))
  }
  as.integer(id)
}

r <- api() |>
  # api_logger(logger_console()) |>
  api_get(
    "/health",
    \() {
      health(con)
    }
  ) |>
  api_get("/feed", \() {
    get_feed(con)
  }) |>
  api_get("/posts/<id>", \(response, id) {
    id <- validate_post_id(id, response)
    if (is.list(id)) {
      return(id)
    }
    get_post(con, as.integer(id), response)
  }) |>
  api_post("/posts", \(request, response, body) {
    user <- authorize(request, response, secret = secret)
    if (is.null(user$sub)) {
      return(user)
    }

    if (inherits(body, "malformed_json")) {
      response$status <- 400L
      return(list(error = "malformed JSON body"))
    }

    post_body <- body$body
    if (!rlang::is_string(post_body)) {
      response$status <- 400L
      return(list(error = "body is required"))
    }

    post_body <- trimws(post_body, whitespace = "[\\h\\v]")
    if (nchar(post_body) == 0L) {
      response$status <- 400L
      return(list(error = "body is required"))
    }

    if (nchar(post_body) > 500L) {
      response$status <- 400L
      return(list(error = "body must be at most 500 characters"))
    }

    res <- create_post(con, user$sub, post_body, response)
    response$status <- 201L
    res
  }) |>
  api_post("/posts/<id>/like", like_post) |>
  api_any("/*", \(response) {
    response$status <- 404L
    list(error = "not found")
  }) |>
  api_run(
    host = Sys.getenv("HOST", "127.0.0.1"),
    port = as.integer(Sys.getenv("PORT", "8080"))
  )
