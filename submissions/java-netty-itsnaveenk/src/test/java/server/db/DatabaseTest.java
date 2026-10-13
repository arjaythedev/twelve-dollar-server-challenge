package server.db;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.io.File;
import java.nio.file.Files;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.Statement;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

public class DatabaseTest {

    private File tempDbFile;
    private Database database;

    @BeforeEach
    void setup() throws Exception {
        tempDbFile = File.createTempFile("test_feed", ".db");
        tempDbFile.deleteOnExit();

        // Initialize schema
        try (Connection c = DriverManager.getConnection("jdbc:sqlite:" + tempDbFile.getAbsolutePath());
             Statement s = c.createStatement()) {
            s.execute("""
                CREATE TABLE users (
                  id          INTEGER PRIMARY KEY,
                  username    TEXT NOT NULL UNIQUE,
                  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );
                """);
            s.execute("""
                CREATE TABLE posts (
                  id          INTEGER PRIMARY KEY,
                  user_id     INTEGER NOT NULL REFERENCES users(id),
                  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
                  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );
                """);
            s.execute("""
                CREATE TABLE likes (
                  user_id     INTEGER NOT NULL REFERENCES users(id),
                  post_id     INTEGER NOT NULL REFERENCES posts(id),
                  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                  PRIMARY KEY (user_id, post_id)
                );
                """);
            s.execute("CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);");
            s.execute("CREATE INDEX posts_user_id_idx ON posts (user_id);");
            s.execute("CREATE INDEX likes_post_id_idx ON likes (post_id);");
            s.execute("INSERT INTO users (id, username) VALUES (1, 'alice');");
            s.execute("INSERT INTO users (id, username) VALUES (2, 'bob');");
        }

        database = new Database(tempDbFile.getAbsolutePath());
    }

    @AfterEach
    void tearDown() throws Exception {
        if (database != null) {
            database.close();
        }
        if (tempDbFile != null && tempDbFile.exists()) {
            tempDbFile.delete();
        }
    }

    @Test
    void testCheckHealth() {
        assertTrue(database.checkHealth());
    }

    @Test
    void testCreatePostAndRead() throws Exception {
        Database.InsertPostResult result = database.createPost(1, "Hello world");
        assertTrue(result.id() > 0);
        assertNotNull(result.createdAt());
        assertTrue(result.createdAt().endsWith("Z"));

        Database.Post post = database.getPostById(result.id());
        assertNotNull(post);
        assertEquals(result.id(), post.id());
        assertEquals("Hello world", post.body());
        assertEquals("alice", post.author());
        assertEquals(0, post.likeCount());
    }

    @Test
    void testLikeWorkflow() throws Exception {
        Database.InsertPostResult p = database.createPost(1, "Post to like");

        // First like
        Database.LikeResult r1 = database.likePost(2, p.id());
        assertEquals(Database.LikeResult.CREATED, r1);

        // Read like count
        Database.Post post = database.getPostById(p.id());
        assertEquals(1, post.likeCount());

        // Repeat like
        Database.LikeResult r2 = database.likePost(2, p.id());
        assertEquals(Database.LikeResult.ALREADY_LIKED, r2);

        // Like non-existent post
        Database.LikeResult r3 = database.likePost(2, 999999L);
        assertEquals(Database.LikeResult.NOT_FOUND, r3);
    }

    @Test
    void testFeedOrdering() throws Exception {
        database.createPost(1, "First post");
        Thread.sleep(10);
        Database.InsertPostResult p2 = database.createPost(2, "Second post");

        List<Database.Post> feed = database.getFeed();
        assertEquals(2, feed.size());
        assertEquals(p2.id(), feed.get(0).id());
        assertEquals("Second post", feed.get(0).body());
    }
}
