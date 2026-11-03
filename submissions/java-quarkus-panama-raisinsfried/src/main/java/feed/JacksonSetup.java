package feed;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.module.blackbird.BlackbirdModule;
import io.quarkus.jackson.ObjectMapperCustomizer;
import jakarta.inject.Singleton;

/** Blackbird: property accessors through LambdaMetafactory instead of reflection. */
@Singleton
final class JacksonSetup implements ObjectMapperCustomizer {
    @Override
    public void customize(ObjectMapper mapper) {
        mapper.registerModule(new BlackbirdModule());
    }
}
