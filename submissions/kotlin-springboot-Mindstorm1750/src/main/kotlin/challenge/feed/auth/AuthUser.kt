package challenge.feed.auth

/** The caller of an authenticated endpoint, taken from a verified JWT (the username is not looked up). */
data class AuthUser(val id: Long, val username: String)
