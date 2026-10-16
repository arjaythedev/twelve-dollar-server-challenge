# secret <- charToRaw("dev-secret")
# test_val <- paste(
#     "Bearer",
#     jose::jwt_encode_hmac(
#         jose::jwt_claim(
#             sub = ulid::ulid(),
#             username = "alice",
#             exp = Sys.time() + 60 * 5000
#         ),
#         secret
#     )
# )
# verify token isn't expired
# rook <- fiery::fake_request(
#     url = 'http://www.example.com/summary?id=2347&user=Thomas+Lin+Pedersen',
#     content = '{"name":["Thomas Lin Pedersen"],"age":[31],"homepage":["www.data-imaginist.com","www.github.com/thomasp85"]}',
#     headers = list(
#         Content_Type = 'application/json',
#         Accept = 'application/json, application/xml; q=0.5, text/*; q=0.3',
#         Accept_Encoding = 'gzip, br',
#         Authorization = test_val
#     )
# )

# request <- reqres::Request$new(rook)
# response <- reqres::Response$new(request)

secret <- charToRaw(Sys.getenv("JWT_SECRET"))

# returns the user claim
authorize <- function(request, response, secret = Sys.getenv("JWT_SECRET")) {
    auth_header <- request$get_header("authorization")
    if (rlang::is_null(auth_header)) {
        response$status <- 401L
        return(list(error = "missing bearer token"))
    }

    if (!startsWith(auth_header, "Bearer ")) {
        response$status <- 401L
        return(list(error = "missing bearer token"))
    }

    # extract the payload
    payload <- substr(auth_header, 8, nchar(auth_header))

    # get the claim
    claim <- rlang::try_fetch(
        jose::jwt_decode_hmac(payload, secret),
        error = \(err) {
            response$status <- 401L
            list(error = "invalid or expired token")
        }
    )

    if (!is.null(claim$error)) {
        return(claim)
    }

    if (is.null(claim$username)) {
        response$status <- 401L
        return(list(error = "invalid token payload"))
    }

    # sub must be a positive integer
    sub <- rlang::try_fetch(as.integer(claim$sub), warning = \(cnd) NA_integer_)
    if (length(sub) != 1L || is.na(sub) || sub < 1L) {
        response$status <- 401L
        return(list(error = "invalid token payload"))
    }

    claim
}
