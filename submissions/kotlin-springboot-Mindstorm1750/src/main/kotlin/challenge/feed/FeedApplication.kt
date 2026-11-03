package challenge.feed

import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.context.properties.ConfigurationPropertiesScan
import org.springframework.boot.runApplication

@SpringBootApplication
@ConfigurationPropertiesScan
class FeedApplication

fun main(args: Array<String>) {
    runApplication<FeedApplication>(*args)
}
