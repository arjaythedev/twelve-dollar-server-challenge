package challenge;

import java.net.InetAddress;
import java.net.UnknownHostException;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.reactor.netty.NettyReactiveWebServerFactory;
import org.springframework.boot.reactor.netty.NettyRouteProvider;
import org.springframework.context.annotation.Bean;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.server.reactive.HttpHandler;
import reactor.core.publisher.Mono;

/**
 * Spring Boot runs and manages the Reactor Netty server; a {@link NettyRouteProvider} routes every
 * request straight to {@link Api}. There is no WebFlux layer and no Spring request/response adapter
 * on the request path.
 */
@SpringBootApplication(proxyBeanMethods = false)
public class App {

    public static void main(String[] args) {
        SpringApplication.run(App.class, args);
    }

    @Bean
    NettyReactiveWebServerFactory webServerFactory(ObjectProvider<NettyRouteProvider> routes) throws UnknownHostException {
        NettyReactiveWebServerFactory factory = new NettyReactiveWebServerFactory(Integer.parseInt(env("PORT", "3000")));
        factory.setAddress(InetAddress.getByName(env("HOST", "127.0.0.1")));
        factory.addRouteProviders(routes.orderedStream().toArray(NettyRouteProvider[]::new));
        // Header values are only ever compared, never echoed back, so skip Netty's per-header validation.
        factory.addServerCustomizers(server -> server.httpRequestDecoder(spec -> spec.validateHeaders(false)));
        return factory;
    }

    @Bean(destroyMethod = "close")
    Db db() {
        int readers = Integer.getInteger("app.readers", 0);
        int maxBatch = Integer.getInteger("app.maxBatch", 256);
        return new Db(env("SQLITE_PATH", null), readers, maxBatch);
    }

    @Bean
    Jwt jwt() {
        return new Jwt(env("JWT_SECRET", null));
    }

    /** Every request is routed straight to {@link Api} on Reactor Netty. */
    @Bean
    NettyRouteProvider apiRoutes(Db db, Jwt jwt) {
        Api api = new Api(db, jwt);
        return routes -> routes.route(req -> true, api::handle);
    }

    /**
     * The reactive web server context requires a Spring {@link HttpHandler}. The route above matches
     * every request, so this fallback never runs.
     */
    @Bean
    HttpHandler httpHandler() {
        byte[] body = Resp.NOT_FOUND.body();
        return (req, res) -> {
            res.setStatusCode(HttpStatus.NOT_FOUND);
            res.getHeaders().setContentType(MediaType.APPLICATION_JSON);
            return res.writeWith(Mono.just(res.bufferFactory().wrap(body)));
        };
    }

    private static String env(String name, String fallback) {
        String v = System.getenv(name);
        if (v != null && !v.isEmpty()) return v;
        if (fallback == null) throw new IllegalStateException(name + " is not set");
        return fallback;
    }
}
