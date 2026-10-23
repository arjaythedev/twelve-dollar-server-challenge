package io.github.wolches.challenge12.dao;

import java.sql.Connection;
import java.sql.SQLException;

@FunctionalInterface
public interface ConnectionJob<T> {

    T run(Connection c) throws SQLException;
}
