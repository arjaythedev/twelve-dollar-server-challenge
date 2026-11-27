defmodule Db do
  @moduledoc """
  The only owner of the SQLite connection. SQLite builds each read response as one JSON column,
  so there is one native call per request. No response or result is cached; every read hits SQLite.
  """
  use GenServer

  @post "SELECT p.id,p.body,p.created_at,u.username AS author," <>
          "(SELECT count(*) FROM likes WHERE post_id=p.id) AS like_count FROM posts p JOIN users u ON u.id=p.user_id"
  @json "json_object('id',id,'body',body,'created_at',created_at,'author',author,'like_count',like_count)"

  def start_link(path), do: GenServer.start_link(__MODULE__, path, name: __MODULE__)
  def feed, do: GenServer.call(__MODULE__, :feed, :infinity)
  def post(id), do: GenServer.call(__MODULE__, {:post, id}, :infinity)
  def create(uid, body, name), do: GenServer.call(__MODULE__, {:create, uid, body, name}, :infinity)
  def like(uid, id), do: GenServer.call(__MODULE__, {:like, uid, id}, :infinity)
  def health, do: GenServer.call(__MODULE__, :health, :infinity)

  @impl true
  def init(path) do
    {:ok, db} = :esqlite3.open(String.to_charlist(path))

    :ok =
      :esqlite3.exec(db,
        "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;" <>
          "PRAGMA foreign_keys=ON; PRAGMA cache_size=-65536; PRAGMA mmap_size=268435456;")

    sql = [
      feed: "SELECT json_object('posts',json_group_array(#{@json})) FROM (#{@post} ORDER BY p.created_at DESC,p.id DESC LIMIT 20)",
      post: "SELECT json_object('post',#{@json}) FROM (#{@post} WHERE p.id=?)",
      create:
        "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING " <>
          "json_object('post',json_object('id',id,'body',body,'created_at',created_at,'author',?3,'like_count',0))",
      like:
        "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) " <>
          "ON CONFLICT(user_id,post_id) DO NOTHING",
      exists: "SELECT 1 FROM posts WHERE id=?",
      health: "SELECT 1"
    ]

    stmts =
      Map.new(sql, fn {name, q} ->
        {:ok, st} = :esqlite3.prepare(db, q, persistent: true)
        {name, st}
      end)

    {:ok, Map.put(stmts, :db, db)}
  end

  @impl true
  def handle_call(req, _from, st) do
    reply =
      try do
        run(req, st)
      catch
        _, _ -> if req == :health, do: {:error, "database query failed"}, else: {500, ~s({"error":"internal server error"})}
      end

    {:reply, reply, st}
  end

  defp run(:feed, st), do: read(:feed, [], st)
  defp run({:post, id}, st), do: read(:post, [id], st)

  defp run({:create, uid, body, name}, st) do
    with_stmt(:create, [uid, body, name], st, fn s ->
      [json] = :esqlite3.step(s)
      :"$done" = :esqlite3.step(s) # finish RETURNING so the row is committed before we answer
      {201, json}
    end)
  end

  defp run({:like, uid, id}, %{db: db} = st) do
    with_stmt(:like, [uid, id], st, fn s -> :"$done" = :esqlite3.step(s) end)

    case :esqlite3.changes(db) do
      1 -> liked(id, false)
      0 ->
        with_stmt(:exists, [id], st, fn s ->
          case :esqlite3.step(s) do
            [1] -> liked(id, true)
            :"$done" -> not_found()
          end
        end)
    end
  end

  defp run(:health, st) do
    with_stmt(:health, [], st, fn s -> [1] = :esqlite3.step(s); :ok end)
  end

  defp read(name, args, st) do
    with_stmt(name, args, st, fn s ->
      case :esqlite3.step(s) do
        [json] when is_binary(json) -> {200, json}
        :"$done" -> not_found()
      end
    end)
  end

  defp with_stmt(name, args, st, fun) do
    s = Map.fetch!(st, name)

    try do
      :ok = :esqlite3.bind(s, args)
      fun.(s)
    after
      :esqlite3.reset(s)
    end
  end

  defp liked(id, already), do: {if(already, do: 200, else: 201), ["{\"liked\":true,\"already_liked\":", to_string(already), ",\"post_id\":", Integer.to_string(id), "}"]}
  defp not_found, do: {404, ~s({"error":"post not found"})}
end
