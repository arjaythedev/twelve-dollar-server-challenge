package challenge.feed

private val POSITIVE_INTEGER = Regex("[1-9][0-9]*")

/** Parses ids from paths and JWT subjects: a plain positive decimal integer that fits in a Long, else null. */
fun String.toPositiveLongOrNull(): Long? = if (POSITIVE_INTEGER.matches(this)) toLongOrNull() else null
