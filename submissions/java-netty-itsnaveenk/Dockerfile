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
    "-Xms512m", \
    "-Xmx512m", \
    "-Xmn384m", \
    "-XX:+AlwaysPreTouch", \
    "-XX:CompileThreshold=1500", \
    "-Dio.netty.allocator.type=pooled", \
    "-Dio.netty.allocator.numDirectArenas=1", \
    "-Dio.netty.allocator.numHeapArenas=0", \
    "-Dio.netty.buffer.checkBounds=false", \
    "-Dio.netty.buffer.checkAccessible=false", \
    "-jar", "/app/server.jar"]
