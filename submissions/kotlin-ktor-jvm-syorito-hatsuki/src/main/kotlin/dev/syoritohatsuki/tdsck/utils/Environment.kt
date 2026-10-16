package dev.syoritohatsuki.tdsck.utils

import io.github.cdimascio.dotenv.dotenv
import kotlin.properties.ReadOnlyProperty

val JWT_SECRET by environment("twelve-dollar-challenge")

val HOST by environment("127.0.0.1")
val PORT by environment(80)

val SQLITE_PATH by environment("./sqlite.db")

inline fun <reified T> environment(defaultValue: T): ReadOnlyProperty<T?, T> = ReadOnlyProperty { _, property ->
    val envValue = System.getProperty(property.name) ?: dotenv { ignoreIfMissing = true }[property.name]
    ?: System.getenv(property.name)

    when (T::class) {
        String::class -> envValue ?: defaultValue
        Int::class -> envValue?.toIntOrNull() ?: defaultValue
        Long::class -> envValue?.toLongOrNull() ?: defaultValue
        Boolean::class -> envValue?.toBooleanStrictOrNull() ?: defaultValue
        Double::class -> envValue?.toDoubleOrNull() ?: defaultValue
        Float::class -> envValue?.toFloatOrNull() ?: defaultValue
        else -> defaultValue
    } as T
}