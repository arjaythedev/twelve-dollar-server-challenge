{-# LANGUAGE LambdaCase #-}
{-# LANGUAGE OverloadedStrings #-}
{-# LANGUAGE ScopedTypeVariables #-}
{-# LANGUAGE TypeApplications #-}

module Main (main) where

import Control.Concurrent
import Control.Concurrent.STM
import Control.Exception
import Control.Monad
import qualified Crypto.Hash.SHA256 as SHA
import Data.Bits (xor, (.|.))
import Data.IORef (newIORef)
import qualified Data.ByteString as B
import qualified Data.ByteString.Base64.URL as B64
import qualified Data.ByteString.Char8 as BC
import qualified Data.ByteString.Lazy as BL
import Data.Functor ((<&>))
import Data.Int (Int64)
import Data.Streaming.Network (bindPortTCP)
import Data.String (fromString)
import qualified Data.Text as T
import Data.Text.Encoding (decodeUtf8With, encodeUtf8)
import Data.Text.Encoding.Error (lenientDecode)
import Data.Text.Read (decimal)
import Database.SQLite3 hiding (close)
import Database.SQLite3.Direct (Utf8 (..))
import qualified Database.SQLite3.Direct as Direct
import GHC.Clock (getMonotonicTime)
import Network.HTTP.Types
import Network.Wai
import Network.Socket (SockAddr, Socket, SocketOption (NoDelay), accept, close, getSocketName, setSocketOption)
import Network.Socket.BufferPool (newBufferPool, receive)
import qualified Network.Socket.ByteString as Sock
import Network.Wai.Handler.Warp (defaultSettings, setPort, setServerName, setTimeout)
import Network.Wai.Handler.Warp.Internal (Connection (..), createWriteBuffer, runSettingsConnection)
import System.Environment (getEnv)
import System.IO.Error (ioeGetErrorType)
import GHC.IO.Exception (IOErrorType (InvalidArgument))

data R = R {feedQ, postQ, jwtQ, bodyQ, healthQ :: Statement}

data W = W {wdb :: Database, insPost, insLike, existsQ :: Statement}

type Job = IO (Maybe SomeException -> IO ())

postJson :: T.Text
postJson =
  "json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,\
  \'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))"

openDb :: FilePath -> IO Database
openDb path = do
  db <- open (T.pack path)
  exec db "PRAGMA busy_timeout=5000; PRAGMA mmap_size=0; PRAGMA cache_size=-32000"
  pure db

openR :: FilePath -> IO R
openR path = do
  db <- openDb path
  let p = prepare db
  R
    <$> p
      ( "SELECT json_object('posts',json_group_array(json(j))) FROM (SELECT "
          <> postJson
          <> " j \
             \FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20)"
      )
    <*> p ("SELECT json_object('post'," <> postJson <> ") FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1")
    -- ?1 = JWT header, ?2 = payload. Invalid JSON becomes NULL, which every json function passes through.
    -- 0 = invalid or expired token, 1 = bad payload, 2 = ok
    <*> p
      "WITH j(h,p) AS (SELECT CASE WHEN json_valid(?1) THEN ?1 END, CASE WHEN json_valid(?2) THEN ?2 END) \
      \SELECT CASE WHEN h->>'$.alg' IS NOT 'HS256' OR coalesce(json_type(p,'$.exp'),'') NOT IN ('integer','real') \
      \              OR p->>'$.exp' <= unixepoch() THEN 0 \
      \            WHEN json_type(p,'$.sub') IS NOT 'text' OR p->>'$.sub' NOT GLOB '[1-9]*' \
      \              OR p->>'$.sub' GLOB '*[^0-9]*' OR json_type(p,'$.username') IS NOT 'text' THEN 1 \
      \            ELSE 2 END, CAST(p->>'$.sub' AS INTEGER), p->>'$.username' FROM j"
    -- 0 = malformed JSON, 1 = body required, 2 = too long, 3 = ok (with the trimmed body)
    <*> p
      "WITH j(v) AS (SELECT CASE WHEN json_valid(?1) THEN ?1 END), \
      \     k(v,b) AS (SELECT v, CASE json_type(v,'$.body') WHEN 'text' \
      \                          THEN trim(v->>'$.body', char(32,9,10,11,12,13)) END FROM j) \
      \SELECT CASE WHEN v IS NULL THEN 0 WHEN ifnull(b,'')='' THEN 1 WHEN length(b)>500 THEN 2 ELSE 3 END, b FROM k"
    <*> p "SELECT json_object('status','ok','db','ok','uptime_s',?1)"

openW :: FilePath -> IO W
openW path = do
  db <- openDb path
  exec db "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL"
  W db
    <$> prepare
      db
      "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING json_object('post',json_object(\
      \'id',id,'body',body,'created_at',created_at,'author',?3,'like_count',0))"
    <*> prepare
      db
      "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) \
      \ON CONFLICT DO NOTHING"
    <*> prepare db "SELECT 1 FROM posts WHERE id=?1"

-- Run a prepared statement to completion and return its rows. stepNoCB is an unsafe FFI call: no OS
-- thread handoff per row, which suits these sub-millisecond queries (and we register no SQLite callbacks).
run :: Statement -> [SQLData] -> IO [[SQLData]]
run st args = (bind st args >> go) `finally` (reset st >> clearBindings st)
  where
    go = stepNoCB st >>= \case Row -> (:) <$> columns st <*> go; Done -> pure []

-- Like 'run', but returns the first column of the first row as SQLite's own UTF-8 bytes (no Text round trip).
runBS :: Statement -> [SQLData] -> IO (Maybe B.ByteString)
runBS st args = (bind st args >> stepNoCB st >>= first) `finally` (reset st >> clearBindings st)
  where
    first = \case Row -> Direct.columnText st 0 >>= \(Utf8 b) -> Just b <$ drain; Done -> pure Nothing
    drain = stepNoCB st >>= \case Row -> drain; Done -> pure ()

-- Warp's own connections get 16 KB send and 16 KB receive buffers each. Our requests are under 1 KB and
-- the largest response (the feed) is about 5 KB, so smaller buffers fit far more idle keep-alive
-- connections in 2 GB. Warp grows the write buffer itself if a response ever doesn't fit.
smallConnection :: Socket -> IO (Connection, SockAddr)
smallConnection listener = do
  (s, sa) <- accept listener
  setSocketOption s NoDelay 1
  pool <- newBufferPool 1024 4096
  wbuf <- newIORef =<< createWriteBuffer 8192
  h2 <- newIORef False
  me <- getSocketName s
  apps <- newTVarIO 0
  let recv = receive s pool `catch` \e -> if ioeGetErrorType e == InvalidArgument then pure "" else throwIO e
  pure
    ( Connection
        { connSendMany = Sock.sendMany s,
          connSendAll = Sock.sendAll s,
          connSendFile = \_ _ _ _ _ -> throwIO (userError "sendfile is not supported"),
          connClose = close s,
          connRecv = recv,
          connRecvBuf = \_ _ -> pure True,
          connWriteBuffer = wbuf,
          connHTTP2 = h2,
          connMySockAddr = me,
          connAppsInProgress = apps
        },
      sa
    )

-- Group commit: take every queued job, run them all in one transaction, commit once, then answer each.
writer :: W -> TQueue Job -> IO ()
writer w q = forever $ do
  jobs <- atomically $ flushTQueue q >>= \js -> if null js then retry else pure js
  exec (wdb w) "BEGIN IMMEDIATE"
  done <- sequence jobs
  c <- try (exec (wdb w) "COMMIT")
  either (\(_ :: SomeException) -> void (try @SomeException (exec (wdb w) "ROLLBACK"))) pure c
  mapM_ ($ either Just (const Nothing) c) done

submit :: TQueue Job -> IO a -> IO a
submit q act = do
  mv <- newEmptyMVar
  atomically $ writeTQueue q $ try act >>= \r -> pure (\ce -> putMVar mv (maybe r Left ce))
  takeMVar mv >>= either throwIO pure

json :: Status -> B.ByteString -> Response
json s = responseLBS s [(hContentType, "application/json")] . BL.fromStrict

err :: Status -> B.ByteString -> Response
err s m = json s ("{\"error\":\"" <> m <> "\"}")

text :: B.ByteString -> SQLData
text = SQLText . decodeUtf8With lenientDecode

-- Signature checked here (HMAC in constant time), header/payload checked by SQLite.
auth :: B.ByteString -> R -> Request -> IO (Either Response (Int64, T.Text))
auth secret r req = case B.stripPrefix "Bearer " =<< lookup hAuthorization (requestHeaders req) of
  Nothing -> pure (Left (err status401 "missing bearer token"))
  Just tok -> case BC.split '.' tok of
    [h, p, s]
      | Right sig <- B64.decodeUnpadded s,
        Right hj <- B64.decodeUnpadded h,
        Right pj <- B64.decodeUnpadded p,
        sig `ctEq` SHA.hmac secret (h <> "." <> p) ->
          run (jwtQ r) [text hj, text pj] <&> \case
            [[SQLInteger 2, SQLInteger uid, SQLText name]] -> Right (uid, name)
            [[SQLInteger 1, _, _]] -> Left (err status401 "invalid token payload")
            _ -> invalid
    _ -> pure invalid
  where
    invalid = Left (err status401 "invalid or expired token")
    ctEq a b = B.length a == B.length b && foldr (.|.) 0 (B.zipWith xor a b) == 0

parseId :: T.Text -> Maybe Int64
parseId t = case decimal t of Right (n, "") | n > 0 -> Just n; _ -> Nothing

app :: B.ByteString -> Double -> MVar R -> TQueue Job -> W -> Application
app secret start reader q w req respond =
  respond
    =<< route `catch` \(e :: SomeException) ->
      case fromException e of
        Just (a :: SomeAsyncException) -> throwIO a
        Nothing -> pure (err status500 "internal server error")
  where
    withR = withMVar reader
    withId i k = maybe (pure (err status400 "invalid post id")) k (parseId i)
    withAuth k = withR (\r -> auth secret r req) >>= either pure k
    one st args = withR (\r -> run (st r) args)
    oneBS st args = withR (\r -> runBS (st r) args)
    route = case (requestMethod req, pathInfo req) of
      ("GET", ["health"]) -> do
        up <- subtract start <$> getMonotonicTime
        try (oneBS healthQ [SQLInteger (floor up)]) <&> \case
          Right (Just b) -> json status200 b
          Left (e :: SomeException) ->
            json status503 $
              "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\""
                <> encodeUtf8 (T.concatMap esc (T.pack (show e)))
                <> "\"}"
          _ -> err status500 "internal server error"
      ("GET", ["feed"]) ->
        oneBS feedQ [] <&> \case
          Just b -> json status200 b
          Nothing -> err status500 "internal server error"
      ("GET", ["posts", i]) -> withId i $ \n ->
        oneBS postQ [SQLInteger n] <&> \case
          Just b -> json status200 b
          Nothing -> err status404 "post not found"
      ("POST", ["posts"]) -> withAuth $ \(uid, name) -> do
        body <- BL.toStrict <$> strictRequestBody req
        one bodyQ [text body] >>= \case
          [[SQLInteger 3, b]] ->
            submit q (runBS (insPost w) [SQLInteger uid, b, SQLText name]) <&> \case
              Just r -> json status201 r
              Nothing -> err status500 "internal server error"
          [[SQLInteger 2, _]] -> pure (err status400 "body must be at most 500 characters")
          [[SQLInteger 1, _]] -> pure (err status400 "body is required")
          _ -> pure (err status400 "malformed JSON body")
      ("POST", ["posts", i, "like"]) -> withAuth $ \(uid, _) -> withId i $ \n -> do
        res <- submit q $ do
          _ <- run (insLike w) [SQLInteger uid, SQLInteger n]
          inserted <- (> 0) <$> changes (wdb w)
          if inserted then pure (Just False) else run (existsQ w) [SQLInteger n] <&> \rows -> if null rows then Nothing else Just True
        pure $ case res of
          Nothing -> err status404 "post not found"
          Just already ->
            json (if already then status200 else status201) $
              "{\"liked\":true,\"already_liked\":"
                <> (if already then "true" else "false")
                <> ",\"post_id\":"
                <> BC.pack (show n)
                <> "}"
      _ -> pure (err status404 "not found")
    esc c | c == '"' || c == '\\' = T.pack ['\\', c] | c < ' ' = " " | otherwise = T.singleton c

main :: IO ()
main = do
  path <- getEnv "SQLITE_PATH"
  secret <- BC.pack <$> getEnv "JWT_SECRET"
  host <- getEnv "HOST"
  port <- read <$> getEnv "PORT"
  start <- getMonotonicTime
  w <- openW path
  q <- newTQueueIO
  _ <- forkIO (writer w q)
  reader <- newMVar =<< openR path -- one read connection: on one vCPU more would only add switching
  listener <- bindPortTCP port (fromString host)
  let settings = setPort port $ setTimeout 120 $ setServerName "" defaultSettings
  runSettingsConnection settings (smallConnection listener) (app secret start reader q w)
