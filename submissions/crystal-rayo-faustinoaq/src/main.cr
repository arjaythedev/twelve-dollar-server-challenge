require "./feed"

app = Rayo::App.new
app.get("/health") do |ctx|
  Feed.db.transaction(ctx)
  Feed.db.health(ctx)
end
app.get("/feed") do |ctx|
  Feed.db.transaction(ctx)
  Feed.db.feed(ctx)
end
app.get("/posts/:id") do |ctx|
  if id = Feed.id(ctx)
    Feed.db.transaction(ctx)
    Feed.db.post(ctx, id)
  end
end
app.post("/posts") do |ctx|
  if auth = Feed.auth(ctx)
    if json = Rayo::JSON::Reader.parse?(ctx.body)
      value = Feed.field(json, "body")
      if value && value.string?
        body, size = Feed.trim(value.string(Feed::BODY))
        if size == 0
          Feed.error(ctx, 400, "body is required")
        elsif size > 500
          Feed.error(ctx, 400, "body must be at most 500 characters")
        else
          Feed.db.transaction(ctx)
          Feed.db.create(ctx, auth[0], auth[1], body)
        end
      else
        Feed.error(ctx, 400, "body is required")
      end
    else
      Feed.error(ctx, 400, "malformed JSON body")
    end
  end
end
app.post("/posts/:id/like") do |ctx|
  if auth = Feed.auth(ctx)
    if id = Feed.id(ctx)
      Feed.db.transaction(ctx)
      Feed.db.like(ctx, auth[0], id)
    end
  end
end
app.after do |ctx|
  if ctx.status == 404 && ctx.output.len == 0
    ctx.json error: "not found"
  elsif ctx.status == 500
    ctx.output.clear
    ctx.json error: "internal server error"
  end
end
app.run host: ENV["HOST"]? || "0.0.0.0", port: (ENV["PORT"]? || "3000").to_i, database: nil
