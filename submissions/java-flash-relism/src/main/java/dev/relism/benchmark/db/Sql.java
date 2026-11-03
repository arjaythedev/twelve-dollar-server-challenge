package dev.relism.benchmark.db;

final class Sql {
    private Sql() {}

    // Short result aliases avoid JDBC materializing SQL expressions as column names on each execution.
    private static final String POST = """
            json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,
              'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))
            """;
    static final String FEED = """
            SELECT CAST('{"posts":[' || coalesce(group_concat(item,','),'') || ']}' AS BLOB) AS json
            FROM (SELECT %s AS item FROM posts p JOIN users u ON u.id=p.user_id
                  ORDER BY p.created_at DESC,p.id DESC LIMIT 20)
            """.formatted(POST);
    static final String GET = """
            SELECT CAST('{"post":' || %s || '}' AS BLOB) AS json
            FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?
            """.formatted(POST);
    static final String CREATE = """
            INSERT INTO posts(user_id,body) VALUES(?,CAST(? AS TEXT))
            RETURNING CAST('{"post":' || json_object('id',id,'body',body,'created_at',created_at,
                           'author',CAST(? AS TEXT),'like_count',0) || '}' AS BLOB) AS json
            """;
    static final String LIKE = """
            INSERT INTO likes(user_id,post_id) SELECT ?,? WHERE EXISTS(SELECT 1 FROM posts WHERE id=?)
            ON CONFLICT(user_id,post_id) DO NOTHING
            """;
}
