defmodule Server do
  @moduledoc "HTTP/1.1 keep-alive server on raw :gen_tcp: one process per connection, OTP's C HTTP parser."

  @idle 75_000
  @max_body 16_384
  @max_head 32_768
  @acceptors 32

  def start do
    path = env!("SQLITE_PATH")
    secret = env!("JWT_SECRET")
    {:ok, _} = Application.ensure_all_started(:crypto)
    {:ok, _} = Db.start_link(path)
    {:ok, addr} = :inet.getaddr(String.to_charlist(System.get_env("HOST", "127.0.0.1")), :inet)
    port = String.to_integer(System.get_env("PORT", "3000"))
    :persistent_term.put(:srv, {secret, :erlang.monotonic_time(:second)})

    {:ok, l} =
      :gen_tcp.listen(port, [:binary, packet: :raw, active: false, reuseaddr: true, nodelay: true,
        backlog: 4096, ip: addr, keepalive: true, send_timeout: 30_000, send_timeout_close: true])

    for _ <- 1..@acceptors, do: spawn(fn -> accept(l) end)
    IO.puts("Listening on #{:inet.ntoa(addr)}:#{port}")
    Process.sleep(:infinity) # this process owns the listen socket
  end

  defp env!(k), do: (case System.get_env(k) do v when v not in [nil, ""] -> v end)

  defp accept(l) do
    case :gen_tcp.accept(l) do
      {:ok, s} ->
        pid = spawn(fn -> receive do :go -> loop(s, <<>>) end end)

        case :gen_tcp.controlling_process(s, pid) do
          :ok -> send(pid, :go)
          _ -> :gen_tcp.close(s)
        end

      {:error, :emfile} -> Process.sleep(10)
      {:error, _} -> :ok
    end

    accept(l)
  end

  # --- connection loop: parse one request from buf, answer it, continue with the leftover bytes ---
  defp loop(s, buf) do
    case :erlang.decode_packet(:http_bin, buf, []) do
      {:ok, {:http_request, m, {:abs_path, path}, vsn}, rest} -> head(s, buf, m, path, vsn, rest, {nil, 0, false})
      {:more, _} -> more(s, buf, buf)
      _ -> bad(s)
    end
  end

  defp head(s, buf0, m, path, vsn, rest, {auth, len, close} = acc) do
    case :erlang.decode_packet(:httph_bin, rest, []) do
      {:ok, {:http_header, _, name, _, v}, rest2} ->
        acc =
          case name do
            :Authorization -> {v, len, close}
            :"Content-Length" -> {auth, parse_len(v), close}
            :Connection -> {auth, len, close or String.contains?(String.downcase(v), "close")}
            _ -> acc
          end

        head(s, buf0, m, path, vsn, rest2, acc)

      {:ok, :http_eoh, rest2} -> body(s, buf0, m, path, vsn, rest2, acc)
      {:more, _} -> more(s, buf0, buf0)
      _ -> bad(s)
    end
  end

  defp body(s, _buf0, _m, _p, _v, _rest, {_, len, _}) when len < 0 or len > @max_body, do: bad(s)

  defp body(s, buf0, m, path, vsn, rest, {auth, len, close}) do
    if byte_size(rest) >= len do
      <<b::binary-size(^len), rest2::binary>> = rest
      {status, out} = route(m, path |> strip_query(), auth, b)
      close = close or vsn == {1, 0}

      case :gen_tcp.send(s, [head_line(status), "\r\ncontent-type: application/json\r\ncontent-length: ",
             Integer.to_string(IO.iodata_length(out)), if(close, do: "\r\nconnection: close\r\n\r\n", else: "\r\n\r\n"), out]) do
        :ok when not close -> loop(s, rest2)
        _ -> :gen_tcp.close(s)
      end
    else
      more(s, buf0, buf0)
    end
  end

  # not enough bytes yet: read more and re-parse from the start of this request
  defp more(s, buf0, _) when byte_size(buf0) > @max_head + @max_body, do: bad(s)

  defp more(s, buf0, _) do
    case :gen_tcp.recv(s, 0, @idle) do
      {:ok, d} -> loop(s, <<buf0::binary, d::binary>>)
      _ -> :gen_tcp.close(s)
    end
  end

  defp bad(s) do
    :gen_tcp.send(s, "HTTP/1.1 400 Bad Request\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
    :gen_tcp.close(s)
  end

  defp parse_len(v) do
    case Integer.parse(v) do
      {n, ""} -> n
      _ -> -1
    end
  end

  defp strip_query(p) do
    case :binary.split(p, "?") do
      [a, _] -> a
      [a] -> a
    end
  end

  defp head_line(200), do: "HTTP/1.1 200 OK"
  defp head_line(201), do: "HTTP/1.1 201 Created"
  defp head_line(400), do: "HTTP/1.1 400 Bad Request"
  defp head_line(401), do: "HTTP/1.1 401 Unauthorized"
  defp head_line(404), do: "HTTP/1.1 404 Not Found"
  defp head_line(503), do: "HTTP/1.1 503 Service Unavailable"
  defp head_line(_), do: "HTTP/1.1 500 Internal Server Error"

  # --- routes ---
  defp route(m, path, auth, body) do
    handle(m, path, auth, body)
  rescue
    _ -> err(500, "internal server error")
  end

  defp handle(:GET, "/feed", _, _), do: Db.feed()

  defp handle(:GET, "/health", _, _) do
    {_, started} = :persistent_term.get(:srv)

    case Db.health() do
      :ok -> {200, ["{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", Integer.to_string(:erlang.monotonic_time(:second) - started), "}"]}
      {:error, msg} -> {503, ["{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":", :json.encode(msg), "}"]}
    end
  end

  defp handle(:POST, "/posts", auth, raw) do
    {secret, _} = :persistent_term.get(:srv)

    with {:ok, uid, name} <- Auth.verify(auth, secret), {:ok, text} <- post_body(raw) do
      Db.create(uid, text, name)
    else
      {:error, msg} -> err(401, msg)
      {:bad, msg} -> err(400, msg)
    end
  end

  defp handle(m, "/posts/" <> rest, auth, _) when m in [:GET, :POST] do
    case {m, :binary.split(rest, "/", [:global])} do
      {:GET, [id]} ->
        with {:ok, id} <- post_id(id), do: Db.post(id), else: ({:bad, msg} -> err(400, msg))

      {:POST, [id, "like"]} ->
        {secret, _} = :persistent_term.get(:srv)

        with {:ok, uid, _} <- Auth.verify(auth, secret), {:ok, id} <- post_id(id) do
          Db.like(uid, id)
        else
          {:error, msg} -> err(401, msg)
          {:bad, msg} -> err(400, msg)
        end

      _ -> err(404, "not found")
    end
  end

  defp handle(_, _, _, _), do: err(404, "not found")

  defp err(status, msg), do: {status, ["{\"error\":", :json.encode(msg), "}"]}

  defp post_id(bin) do
    case Auth.positive_id(bin) do
      0 -> {:bad, "invalid post id"}
      id -> {:ok, id}
    end
  end

  defp post_body(raw) do
    json = try do :json.decode(raw) rescue _ -> :bad end

    case json do
      :bad -> {:bad, "malformed JSON body"}
      %{"body" => b} when is_binary(b) ->
        # trim Unicode code points (not graphemes), like JavaScript String.trim()
        t = b |> String.to_charlist() |> Enum.drop_while(&space?/1) |> Enum.reverse() |> Enum.drop_while(&space?/1) |> Enum.reverse()

        case length(t) do
          0 -> {:bad, "body is required"}
          n when n > 500 -> {:bad, "body must be at most 500 characters"}
          _ -> {:ok, :unicode.characters_to_binary(t)}
        end

      _ -> {:bad, "body is required"}
    end
  end

  defp space?(c) when c in 9..13 or c in 0x2000..0x200A, do: true
  defp space?(c), do: c in [32, 0xA0, 0x1680, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF]
end
