package io.github.wolches.challenge12.service;

import io.github.wolches.challenge12.exception.MissingBodyException;
import io.github.wolches.challenge12.exception.TooLongBodyException;
import io.github.wolches.challenge12.model.PostCreateRequestDto;
import jakarta.enterprise.context.ApplicationScoped;

@ApplicationScoped
public class BodyService {

    public String getBodyString(PostCreateRequestDto postCreateRequestDto) {
        var body = postCreateRequestDto.body();
        if (postCreateRequestDto.body() == null) {
            throw new MissingBodyException();
        }
        if (!body.isTextual()) {
            throw new MissingBodyException();
        }
        var text = body.asText().trim();
        if (text.isEmpty()) {
            throw new MissingBodyException();
        }
        if (text.length() > 500) {
            throw new TooLongBodyException();
        }
        return text;
    }
}
