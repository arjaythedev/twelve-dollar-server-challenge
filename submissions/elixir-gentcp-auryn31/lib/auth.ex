defmodule Auth do
  @moduledoc "HS256 JWT check with OTP crypto + json. Every request verifies its token; nothing is remembered."

  @max_float 1.7976931348623157e308

  def verify(<<"Bearer ", token::binary>>, secret) do
    case claims(token, secret) do
      nil -> {:error, "invalid or expired token"}
      %{"sub" => sub, "username" => name} when is_binary(name) ->
        case positive_id(sub) do
          0 -> {:error, "invalid token payload"}
          id -> {:ok, id, name}
        end
      _ -> {:error, "invalid token payload"}
    end
  end

  def verify(_, _), do: {:error, "missing bearer token"}

  defp claims(token, secret) do
    [h64, p64, s64] = :binary.split(token, ".", [:global])
    43 = byte_size(s64)
    true = :crypto.hash_equals(:crypto.mac(:hmac, :sha256, secret, [h64, ?., p64]), decode64(s64))
    %{"alg" => "HS256"} = :json.decode(decode64(h64))
    %{"exp" => exp} = claims = :json.decode(decode64(p64))
    now = :erlang.system_time(:microsecond) / 1_000_000
    true = is_number(exp) and exp <= @max_float and exp > now
    nbf = Map.get(claims, "nbf", 0)
    true = is_number(nbf) and nbf >= -@max_float and nbf <= now
    claims
  rescue
    _ -> nil
  end

  # JWT segments are unpadded base64url with no whitespace; OTP's decoder is laxer than that.
  defp decode64(bin) when byte_size(bin) > 0 do
    true = valid64(bin)
    :base64.decode(bin, %{mode: :urlsafe, padding: false})
  end

  defp valid64(<<>>), do: true
  defp valid64(<<c, rest::binary>>) when c in ?a..?z or c in ?A..?Z or c in ?0..?9 or c == ?- or c == ?_, do: valid64(rest)
  defp valid64(_), do: false

  # Positive decimal integer up to 2^53-1, no sign, no leading junk; 0 means invalid.
  def positive_id(<<c, rest::binary>>) when c in ?0..?9, do: digits(rest, c - ?0)
  def positive_id(_), do: 0

  defp digits(_, n) when n > 9_007_199_254_740_991, do: 0
  defp digits(<<>>, n), do: n
  defp digits(<<c, rest::binary>>, n) when c in ?0..?9, do: digits(rest, n * 10 + c - ?0)
  defp digits(_, _), do: 0
end
