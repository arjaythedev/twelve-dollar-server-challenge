package com.brocchirodrigo.feed.infrastructure.persistence.mapper;

import com.brocchirodrigo.feed.domain.model.Post;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteStatement;

public final class PostRowMapper {

    private PostRowMapper() {
    }

    public static Post toPost(SqliteStatement row) {
        return new Post(row.getLong(0), row.getText(1), row.getText(2), row.getText(3), row.getLong(4));
    }
}
