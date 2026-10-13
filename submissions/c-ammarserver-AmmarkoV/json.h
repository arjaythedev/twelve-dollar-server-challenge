/* The small amount of JSON the feed API has to read and write. */
#ifndef FEEDAPI_JSON_H_INCLUDED
#define FEEDAPI_JSON_H_INCLUDED

/* The spec asks for different errors for "this is not JSON" ( 400 malformed JSON body ) and "this is JSON
 * but its body field is not a non-empty string" ( 400 body is required ), so the whole document is
 * validated even though only one field is ever wanted out of it. */
enum JsonFieldResult
{
   JSON_FIELD_OK = 0,
   JSON_MALFORMED,          /* the document is not valid JSON */
   JSON_FIELD_NOT_A_STRING  /* valid JSON, but the field is absent or is not a string */
};

/**
 * Validate a JSON document and copy the unescaped value of one top-level string field out of it.
 * out gets the decoded UTF-8 bytes, NUL terminated. A value too long for out still returns
 * JSON_FIELD_OK with the length it would have needed, so the caller can tell a too-long value from an
 * invalid one.
 */
enum JsonFieldResult JsonGetStringField(
                                         const char * doc,
                                         unsigned int docLength,
                                         const char * field,
                                         char * out,
                                         unsigned int outSize,
                                         unsigned int * outLength
                                       );

/**
 * Append text to a JSON string value, escaping what JSON requires to be escaped ( '"' , '\' and the C0
 * controls ). Everything else, UTF-8 included, is passed through : the spec makes escaping non-ASCII and
 * <>&/ optional, and not escaping them is both fewer bytes and less work.
 * Returns the number of bytes written, or 0 if there was not room for all of it.
 */
unsigned int JsonEscape(const char * in,unsigned int inLength,char * out,unsigned int outSize);

#endif
