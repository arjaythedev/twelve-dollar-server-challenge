-- gen-hmac.sql: SQL that writes SQL. Run at build time (sqlite3 :memory: < gen-hmac.sql > hmac.sql);
-- it prints hmac.sql, which defines HMAC-SHA256 for JWTs as temp tables and views.
--
-- SQLite has no SHA-256, so the hash is a recursive CTE with one row per round: the 8 working
-- variables, the 8 chaining values and a 16-word message window as columns. There are no user
-- functions in SQL either, so the round expressions are expanded here from text templates with
-- replace(). SQLite has no XOR: a^b^c = a+b+c-2*maj(a,b,c) on 32-bit values (a full adder: the
-- sum bit plus twice the carry), which names each operand fewer times than the inclusion-exclusion
-- form. Nor does it share subexpressions, so T1, needed twice per round, is computed once as the
-- only row of generate_series(T1, 4294967295, 4294967296) and read back as x1.value.
.headers off
.mode list
CREATE TEMP TABLE t(k TEXT PRIMARY KEY, v TEXT);
INSERT INTO t VALUES
 ('rotr', '((@X>>@N)|((@X<<@L)&4294967295))'),
 ('xor3', '((@A)+(@B)+(@C)-2*(((@A)&(@B))|((@C)&((@A)|(@B)))))');
-- the four sigma functions; the third term of the small sigmas is a shift, not a rotation
INSERT INTO t SELECT column1,
  replace(replace(replace(x3.v,
    '@A', replace(replace(replace(ro.v, '@X', column2), '@N', column3), '@L', 32 - column3)),
    '@B', replace(replace(replace(ro.v, '@X', column2), '@N', column4), '@L', 32 - column4)),
    '@C', CASE WHEN column6 THEN '(' || column2 || '>>' || column5 || ')'
          ELSE replace(replace(replace(ro.v, '@X', column2), '@N', column5), '@L', 32 - column5) END)
FROM (VALUES ('S0', 'a', 2, 13, 22, 0), ('S1', 'e', 6, 11, 25, 0),
             ('s0', 'w1', 7, 18, 3, 1), ('s1', 'w14', 17, 19, 10, 1)), t x3, t ro
WHERE x3.k = 'xor3' AND ro.k = 'rotr';
INSERT INTO t SELECT 'T1', '(h+' || v || '+((e&f)|((~e)&g))+kv+w15)' FROM t WHERE k = 'S1';
INSERT INTO t SELECT 'T2', '(' || v || '+((a&b)|(c&(a|b))))' FROM t WHERE k = 'S0';
-- One CTE: @NAME(@CARRY, blk, t, a..h, h0..h7, w0..w15). Row (blk, t) is about to run round t of
-- block blk with W_t in w15. @WORD is the message word expression with @J for the word index.
INSERT INTO t VALUES ('cte', '@NAME(@CARRY,blk,t,a,b,c,d,e,f,g,h,h0,h1,h2,h3,h4,h5,h6,h7,w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15) AS (
@SEED
UNION ALL SELECT @CARRY,blk+(t=63),(t+1)%64,
CASE WHEN t=63 THEN (h0+x1.value+@T2)&4294967295 ELSE (x1.value+@T2)&4294967295 END,
CASE WHEN t=63 THEN (h1+a)&4294967295 ELSE a END,
CASE WHEN t=63 THEN (h2+b)&4294967295 ELSE b END,
CASE WHEN t=63 THEN (h3+c)&4294967295 ELSE c END,
CASE WHEN t=63 THEN (h4+d+x1.value)&4294967295 ELSE (d+x1.value)&4294967295 END,
CASE WHEN t=63 THEN (h5+e)&4294967295 ELSE e END,
CASE WHEN t=63 THEN (h6+f)&4294967295 ELSE f END,
CASE WHEN t=63 THEN (h7+g)&4294967295 ELSE g END,
CASE WHEN t=63 THEN (h0+x1.value+@T2)&4294967295 ELSE h0 END,
CASE WHEN t=63 THEN (h1+a)&4294967295 ELSE h1 END,
CASE WHEN t=63 THEN (h2+b)&4294967295 ELSE h2 END,
CASE WHEN t=63 THEN (h3+c)&4294967295 ELSE h3 END,
CASE WHEN t=63 THEN (h4+d+x1.value)&4294967295 ELSE h4 END,
CASE WHEN t=63 THEN (h5+e)&4294967295 ELSE h5 END,
CASE WHEN t=63 THEN (h6+f)&4294967295 ELSE h6 END,
CASE WHEN t=63 THEN (h7+g)&4294967295 ELSE h7 END,
w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,
CASE WHEN t<15 THEN @WORD_NEXT WHEN t=63 THEN @WORD_BLOCK ELSE (@s1+w9+@s0+w0)&4294967295 END
FROM @NAME JOIN temp.jk ON kt=t, generate_series(@T1&4294967295, 4294967295, 4294967296) AS x1 WHERE blk<=@STOP)');
-- the three hashes: the key blocks (midstates), the inner hash of the token, the outer hash
CREATE TEMP TABLE c(id INT, name TEXT, carry TEXT, seed TEXT, word TEXT, stop TEXT);
INSERT INTO c VALUES
 (1, 's', 'pad', 'SELECT pad,0,0,h0,h1,h2,h3,h4,h5,h6,h7,h0,h1,h2,h3,h4,h5,h6,h7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,v
FROM temp.jkey, temp.jiv WHERE j=0', '(SELECT v FROM temp.jkey kk WHERE kk.pad=s.pad AND kk.j=(@J))', '0'),
 (2, 's', 'm,n,nb', 'SELECT m,n,nb,0,0,h0,h1,h2,h3,h4,h5,h6,h7,h0,h1,h2,h3,h4,h5,h6,h7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,@WORD0
FROM (SELECT m,length(m) AS n,(length(m)+72)/64 AS nb FROM temp.jr), temp.jmid WHERE pad=0',
  '(CASE WHEN (@J)=16*nb-1 THEN (64+n)*8 ELSE (@B0<<24)|(@B1<<16)|(@B2<<8)|@B3 END)', 'nb-1'),
 (3, 'o', 'd1,d2,d3,d4,d5,d6,d7', 'SELECT s.h1,s.h2,s.h3,s.h4,s.h5,s.h6,s.h7,0,0,o.h0,o.h1,o.h2,o.h3,o.h4,o.h5,o.h6,o.h7,o.h0,o.h1,o.h2,o.h3,o.h4,o.h5,o.h6,o.h7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,s.h0
FROM s, temp.jmid o WHERE s.blk=s.nb AND o.pad=1',
  '(CASE (@J) WHEN 1 THEN d1 WHEN 2 THEN d2 WHEN 3 THEN d3 WHEN 4 THEN d4 WHEN 5 THEN d5 WHEN 6 THEN d6 WHEN 7 THEN d7 WHEN 8 THEN 2147483648 WHEN 15 THEN 768 ELSE 0 END)', '0');
-- byte k of the padded token: the ASCII character, then 0x80, then zeros
UPDATE c SET word = replace(replace(replace(replace(word,
  '@B0', replace(b, '@K', '4*(@J)')), '@B1', replace(b, '@K', '4*(@J)+1')),
  '@B2', replace(b, '@K', '4*(@J)+2')), '@B3', replace(b, '@K', '4*(@J)+3'))
FROM (SELECT '(CASE WHEN (@K)<n THEN unicode(substr(m,(@K)+1,1)) WHEN (@K)=n THEN 128 ELSE 0 END)' AS b) WHERE id = 2;
UPDATE c SET seed = replace(seed, '@WORD0', replace(word, '@J', '0'));
CREATE TEMP VIEW expanded AS SELECT id,
  replace(replace(replace(replace(replace(replace(replace(replace(replace(replace(cte.v,
    '@WORD_NEXT', replace(word, '@J', '(16*blk+t+1)')), '@WORD_BLOCK', replace(word, '@J', '(16*blk+16)')),
    '@SEED', seed), '@NAME', name), '@CARRY', carry), '@STOP', stop),
    '@T1', (SELECT v FROM t WHERE k = 'T1')), '@T2', (SELECT v FROM t WHERE k = 'T2')),
    '@s0', (SELECT v FROM t WHERE k = 's0')), '@s1', (SELECT v FROM t WHERE k = 's1')) AS sql
FROM c, t cte WHERE cte.k = 'cte';

SELECT '-- hmac.sql: generated by gen-hmac.sql at build time. Needs temp.cfg(secret) and a view temp.jr(m).
-- SHA-256''s constants are not typed in: they are the fractional parts of the cube roots (K) and
-- square roots (IV) of the first primes, and the primes come from a sieve.
CREATE TEMP TABLE jk(kt INTEGER PRIMARY KEY, kv INT);
INSERT INTO temp.jk WITH RECURSIVE n(x) AS (SELECT 2 UNION ALL SELECT x+1 FROM n WHERE x<311),
  p AS (SELECT x, row_number() OVER (ORDER BY x) - 1 AS i FROM n
        WHERE NOT EXISTS (SELECT 1 FROM n d WHERE d.x*d.x<=n.x AND n.x%d.x=0))
  SELECT i, CAST((pow(x,1.0/3)-floor(pow(x,1.0/3)))*4294967296 AS INT) FROM p;
CREATE TEMP TABLE jiv AS SELECT ' || group_concat(printf('CAST((sqrt(%d)-floor(sqrt(%d)))*4294967296 AS INT) AS h%d', p, p, i), ', ') || ';
-- the key XOR ipad (0x36) and opad (0x5c), as 16 big-endian words each (keys up to 64 bytes)
CREATE TEMP TABLE jkey AS WITH b(i, c) AS (SELECT value, CASE WHEN value < length(secret)
    THEN unicode(substr(secret, value+1, 1)) ELSE 0 END FROM generate_series(0, 63), temp.cfg)
  SELECT pad, i/4 AS j, sum(((c|x)-(c&x)) << (8*(3-i%4))) AS v
  FROM b, (SELECT 0 AS pad, 54 AS x UNION ALL SELECT 1, 92) GROUP BY pad, i/4;
-- midstates: the key blocks compressed once from the IV
CREATE TEMP TABLE jmid AS WITH RECURSIVE ' || (SELECT sql FROM expanded WHERE id = 1) || '
SELECT pad,h0,h1,h2,h3,h4,h5,h6,h7 FROM s WHERE blk=1;
-- the signature of the token in temp.jr, base64url without padding
CREATE TEMP VIEW jsig AS WITH RECURSIVE ' || (SELECT sql FROM expanded WHERE id = 2) || ',
' || (SELECT sql FROM expanded WHERE id = 3) || '
SELECT rtrim(replace(replace(base64(unhex(printf(''%08X%08X%08X%08X%08X%08X%08X%08X'',h0,h1,h2,h3,h4,h5,h6,h7))),''+'',''-''),''/'',''_''),''=''||char(10)) AS sig FROM o WHERE blk=1;'
FROM (SELECT column1 AS p, column2 AS i FROM (VALUES (2,0),(3,1),(5,2),(7,3),(11,4),(13,5),(17,6),(19,7)));
