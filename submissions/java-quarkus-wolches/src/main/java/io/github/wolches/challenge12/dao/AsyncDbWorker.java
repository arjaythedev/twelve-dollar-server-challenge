package io.github.wolches.challenge12.dao;

import io.agroal.api.AgroalDataSource;
import io.github.wolches.challenge12.exception.BusinessLogicException;
import io.github.wolches.challenge12.exception.InternalServerErrorException;
import io.smallrye.mutiny.Uni;
import io.vertx.mutiny.core.Vertx;
import io.vertx.mutiny.core.WorkerExecutor;
import lombok.extern.slf4j.Slf4j;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.util.HashMap;
import java.util.Map;

import static java.util.concurrent.TimeUnit.SECONDS;

@Slf4j
public class AsyncDbWorker implements AutoCloseable {
    private final String name;
    private final Connection connection;
    private final WorkerExecutor worker;
    private final Map<String, PreparedStatement> preparedStatements;

    public AsyncDbWorker(AgroalDataSource dataSource, Vertx vertx, String name) throws SQLException {
        this.name = name;
        this.worker = vertx.createSharedWorkerExecutor(name, 1, 1, SECONDS);
        this.connection = dataSource.getConnection();
        this.preparedStatements = new HashMap<>();
        this.connection.setAutoCommit(false);
    }

    public <T> Uni<T> tx(ConnectionJob<T> job) {
        return worker.executeBlocking(() -> {
            try {
                var result = job.run(connection);
                connection.commit();
                return result;
            } catch (BusinessLogicException e) {
                rollback();
                throw e;
            } catch (Exception e) {
                rollback();
                throw new InternalServerErrorException();
            }
        });
    }

    public PreparedStatement prepareStatement(String sql) throws SQLException {
        if (!preparedStatements.containsKey(sql)) {
            preparedStatements.put(sql, connection.prepareStatement(sql));
        }
        var cached = preparedStatements.get(sql);
        cached.clearParameters();
        return cached;
    }

    @Override
    public void close() throws SQLException {
        connection.close();
        worker.closeAndAwait();
    }

    private void rollback() {
        try {
            connection.rollback();
        } catch (SQLException ex) {
            log.error("{}: error rolling back transaction", name, ex);
        }
    }
}
