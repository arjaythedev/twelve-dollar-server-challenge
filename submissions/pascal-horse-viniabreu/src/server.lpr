program TwelveDollarServer;

{$MODE DELPHI}{$H+}
{$APPTYPE CONSOLE}
{$POINTERMATH ON}

uses
  {$IFDEF UNIX}
  cthreads,
  cwstring,
  {$ENDIF}
  Classes,
  SysUtils,
  StrUtils,
  DateUtils,
  fpjson,
  jsonparser,
  sqlite3,
  Horse;

const
  JsonContentType = 'application/json';

var
  DatabasePath: string;
  JwtSecret: RawByteString;
  StartedAt: QWord;

threadvar
  DbHandle: PSQLite3;
  FeedStatement: PSQLite3_Stmt;
  PostStatement: PSQLite3_Stmt;
  CreatePostStatement: PSQLite3_Stmt;
  LikeStatement: PSQLite3_Stmt;
  PostExistsStatement: PSQLite3_Stmt;
  HealthStatement: PSQLite3_Stmt;

type
  TAuthResult = (arOK, arMissingBearer, arInvalidToken, arInvalidPayload);

  TStringBuffer = record
    Data: RawByteString;
    Used: SizeInt;
    procedure Init(const InitialCapacity: SizeInt);
    procedure AppendChar(const Value: AnsiChar); inline;
    procedure AppendText(const Value: string); inline;
    procedure AppendJsonString(const Value: string);
    procedure AppendJsonUtf8(const Value: PAnsiChar; const ValueLength: SizeInt);
    function Finish: string;
  end;

procedure TStringBuffer.Init(const InitialCapacity: SizeInt);
begin
  Used := 0;
  SetLength(Data, InitialCapacity);
end;

procedure TStringBuffer.AppendChar(const Value: AnsiChar);
begin
  if Used = Length(Data) then
  begin
    if Used = 0 then
      SetLength(Data, 64)
    else
      SetLength(Data, Used * 2);
  end;
  Inc(Used);
  Data[Used] := Value;
end;

procedure TStringBuffer.AppendText(const Value: string);
var
  Required, Capacity: SizeInt;
begin
  if Value = '' then
    Exit;
  Required := Used + Length(Value);
  if Required > Length(Data) then
  begin
    Capacity := Length(Data);
    if Capacity < 64 then Capacity := 64;
    while Capacity < Required do
      Capacity := Capacity * 2;
    SetLength(Data, Capacity);
  end;
  Move(Value[1], Data[Used + 1], Length(Value));
  Used := Required;
end;

procedure TStringBuffer.AppendJsonString(const Value: string);
const
  Hex: array[0..15] of Char = '0123456789abcdef';
var
  I: Integer;
  B: Byte;
begin
  AppendChar('"');
  for I := 1 to Length(Value) do
  begin
    B := Ord(Value[I]);
    case B of
      8: AppendText('\b');
      9: AppendText('\t');
      10: AppendText('\n');
      12: AppendText('\f');
      13: AppendText('\r');
      Ord('"'): AppendText('\"');
      Ord('\'): AppendText('\\');
    else
      if B < 32 then
      begin
        AppendText('\u00');
        AppendChar(Hex[B shr 4]);
        AppendChar(Hex[B and $0F]);
      end
      else
        AppendChar(AnsiChar(Value[I]));
    end;
  end;
  AppendChar('"');
end;

procedure TStringBuffer.AppendJsonUtf8(const Value: PAnsiChar; const ValueLength: SizeInt);
const
  Hex: array[0..15] of Char = '0123456789abcdef';
var
  I: SizeInt;
  B: Byte;
begin
  AppendChar('"');
  for I := 0 to ValueLength - 1 do
  begin
    B := Ord(Value[I]);
    case B of
      8: AppendText('\b');
      9: AppendText('\t');
      10: AppendText('\n');
      12: AppendText('\f');
      13: AppendText('\r');
      Ord('"'): AppendText('\"');
      Ord('\'): AppendText('\\');
    else
      if B < 32 then
      begin
        AppendText('\u00');
        AppendChar(Hex[B shr 4]);
        AppendChar(Hex[B and $0F]);
      end
      else
        AppendChar(AnsiChar(B));
    end;
  end;
  AppendChar('"');
end;

function TStringBuffer.Finish: string;
begin
  SetLength(Data, Used);
  Result := string(Data);
end;

function RotateRight(Value: LongWord; Count: Byte): LongWord; inline;
begin
  Result := (Value shr Count) or (Value shl (32 - Count));
end;

function Sha256(const Data: RawByteString): RawByteString;
const
  K: array[0..63] of LongWord = (
    $428A2F98, $71374491, $B5C0FBCF, $E9B5DBA5, $3956C25B, $59F111F1, $923F82A4, $AB1C5ED5,
    $D807AA98, $12835B01, $243185BE, $550C7DC3, $72BE5D74, $80DEB1FE, $9BDC06A7, $C19BF174,
    $E49B69C1, $EFBE4786, $0FC19DC6, $240CA1CC, $2DE92C6F, $4A7484AA, $5CB0A9DC, $76F988DA,
    $983E5152, $A831C66D, $B00327C8, $BF597FC7, $C6E00BF3, $D5A79147, $06CA6351, $14292967,
    $27B70A85, $2E1B2138, $4D2C6DFC, $53380D13, $650A7354, $766A0ABB, $81C2C92E, $92722C85,
    $A2BFE8A1, $A81A664B, $C24B8B70, $C76C51A3, $D192E819, $D6990624, $F40E3585, $106AA070,
    $19A4C116, $1E376C08, $2748774C, $34B0BCB5, $391C0CB3, $4ED8AA4A, $5B9CCA4F, $682E6FF3,
    $748F82EE, $78A5636F, $84C87814, $8CC70208, $90BEFFFA, $A4506CEB, $BEF9A3F7, $C67178F2
  );
var
  Buffer: array of Byte;
  W: array[0..63] of LongWord;
  A, B, C, D, E, F, G, H, T1, T2: LongWord;
  H0, H1, H2, H3, H4, H5, H6, H7: LongWord;
  BitLength: QWord;
  Block, I, J, Offset, OriginalLength, PaddedLength: Integer;
begin
  OriginalLength := Length(Data);
  PaddedLength := ((OriginalLength + 9 + 63) div 64) * 64;
  SetLength(Buffer, PaddedLength);
  if OriginalLength > 0 then
    Move(Data[1], Buffer[0], OriginalLength);
  Buffer[OriginalLength] := $80;
  BitLength := QWord(OriginalLength) * 8;
  for I := 0 to 7 do
    Buffer[PaddedLength - 1 - I] := Byte(BitLength shr (I * 8));

  H0 := $6A09E667; H1 := $BB67AE85; H2 := $3C6EF372; H3 := $A54FF53A;
  H4 := $510E527F; H5 := $9B05688C; H6 := $1F83D9AB; H7 := $5BE0CD19;

  for Block := 0 to (PaddedLength div 64) - 1 do
  begin
    Offset := Block * 64;
    for I := 0 to 15 do
    begin
      J := Offset + I * 4;
      W[I] := (LongWord(Buffer[J]) shl 24) or (LongWord(Buffer[J + 1]) shl 16) or
              (LongWord(Buffer[J + 2]) shl 8) or LongWord(Buffer[J + 3]);
    end;
    for I := 16 to 63 do
      W[I] := (RotateRight(W[I - 2], 17) xor RotateRight(W[I - 2], 19) xor (W[I - 2] shr 10)) +
              W[I - 7] +
              (RotateRight(W[I - 15], 7) xor RotateRight(W[I - 15], 18) xor (W[I - 15] shr 3)) +
              W[I - 16];

    A := H0; B := H1; C := H2; D := H3; E := H4; F := H5; G := H6; H := H7;
    for I := 0 to 63 do
    begin
      T1 := H + (RotateRight(E, 6) xor RotateRight(E, 11) xor RotateRight(E, 25)) +
            ((E and F) xor ((not E) and G)) + K[I] + W[I];
      T2 := (RotateRight(A, 2) xor RotateRight(A, 13) xor RotateRight(A, 22)) +
            ((A and B) xor (A and C) xor (B and C));
      H := G; G := F; F := E; E := D + T1; D := C; C := B; B := A; A := T1 + T2;
    end;
    H0 := H0 + A; H1 := H1 + B; H2 := H2 + C; H3 := H3 + D;
    H4 := H4 + E; H5 := H5 + F; H6 := H6 + G; H7 := H7 + H;
  end;

  SetLength(Result, 32);
  for I := 0 to 3 do
  begin
    Result[I + 1] := AnsiChar(H0 shr (24 - I * 8)); Result[I + 5] := AnsiChar(H1 shr (24 - I * 8));
    Result[I + 9] := AnsiChar(H2 shr (24 - I * 8)); Result[I + 13] := AnsiChar(H3 shr (24 - I * 8));
    Result[I + 17] := AnsiChar(H4 shr (24 - I * 8)); Result[I + 21] := AnsiChar(H5 shr (24 - I * 8));
    Result[I + 25] := AnsiChar(H6 shr (24 - I * 8)); Result[I + 29] := AnsiChar(H7 shr (24 - I * 8));
  end;
end;

function HmacSha256(const Key, Data: RawByteString): RawByteString;
var
  KeyBlock: array[0..63] of Byte;
  Inner, Outer: RawByteString;
  I, KeyLength: Integer;
begin
  FillChar(KeyBlock, SizeOf(KeyBlock), 0);
  if Length(Key) > 64 then
    Inner := Sha256(Key)
  else
    Inner := Key;
  KeyLength := Length(Inner);
  for I := 1 to KeyLength do
    KeyBlock[I - 1] := Ord(Inner[I]);

  SetLength(Inner, 64 + Length(Data));
  SetLength(Outer, 64 + 32);
  for I := 0 to 63 do
  begin
    Inner[I + 1] := AnsiChar(KeyBlock[I] xor $36);
    Outer[I + 1] := AnsiChar(KeyBlock[I] xor $5C);
  end;
  if Length(Data) > 0 then
    Move(Data[1], Inner[65], Length(Data));
  Inner := Sha256(Inner);
  Move(Inner[1], Outer[65], 32);
  Result := Sha256(Outer);
end;

function Base64UrlDecode(const Value: string; out Decoded: RawByteString): Boolean;
var
  I, Bits, Accumulator, Code, ByteValue, Used: Integer;
  Ch: Char;
begin
  Result := False;
  Decoded := '';
  if (Length(Value) = 0) or ((Length(Value) mod 4) = 1) then
    Exit;
  SetLength(Decoded, ((Length(Value) * 3) div 4) + 2);
  Used := 0;
  Bits := 0;
  Accumulator := 0;
  for I := 1 to Length(Value) do
  begin
    Ch := Value[I];
    if (Ch >= 'A') and (Ch <= 'Z') then Code := Ord(Ch) - Ord('A')
    else if (Ch >= 'a') and (Ch <= 'z') then Code := Ord(Ch) - Ord('a') + 26
    else if (Ch >= '0') and (Ch <= '9') then Code := Ord(Ch) - Ord('0') + 52
    else if Ch = '-' then Code := 62
    else if Ch = '_' then Code := 63
    else Exit;
    Accumulator := (Accumulator shl 6) or Code;
    Inc(Bits, 6);
    while Bits >= 8 do
    begin
      Dec(Bits, 8);
      ByteValue := (Accumulator shr Bits) and $FF;
      Inc(Used);
      Decoded[Used] := AnsiChar(ByteValue);
    end;
    if Bits = 0 then
      Accumulator := 0
    else
      Accumulator := Accumulator and ((1 shl Bits) - 1);
  end;
  SetLength(Decoded, Used);
  Result := True;
end;

function ConstantTimeEqual(const Left, Right: RawByteString): Boolean;
var
  I, Difference: Integer;
begin
  Difference := Length(Left) xor Length(Right);
  for I := 1 to Length(Left) do
    if I <= Length(Right) then
      Difference := Difference or (Ord(Left[I]) xor Ord(Right[I]))
    else
      Difference := Difference or Ord(Left[I]);
  Result := Difference = 0;
end;

function JsonString(const Value: string): string;
var
  Buffer: TStringBuffer;
begin
  Buffer.Init(Length(Value) + 2);
  Buffer.AppendJsonString(Value);
  Result := Buffer.Finish;
end;

procedure AppendPostJson(var Buffer: TStringBuffer; const Id: Int64;
  const Body, CreatedAt, Author: string; const LikeCount: Int64);
begin
  Buffer.AppendText('{"id":');
  Buffer.AppendText(IntToStr(Id));
  Buffer.AppendText(',"body":');
  Buffer.AppendJsonString(Body);
  Buffer.AppendText(',"created_at":');
  Buffer.AppendJsonString(CreatedAt);
  Buffer.AppendText(',"author":');
  Buffer.AppendJsonString(Author);
  Buffer.AppendText(',"like_count":');
  Buffer.AppendText(IntToStr(LikeCount));
  Buffer.AppendChar('}');
end;

procedure AppendPostJsonFromStatement(var Buffer: TStringBuffer;
  Statement: PSQLite3_Stmt);
var
  Column: Integer;
begin
  Buffer.AppendText('{"id":');
  Buffer.AppendText(IntToStr(sqlite3_column_int64(Statement, 0)));
  Buffer.AppendText(',"body":');
  Column := 1;
  Buffer.AppendJsonUtf8(sqlite3_column_text(Statement, Column),
    sqlite3_column_bytes(Statement, Column));
  Buffer.AppendText(',"created_at":');
  Column := 2;
  Buffer.AppendJsonUtf8(sqlite3_column_text(Statement, Column),
    sqlite3_column_bytes(Statement, Column));
  Buffer.AppendText(',"author":');
  Column := 3;
  Buffer.AppendJsonUtf8(sqlite3_column_text(Statement, Column),
    sqlite3_column_bytes(Statement, Column));
  Buffer.AppendText(',"like_count":');
  Buffer.AppendText(IntToStr(sqlite3_column_int64(Statement, 4)));
  Buffer.AppendChar('}');
end;

function PostJson(const Id: Int64; const Body, CreatedAt, Author: string; const LikeCount: Int64): string;
var
  Buffer: TStringBuffer;
begin
  Buffer.Init(Length(Body) + Length(CreatedAt) + Length(Author) + 96);
  AppendPostJson(Buffer, Id, Body, CreatedAt, Author, LikeCount);
  Result := Buffer.Finish;
end;

procedure SendJson(Res: THorseResponse; StatusCode: Integer; const Payload: string);
begin
  Res.ContentType(JsonContentType).Status(StatusCode).Send(Payload);
end;

procedure SendError(Res: THorseResponse; StatusCode: Integer; const Message: string);
begin
  SendJson(Res, StatusCode, '{"error":' + JsonString(Message) + '}');
end;

function IsPositiveId(const Value: string; out Id: Int64): Boolean;
var
  I: Integer;
begin
  Result := False;
  Id := 0;
  if Value = '' then Exit;
  for I := 1 to Length(Value) do
    if not (Value[I] in ['0'..'9']) then Exit;
  Result := TryStrToInt64(Value, Id) and (Id > 0);
end;

function Utf8CodePointCount(const Value: string): Integer;
var
  I: Integer;
  B: Byte;
begin
  Result := 0;
  I := 1;
  while I <= Length(Value) do
  begin
    B := Ord(Value[I]);
    Inc(Result);
    if (B and $80) = 0 then Inc(I)
    else if (B and $E0) = $C0 then Inc(I, 2)
    else if (B and $F0) = $E0 then Inc(I, 3)
    else if (B and $F8) = $F0 then Inc(I, 4)
    else Inc(I);
  end;
end;

procedure CheckSqlite(const Code: Integer);
begin
  if Code <> SQLITE_OK then
    raise Exception.Create(string(sqlite3_errmsg(DbHandle)));
end;

procedure ExecuteDirect(const Sql: string);
begin
  CheckSqlite(sqlite3_exec(DbHandle, PAnsiChar(Sql), nil, nil, nil));
end;

function GetDatabase: PSQLite3;
var
  Code: Integer;
begin
  if DbHandle = nil then
  begin
    Code := sqlite3_open_v2(PAnsiChar(DatabasePath), @DbHandle,
      SQLITE_OPEN_READWRITE or SQLITE_OPEN_NOMUTEX, nil);
    if Code <> SQLITE_OK then
      raise Exception.Create('unable to open SQLite database');
    try
      ExecuteDirect('PRAGMA journal_mode=WAL');
      ExecuteDirect('PRAGMA synchronous=NORMAL');
      CheckSqlite(sqlite3_busy_timeout(DbHandle, 5000));
      ExecuteDirect('PRAGMA mmap_size=1073741824');
      ExecuteDirect('PRAGMA cache_size=-65536');
    except
      sqlite3_close(DbHandle);
      DbHandle := nil;
      raise;
    end;
  end;
  Result := DbHandle;
end;

function CachedStatement(var Statement: PSQLite3_Stmt;
  const Sql: string): PSQLite3_Stmt;
begin
  GetDatabase;
  if Statement = nil then
    CheckSqlite(sqlite3_prepare_v2(DbHandle, PAnsiChar(Sql), Length(Sql),
      @Statement, nil));
  Result := Statement;
end;

procedure ResetStatement(const Statement: PSQLite3_Stmt); inline;
begin
  CheckSqlite(sqlite3_reset(Statement));
end;

function GetPostJson(const Id: Int64; out Payload: string): Boolean;
var
  Statement: PSQLite3_Stmt;
  Code: Integer;
  Buffer: TStringBuffer;
begin
  Statement := CachedStatement(PostStatement,
    'SELECT p.id, p.body, p.created_at, u.username, ' +
    '(SELECT count(*) FROM likes l WHERE l.post_id = p.id) ' +
    'FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?');
  CheckSqlite(sqlite3_bind_int64(Statement, 1, Id));
  Code := sqlite3_step(Statement);
  try
    Result := Code = SQLITE_ROW;
    if Result then
    begin
      Buffer.Init(512);
      AppendPostJsonFromStatement(Buffer, Statement);
      Payload := Buffer.Finish;
    end
    else if Code <> SQLITE_DONE then
      CheckSqlite(Code);
  finally
    ResetStatement(Statement);
  end;
end;

function GetFeedJson: string;
var
  Statement: PSQLite3_Stmt;
  Code: Integer;
  First: Boolean;
  Buffer: TStringBuffer;
begin
  Statement := CachedStatement(FeedStatement,
    'SELECT p.id, p.body, p.created_at, u.username, ' +
    '(SELECT count(*) FROM likes l WHERE l.post_id = p.id) ' +
    'FROM posts p JOIN users u ON u.id = p.user_id ' +
    'ORDER BY p.created_at DESC, p.id DESC LIMIT 20');
  Buffer.Init(12288);
  Buffer.AppendText('{"posts":[');
  First := True;
  Code := sqlite3_step(Statement);
  try
    while Code = SQLITE_ROW do
    begin
      if not First then Buffer.AppendChar(',');
      AppendPostJsonFromStatement(Buffer, Statement);
      First := False;
      Code := sqlite3_step(Statement);
    end;
    if Code <> SQLITE_DONE then
      CheckSqlite(Code);
    Buffer.AppendText(']}');
    Result := Buffer.Finish;
  finally
    ResetStatement(Statement);
  end;
end;

function CreatePostJson(const UserId: Int64; const Body, Username: string): string;
var
  Statement: PSQLite3_Stmt;
  Code: Integer;
  Buffer: TStringBuffer;
begin
  Statement := CachedStatement(CreatePostStatement,
    'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at');
  CheckSqlite(sqlite3_bind_int64(Statement, 1, UserId));
  CheckSqlite(sqlite3_bind_text(Statement, 2, PAnsiChar(Body), Length(Body),
    sqlite3_destructor_type(SQLITE_TRANSIENT)));
  Code := sqlite3_step(Statement);
  try
    if Code <> SQLITE_ROW then
      CheckSqlite(Code);
    Buffer.Init(Length(Body) + Length(Username) + 128);
    Buffer.AppendText('{"post":{"id":');
    Buffer.AppendText(IntToStr(sqlite3_column_int64(Statement, 0)));
    Buffer.AppendText(',"body":');
    Buffer.AppendJsonString(Body);
    Buffer.AppendText(',"created_at":');
    Buffer.AppendJsonUtf8(sqlite3_column_text(Statement, 1),
      sqlite3_column_bytes(Statement, 1));
    Buffer.AppendText(',"author":');
    Buffer.AppendJsonString(Username);
    Buffer.AppendText(',"like_count":0}}');
    Result := Buffer.Finish;
    Code := sqlite3_step(Statement);
    if Code <> SQLITE_DONE then
      CheckSqlite(Code);
  finally
    ResetStatement(Statement);
  end;
end;

function LikePost(const UserId, PostId: Int64; out AlreadyLiked: Boolean): Boolean;
var
  Statement: PSQLite3_Stmt;
  Code: Integer;
begin
  Result := False;
  AlreadyLiked := False;
  Statement := CachedStatement(LikeStatement,
    'INSERT INTO likes (user_id, post_id) ' +
    'SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?) ' +
    'ON CONFLICT(user_id, post_id) DO NOTHING');
  CheckSqlite(sqlite3_bind_int64(Statement, 1, UserId));
  CheckSqlite(sqlite3_bind_int64(Statement, 2, PostId));
  CheckSqlite(sqlite3_bind_int64(Statement, 3, PostId));
  Code := sqlite3_step(Statement);
  try
    if Code <> SQLITE_DONE then
      CheckSqlite(Code);
    if sqlite3_changes(DbHandle) = 1 then
      Exit(True);
  finally
    ResetStatement(Statement);
  end;

  Statement := CachedStatement(PostExistsStatement,
    'SELECT 1 FROM posts WHERE id = ?');
  CheckSqlite(sqlite3_bind_int64(Statement, 1, PostId));
  Code := sqlite3_step(Statement);
  try
    Result := Code = SQLITE_ROW;
    if not Result and (Code <> SQLITE_DONE) then
      CheckSqlite(Code);
    AlreadyLiked := Result;
  finally
    ResetStatement(Statement);
  end;
end;

procedure CheckDatabase;
var
  Statement: PSQLite3_Stmt;
  Code: Integer;
begin
  Statement := CachedStatement(HealthStatement, 'SELECT 1');
  Code := sqlite3_step(Statement);
  try
    if Code <> SQLITE_ROW then
      CheckSqlite(Code);
  finally
    ResetStatement(Statement);
  end;
end;

function ParseJwtObject(const Text: RawByteString; out ObjectValue: TJSONObject): Boolean;
var
  Data: TJSONData;
begin
  Result := False;
  ObjectValue := nil;
  try
    Data := GetJSON(string(Text));
    if Data is TJSONObject then
    begin
      ObjectValue := TJSONObject(Data);
      Result := True;
    end
    else
      Data.Free;
  except
    ObjectValue := nil;
  end;
end;

function Authenticate(Req: THorseRequest; out UserId: Int64; out Username: string): TAuthResult;
var
  Authorization, Token, HeaderPart, PayloadPart, SignaturePart: string;
  FirstDot, SecondDot: Integer;
  HeaderJson, PayloadJson, Signature, Expected: RawByteString;
  Header, Payload: TJSONObject;
  Field: TJSONData;
  Expiry: Int64;
begin
  UserId := 0;
  Username := '';
  Authorization := Req.Headers['authorization'];
  if Copy(Authorization, 1, 7) <> 'Bearer ' then
    Exit(arMissingBearer);
  Token := Copy(Authorization, 8, MaxInt);
  FirstDot := Pos('.', Token);
  SecondDot := PosEx('.', Token, FirstDot + 1);
  if (FirstDot <= 1) or (SecondDot <= FirstDot + 1) or (SecondDot >= Length(Token)) or
    (PosEx('.', Token, SecondDot + 1) <> 0) then
    Exit(arInvalidToken);
  HeaderPart := Copy(Token, 1, FirstDot - 1);
  PayloadPart := Copy(Token, FirstDot + 1, SecondDot - FirstDot - 1);
  SignaturePart := Copy(Token, SecondDot + 1, MaxInt);
  if not Base64UrlDecode(HeaderPart, HeaderJson) or
    not Base64UrlDecode(PayloadPart, PayloadJson) or
    not Base64UrlDecode(SignaturePart, Signature) then
    Exit(arInvalidToken);
  Expected := HmacSha256(JwtSecret, RawByteString(HeaderPart + '.' + PayloadPart));
  if not ConstantTimeEqual(Signature, Expected) then
    Exit(arInvalidToken);
  Header := nil;
  Payload := nil;
  try
    if not ParseJwtObject(HeaderJson, Header) then Exit(arInvalidToken);
    Field := Header.Find('alg');
    if (Field = nil) or not (Field is TJSONString) or (Field.AsString <> 'HS256') then
      Exit(arInvalidToken);
    if not ParseJwtObject(PayloadJson, Payload) then Exit(arInvalidToken);
    Field := Payload.Find('exp');
    if (Field = nil) or not (Field.JSONType = jtNumber) or
      not TryStrToInt64(Field.AsString, Expiry) or (Expiry <= DateTimeToUnix(Now, False)) then
      Exit(arInvalidToken);
    Field := Payload.Find('sub');
    if (Field = nil) or not (Field is TJSONString) or not IsPositiveId(Field.AsString, UserId) then
      Exit(arInvalidPayload);
    Field := Payload.Find('username');
    if (Field = nil) or not (Field is TJSONString) then
      Exit(arInvalidPayload);
    Username := Field.AsString;
    Result := arOK;
  finally
    Header.Free;
    Payload.Free;
  end;
end;

function ParsePostBody(const Body: string; out TrimmedBody, ErrorMessage: string): Boolean;
var
  Data, Field: TJSONData;
begin
  Result := False;
  TrimmedBody := '';
  ErrorMessage := '';
  Data := nil;
  try
    try
      Data := GetJSON(Body);
    except
      ErrorMessage := 'malformed JSON body';
      Exit;
    end;
    if not (Data is TJSONObject) then
    begin
      ErrorMessage := 'body is required';
      Exit;
    end;
    Field := TJSONObject(Data).Find('body');
    if (Field = nil) or not (Field is TJSONString) then
    begin
      ErrorMessage := 'body is required';
      Exit;
    end;
    TrimmedBody := Trim(Field.AsString);
    if TrimmedBody = '' then
      ErrorMessage := 'body is required'
    else if Utf8CodePointCount(TrimmedBody) > 500 then
      ErrorMessage := 'body must be at most 500 characters'
    else
      Result := True;
  finally
    Data.Free;
  end;
end;

procedure HandleHealth(Req: THorseRequest; Res: THorseResponse);
begin
  try
    CheckDatabase;
    SendJson(Res, 200, '{"status":"ok","db":"ok","uptime_s":' +
      IntToStr((GetTickCount64 - StartedAt) div 1000) + '}');
  except
    on E: Exception do SendError(Res, 503, E.Message);
  end;
end;

procedure HandleFeed(Req: THorseRequest; Res: THorseResponse);
begin
  try
    SendJson(Res, 200, GetFeedJson);
  except
    on E: Exception do SendError(Res, 500, 'internal server error');
  end;
end;

procedure HandleGetPost(Req: THorseRequest; Res: THorseResponse);
var
  Id: Int64;
  Post: string;
begin
  if not IsPositiveId(Req.Params['id'], Id) then
  begin
    SendError(Res, 400, 'invalid post id');
    Exit;
  end;
  try
    if GetPostJson(Id, Post) then
      SendJson(Res, 200, '{"post":' + Post + '}')
    else
      SendError(Res, 404, 'post not found');
  except
    on E: Exception do SendError(Res, 500, 'internal server error');
  end;
end;

procedure HandleCreatePost(Req: THorseRequest; Res: THorseResponse);
var
  UserId: Int64;
  Username, Body, ErrorMessage: string;
  Auth: TAuthResult;
begin
  Auth := Authenticate(Req, UserId, Username);
  case Auth of
    arMissingBearer: begin SendError(Res, 401, 'missing bearer token'); Exit; end;
    arInvalidToken: begin SendError(Res, 401, 'invalid or expired token'); Exit; end;
    arInvalidPayload: begin SendError(Res, 401, 'invalid token payload'); Exit; end;
  end;
  if not ParsePostBody(Req.Body, Body, ErrorMessage) then
  begin
    SendError(Res, 400, ErrorMessage);
    Exit;
  end;
  try
    SendJson(Res, 201, CreatePostJson(UserId, Body, Username));
  except
    on E: Exception do SendError(Res, 500, 'internal server error');
  end;
end;

procedure HandleLikePost(Req: THorseRequest; Res: THorseResponse);
var
  UserId, PostId: Int64;
  Username: string;
  Auth: TAuthResult;
  AlreadyLiked, Exists: Boolean;
begin
  Auth := Authenticate(Req, UserId, Username);
  case Auth of
    arMissingBearer: begin SendError(Res, 401, 'missing bearer token'); Exit; end;
    arInvalidToken: begin SendError(Res, 401, 'invalid or expired token'); Exit; end;
    arInvalidPayload: begin SendError(Res, 401, 'invalid token payload'); Exit; end;
  end;
  if not IsPositiveId(Req.Params['id'], PostId) then
  begin
    SendError(Res, 400, 'invalid post id');
    Exit;
  end;
  try
    Exists := LikePost(UserId, PostId, AlreadyLiked);
    if not Exists then
      SendError(Res, 404, 'post not found')
    else if AlreadyLiked then
      SendJson(Res, 200, '{"liked":true,"already_liked":true,"post_id":' + IntToStr(PostId) + '}')
    else
      SendJson(Res, 201, '{"liked":true,"already_liked":false,"post_id":' + IntToStr(PostId) + '}');
  except
    on E: Exception do SendError(Res, 500, 'internal server error');
  end;
end;

procedure HandleNotFound(Req: THorseRequest; Res: THorseResponse);
begin
  SendError(Res, 404, 'not found');
end;

procedure RegisterRoutes;
begin
  THorse.Get('/health', HandleHealth);
  THorse.Get('/feed', HandleFeed);
  THorse.Get('/posts/:id', HandleGetPost);
  THorse.Post('/posts', HandleCreatePost);
  THorse.Post('/posts/:id/like', HandleLikePost);
  THorse.Get('*', HandleNotFound);
  THorse.Post('*', HandleNotFound);
  THorse.Put('*', HandleNotFound);
  THorse.Patch('*', HandleNotFound);
  THorse.Delete('*', HandleNotFound);
end;

var
  Host: string;
  Port: Integer;
begin
  SetMultiByteConversionCodePage(CP_UTF8);
  DatabasePath := GetEnvironmentVariable('SQLITE_PATH');
  JwtSecret := RawByteString(GetEnvironmentVariable('JWT_SECRET'));
  Host := GetEnvironmentVariable('HOST');
  Port := StrToIntDef(GetEnvironmentVariable('PORT'), 3000);
  if DatabasePath = '' then
    raise Exception.Create('SQLITE_PATH is required');
  if JwtSecret = '' then
    raise Exception.Create('JWT_SECRET is required');
  if Host = '' then Host := '127.0.0.1';
  StartedAt := GetTickCount64;
  THorse.UseRadixRouter;
  RegisterRoutes;
  THorse.Listen(Port, Host);
end.
