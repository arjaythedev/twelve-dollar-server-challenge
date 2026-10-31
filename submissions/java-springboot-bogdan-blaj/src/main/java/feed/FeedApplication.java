package feed;

import feed.http.FeedServer;
import org.springframework.boot.Banner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.Environment;

@SpringBootApplication(proxyBeanMethods = false)
public class FeedApplication {

    public static void main(String[] args) {
        new SpringApplicationBuilder(FeedApplication.class)
                .web(WebApplicationType.NONE)
                .bannerMode(Banner.Mode.OFF)
                .run(args);
    }

    @Bean
    FeedProperties feedProperties(Environment env) {
        return FeedProperties.fromEnvironment(env);
    }

    @Bean
    FeedServer feedServer(FeedProperties properties) {
        return new FeedServer(properties);
    }
}
