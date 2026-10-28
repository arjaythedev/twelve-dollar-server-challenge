/* JSON utilities for HolyC */
#ifndef JSON_HC
#define JSON_HC

#include "./strings.HC"

extern I32 sprintf(U8 *buf, U8 *fmt, ...);

#define JSON_TRUE "true"
#define JSON_FALSE "false"

/* Build HTTP response with status line and headers */
public U8 *build_response_fast(I32 status, U8 *body, I64 body_len) {
    U8 *resp = MAlloc(body_len + 256);
    U8 status_line[128];
    if (status == 200) sprintf((U8*)status_line, (U8*)"HTTP/1.1 200 OK\r\n");
    else if (status == 201) sprintf((U8*)status_line, (U8*)"HTTP/1.1 201 Created\r\n");
    else if (status == 400) sprintf((U8*)status_line, (U8*)"HTTP/1.1 400 Bad Request\r\n");
    else if (status == 401) sprintf((U8*)status_line, (U8*)"HTTP/1.1 401 Unauthorized\r\n");
    else if (status == 404) sprintf((U8*)status_line, (U8*)"HTTP/1.1 404 Not Found\r\n");
    else if (status == 500) sprintf((U8*)status_line, (U8*)"HTTP/1.1 500 Internal Server Error\r\n");
    else if (status == 503) sprintf((U8*)status_line, (U8*)"HTTP/1.1 503 Service Unavailable\r\n");
    else sprintf((U8*)status_line, (U8*)"HTTP/1.1 %d OK\r\n", status);
    
    sprintf((U8*)resp, (U8*)"%sContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: %ld\r\n\r\n%s", status_line, body_len, body);
    return resp;
}

#endif