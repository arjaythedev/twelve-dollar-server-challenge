package com.brocchirodrigo.feed;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.ConfigurationPropertiesScan;

@SpringBootApplication(proxyBeanMethods = false)
@ConfigurationPropertiesScan
public class FeedApplication {

    public static void main(String[] args) {
        var processors = Integer.toString(Runtime.getRuntime().availableProcessors());
        System.getProperties().putIfAbsent("reactor.netty.ioWorkerCount", processors);
        SpringApplication.run(FeedApplication.class, args);
    }
}
