package challenge;

import io.micronaut.core.annotation.Order;
import io.micronaut.core.order.Ordered;
import io.micronaut.core.type.Argument;
import io.micronaut.core.type.MutableHeaders;
import io.micronaut.http.*;
import io.micronaut.http.annotation.Produces;
import io.micronaut.http.body.*;
import io.micronaut.http.codec.CodecException;
import jakarta.inject.Singleton;
import java.io.IOException;
import java.io.OutputStream;

@Singleton
@Produces(MediaType.APPLICATION_JSON)
@Order(Ordered.HIGHEST_PRECEDENCE)
public final class SqlJsonBodyWriter implements TypedMessageBodyWriter<byte[]>, ResponseBodyWriter<byte[]> {
    private static final Argument<byte[]> TYPE = Argument.of(byte[].class);

    @Override public Argument<byte[]> getType() { return TYPE; }

    @Override public CloseableByteBody writePiece(ByteBodyFactory bodyFactory, HttpRequest<?> request,
            HttpResponse<?> response, Argument<byte[]> type, MediaType mediaType, byte[] bytes) {
        // Each SQLite result owns a fresh array, never mutated after publication. Adapt only
        // during response encoding (after commit), transferring release ownership to Micronaut.
        return bodyFactory.adapt(bytes);
    }

    @Override public void writeTo(Argument<byte[]> type, MediaType mediaType, byte[] bytes,
            MutableHeaders headers, OutputStream stream) throws CodecException {
        if (mediaType != null) headers.setIfMissing(HttpHeaders.CONTENT_TYPE, mediaType);
        try { stream.write(bytes); }
        catch (IOException e) { throw new CodecException("Failed to write SQL JSON response", e); }
    }
}
