package org.localgrid.gridwatch.json

/**
 * A small JSON reader for the admin page's own `/api/status` reply (D70).
 *
 * Android's `org.json` is a stub in JVM unit tests, and the app must decode the same bytes the
 * tests do, so this parser lives in the app. It reads only what the AP sends: objects, arrays,
 * strings, numbers, booleans and null. It builds no text of its own and never logs.
 */
sealed interface JsonValue {
    data object Null : JsonValue
    data class Bool(val value: Boolean) : JsonValue
    data class Num(val value: Double) : JsonValue
    data class Str(val value: String) : JsonValue
    data class Arr(val items: List<JsonValue>) : JsonValue
    data class Obj(val fields: Map<String, JsonValue>) : JsonValue
}

class JsonException(message: String) : Exception(message)

object Json {
    fun parse(text: String): JsonValue {
        val p = Parser(text)
        p.skip()
        val v = p.value()
        p.skip()
        if (!p.atEnd()) throw JsonException("trailing characters after the JSON value")
        return v
    }

    private class Parser(private val s: String) {
        private var i = 0
        private var depth = 0

        fun atEnd() = i >= s.length

        fun skip() {
            while (i < s.length && s[i].isWhitespace()) i++
        }

        private fun expect(c: Char) {
            if (i >= s.length || s[i] != c) throw JsonException("expected '$c' at $i")
            i++
        }

        fun value(): JsonValue {
            if (i >= s.length) throw JsonException("the JSON ends early")
            return when (s[i]) {
                '{' -> obj()
                '[' -> arr()
                '"' -> JsonValue.Str(string())
                't' -> literal("true", JsonValue.Bool(true))
                'f' -> literal("false", JsonValue.Bool(false))
                'n' -> literal("null", JsonValue.Null)
                else -> number()
            }
        }

        private fun literal(word: String, v: JsonValue): JsonValue {
            if (!s.startsWith(word, i)) throw JsonException("bad literal at $i")
            i += word.length
            return v
        }

        private fun enter() {
            if (++depth > 32) throw JsonException("the JSON is nested too deeply")
        }

        private fun obj(): JsonValue {
            enter()
            expect('{')
            val out = LinkedHashMap<String, JsonValue>()
            skip()
            if (i < s.length && s[i] == '}') { i++; depth--; return JsonValue.Obj(out) }
            while (true) {
                skip()
                val k = string()
                skip()
                expect(':')
                skip()
                out[k] = value()
                skip()
                if (i < s.length && s[i] == ',') { i++; continue }
                expect('}')
                depth--
                return JsonValue.Obj(out)
            }
        }

        private fun arr(): JsonValue {
            enter()
            expect('[')
            val out = ArrayList<JsonValue>()
            skip()
            if (i < s.length && s[i] == ']') { i++; depth--; return JsonValue.Arr(out) }
            while (true) {
                skip()
                out.add(value())
                skip()
                if (i < s.length && s[i] == ',') { i++; continue }
                expect(']')
                depth--
                return JsonValue.Arr(out)
            }
        }

        private fun string(): String {
            expect('"')
            val b = StringBuilder()
            while (true) {
                if (i >= s.length) throw JsonException("a string never ends")
                val c = s[i++]
                when {
                    c == '"' -> return b.toString()
                    c != '\\' -> b.append(c)
                    else -> {
                        if (i >= s.length) throw JsonException("an escape never ends")
                        when (val e = s[i++]) {
                            '"', '\\', '/' -> b.append(e)
                            'b' -> b.append('\b')
                            'f' -> b.append('')
                            'n' -> b.append('\n')
                            'r' -> b.append('\r')
                            't' -> b.append('\t')
                            'u' -> {
                                if (i + 4 > s.length) throw JsonException("a \\u escape is cut short")
                                b.append(s.substring(i, i + 4).toInt(16).toChar())
                                i += 4
                            }
                            else -> throw JsonException("unknown escape \\$e")
                        }
                    }
                }
            }
        }

        private fun number(): JsonValue {
            val start = i
            if (i < s.length && (s[i] == '-' || s[i] == '+')) i++
            while (i < s.length && (s[i].isDigit() || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                    ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E')))
            ) i++
            val t = s.substring(start, i)
            return JsonValue.Num(t.toDoubleOrNull() ?: throw JsonException("bad number \"$t\""))
        }
    }
}

// -- the few accessors the status reply needs, forgiving of a field an older AP does not send

fun JsonValue?.obj(): Map<String, JsonValue> = (this as? JsonValue.Obj)?.fields ?: emptyMap()
fun JsonValue?.arr(): List<JsonValue> = (this as? JsonValue.Arr)?.items ?: emptyList()
fun JsonValue?.str(default: String = ""): String = (this as? JsonValue.Str)?.value ?: default
fun JsonValue?.strOrNull(): String? = (this as? JsonValue.Str)?.value
fun JsonValue?.num(default: Double = 0.0): Double = when (this) {
    is JsonValue.Num -> value
    is JsonValue.Bool -> if (value) 1.0 else 0.0
    else -> default
}

fun JsonValue?.int(default: Int = 0): Int = num(default.toDouble()).toInt()
fun JsonValue?.long(default: Long = 0L): Long = num(default.toDouble()).toLong()
fun JsonValue?.bool(default: Boolean = false): Boolean = when (this) {
    is JsonValue.Bool -> value
    is JsonValue.Num -> value != 0.0
    else -> default
}

operator fun JsonValue?.get(field: String): JsonValue? = (this as? JsonValue.Obj)?.fields?.get(field)
