FROM eclipse-temurin:21-jre

WORKDIR /app

# Copy shaded fat jar
COPY target/server.jar /app/server.jar

ENV SQLITE_PATH=/app/feed.db \
    JWT_SECRET=twelve-dollar-challenge \
    HOST=0.0.0.0 \
    PORT=80

EXPOSE 80

ENTRYPOINT ["java", \
    "-XX:+UseSerialGC", \
    "-Xms384m", \
    "-Xmx512m", \
    "-XX:+AlwaysPreTouch", \
    "-Dio.netty.allocator.type=pooled", \
    "-jar", "/app/server.jar"]
