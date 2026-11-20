<?php

declare(strict_types=1);

namespace App\Stages;

use Pype\Validation\Validate;

/** The new post's body (trimmed by TrimStrings): required, a string, at most 500 characters. The spec answers 400. */
final class ValidatePost extends Validate
{
    protected function rules(): array
    {
        return ['body' => 'required|string|max:500'];
    }

    protected function messages(): array
    {
        return [
            'body.required' => 'body is required',
            'body.string' => 'body is required',
            'body.max' => 'body must be at most 500 characters',
        ];
    }

    protected function status(): int
    {
        return 400;
    }
}
