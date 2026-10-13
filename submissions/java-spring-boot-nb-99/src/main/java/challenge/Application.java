package challenge;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.tomcat.TomcatConnectorCustomizer;
import org.springframework.context.annotation.Bean;

@SpringBootApplication
public class Application {
    public static void main(String[] args) {
        SpringApplication.run(Application.class, args);
    }

    /**
     * Tomcat allocates a read and a write buffer on the heap for every open connection, and serving
     * directly means one keep-alive connection per client. Requests are far below 4 KiB, so a
     * smaller read buffer leaves heap room for more idle clients. Responses keep the 8 KiB write
     * buffer, so a feed response still leaves in one write.
     */
    @Bean
    TomcatConnectorCustomizer socketBuffers() {
        return connector -> connector.setProperty("socket.appReadBufSize", "4096");
    }
}
