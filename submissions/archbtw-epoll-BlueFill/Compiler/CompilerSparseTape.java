import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.HashMap;
import java.util.Map;
import java.util.List;

/**
 * Compiler for the "I use Arch btw" language (.archbtw) to plain C.
 *
 * Usage:
 *   java Compiler
 *   java Compiler [input.archbtw [output.c]]
 *
 * With no arguments, this compiles ./server.archbtw to ./server.c.
 * Applies semantics-preserving peephole optimizations while generating C.
 */
public final class CompilerSparseTape {
    private static final Path DEFAULT_INPUT = Path.of("server.archbtw");
    private static final Path DEFAULT_OUTPUT = Path.of("server.c");
    private static final int MEMORY_SIZE = 65_536;

    private CompilerSparseTape() {
    }

    public static void main(String[] args) {
        if (args.length > 2) {
            System.err.println("Usage: java CompilerSparseTape [input.archbtw [output.c]]");
            System.exit(2);
        }

        Path input = args.length >= 1 ? Path.of(args[0]) : DEFAULT_INPUT;
        Path output = args.length >= 2 ? Path.of(args[1]) : DEFAULT_OUTPUT;

        try {
            String source = Files.readString(input, StandardCharsets.UTF_8);
            List<Token> tokens = tokenize(source);
            String cSource = compile(tokens);
            Files.writeString(output, cSource, StandardCharsets.UTF_8);
            System.out.println("Compiled " + input + " -> " + output);
        } catch (CompilationException e) {
            System.err.println("Compiler error: " + e.getMessage());
            System.exit(1);
        } catch (IOException e) {
            System.err.println("I/O error: " + e.getMessage());
            System.exit(1);
        }
    }

    private static List<Token> tokenize(String source) throws CompilationException {
        List<Token> tokens = new ArrayList<>();
        int index = 0;
        int line = 1;
        int column = 1;
        final int sourceLength = source.length();

        while (index < sourceLength) {
            char current = source.charAt(index);

            if (isWhitespace(current)) {
                if (current == '\n') {
                    line++;
                    column = 1;
                } else {
                    column++;
                }
                index++;
                continue;
            }

            // A semicolon starts a comment extending to the end of the line.
            if (current == ';') {
                while (index < sourceLength && source.charAt(index) != '\n') {
                    index++;
                    column++;
                }
                continue;
            }

            int tokenLine = line;
            int tokenColumn = column;
            int start = index;

            while (index < sourceLength) {
                current = source.charAt(index);
                if (isWhitespace(current) || current == ';') {
                    break;
                }
                index++;
                column++;
            }

            int length = index - start;
            Op op = keywordAt(source, start, length);
            if (op == null) {
                String value = source.substring(start, index);
                throw new CompilationException(
                        "unknown keyword '" + value + "' at line " + tokenLine
                                + ", column " + tokenColumn);
            }
            tokens.add(new Token(op, tokenLine, tokenColumn));
        }

        return tokens;
    }

    private static boolean isWhitespace(char c) {
        // The language specification defines TAB, LF, CR and space as separators.
        return c == '\t' || c == '\n' || c == '\r' || c == ' ';
    }

    /** Resolves a source slice without allocating a String for every valid token. */
    private static Op keywordAt(String source, int start, int length) {
        return switch (length) {
            case 1 -> source.charAt(start) == 'i' ? Op.I : null;
            case 2 -> source.regionMatches(start, "by", 0, 2) ? Op.BY : null;
            case 3 -> {
                if (source.regionMatches(start, "use", 0, 3)) yield Op.USE;
                if (source.regionMatches(start, "btw", 0, 3)) yield Op.BTW;
                if (source.regionMatches(start, "the", 0, 3)) yield Op.THE;
                if (source.regionMatches(start, "way", 0, 3)) yield Op.WAY;
                yield null;
            }
            case 4 -> source.regionMatches(start, "arch", 0, 4) ? Op.ARCH : null;
            case 5 -> source.regionMatches(start, "linux", 0, 5) ? Op.LINUX : null;
            case 6 -> source.regionMatches(start, "gentoo", 0, 6) ? Op.GENTOO : null;
            default -> null;
        };
    }

    private static String compile(List<Token> tokens) throws CompilationException {
        int tokenCount = tokens.size();
        int[] matchingLoop = new int[tokenCount];
        Arrays.fill(matchingLoop, -1);
        Deque<Integer> openLoops = new ArrayDeque<>();

        // Validate and pair all loops before emitting any C code.
        for (int i = 0; i < tokenCount; i++) {
            Op op = tokens.get(i).op;
            if (op == Op.THE) {
                openLoops.push(i);
            } else if (op == Op.WAY) {
                if (openLoops.isEmpty()) {
                    Token token = tokens.get(i);
                    throw new CompilationException("unexpected 'way' at line "
                            + token.line + ", column " + token.column
                            + " (there is no matching 'the')");
                }
                int start = openLoops.pop();
                matchingLoop[start] = i;
                matchingLoop[i] = start;
            }
        }
        if (!openLoops.isEmpty()) {
            Token token = tokens.get(openLoops.peekLast());
            throw new CompilationException("unclosed 'the' at line " + token.line
                    + ", column " + token.column + " (missing 'way')");
        }

        // Prefix sums make simple-loop detection O(n), even with nested loops.
        int[] arithmeticDeltaPrefix = new int[tokenCount + 1];
        int[] nonArithmeticPrefix = new int[tokenCount + 1];
        for (int i = 0; i < tokenCount; i++) {
            Op op = tokens.get(i).op;
            arithmeticDeltaPrefix[i + 1] = arithmeticDeltaPrefix[i]
                    + (op == Op.ARCH ? 1 : op == Op.LINUX ? -1 : 0);
            nonArithmeticPrefix[i + 1] = nonArithmeticPrefix[i]
                    + (op == Op.ARCH || op == Op.LINUX ? 0 : 1);
        }

        StringBuilder out = new StringBuilder(32_768);

        out.append("/* Generated by CompilerSparseTape.java. Sparse tape reset and affine-loop optimizations enabled. */\n")
                .append("#include <stdint.h>\n")
                .append("#include <stdio.h>\n")
                .append("#include <stdlib.h>\n")
                .append("#include <string.h>\n")
                .append("\n")
                .append("#ifdef SERVER_THREAD_LOCAL\n")
                .append("#define SERVER_STATE static _Thread_local\n")
                .append("#else\n")
                .append("#define SERVER_STATE static\n")
                .append("#endif\n")
                .append("\n")
                .append("#define MEMORY_SIZE ").append(MEMORY_SIZE).append("\n")
                .append("\n")
                .append("typedef struct {\n")
                .append("    char *method;\n")
                .append("    char *path;\n")
                .append("    char *body;\n")
                .append("    char *headers;\n")
                .append("} request_context_t;\n")
                .append("\n")
                .append("typedef struct {\n")
                .append("    int status;\n")
                .append("    char *content_type;\n")
                .append("    char *body;\n")
                .append("} response_context_t;\n")
                .append("\n")
                .append("typedef void (*server_query_handler_t)(const char *sql, size_t length);\n")
                .append("\n")
                .append("SERVER_STATE char *server_output = NULL;\n")
                .append("SERVER_STATE size_t server_output_length = 0;\n")
                .append("SERVER_STATE size_t server_output_capacity = 0;\n")
                .append("\n")
                .append("SERVER_STATE char *server_response = NULL;\n")
                .append("SERVER_STATE size_t server_response_length = 0;\n")
                .append("SERVER_STATE size_t server_response_capacity = 0;\n")
                .append("SERVER_STATE int server_has_response = 0;\n")
                .append("SERVER_STATE int server_response_status = 200;\n")
                .append("\n")
                .append("SERVER_STATE char *server_line = NULL;\n")
                .append("SERVER_STATE size_t server_line_length = 0;\n")
                .append("SERVER_STATE size_t server_line_capacity = 0;\n")
                .append("\n")
                .append("SERVER_STATE int server_capture = 0;\n")
                .append("SERVER_STATE int server_protocol = 0;\n")
                .append("\n")
                .append("SERVER_STATE char *server_input = NULL;\n")
                .append("SERVER_STATE size_t server_input_length = 0;\n")
                .append("SERVER_STATE size_t server_input_position = 0;\n")
                .append("SERVER_STATE size_t server_input_capacity = 0;\n")
                .append("\n")
                .append("/* Sparse reset: revisit only cells touched by the previous run. */\n")
                .append("SERVER_STATE uint8_t server_tape[MEMORY_SIZE];\n")
                .append("SERVER_STATE uint8_t server_touched[(MEMORY_SIZE + 7U) / 8U];\n")
                .append("SERVER_STATE uint16_t server_dirty_bytes[(MEMORY_SIZE + 7U) / 8U];\n")
                .append("SERVER_STATE size_t server_dirty_count;\n")
                .append("static inline void server_mark_cell(size_t index) {\n")
                .append("    size_t byte_index = index >> 3;\n")
                .append("    uint8_t *slot = &server_touched[byte_index];\n")
                .append("    if (*slot == 0U) {\n")
                .append("        server_dirty_bytes[server_dirty_count++] = (uint16_t) byte_index;\n")
                .append("    }\n")
                .append("    *slot |= (uint8_t) (1U << (index & 7U));\n")
                .append("}\n")
                .append("static void server_tape_reset(void) {\n")
                .append("    while (server_dirty_count != 0U) {\n")
                .append("        size_t byte_index = server_dirty_bytes[--server_dirty_count];\n")
                .append("        unsigned int pending = server_touched[byte_index];\n")
                .append("        while (pending != 0U) {\n")
                .append("            unsigned int bit = (unsigned int) __builtin_ctz(pending);\n")
                .append("            server_tape[(byte_index << 3) + bit] = 0;\n")
                .append("            pending &= pending - 1U;\n")
                .append("        }\n")
                .append("        server_touched[byte_index] = 0;\n")
                .append("    }\n")
                .append("}\n")
                .append("\n")
                .append("SERVER_STATE server_query_handler_t server_query_handler = NULL;\n")
                .append("\n")
                .append("static int server_reserve(char **buffer, size_t *capacity, size_t needed) {\n")
                .append("    if (needed <= *capacity) {\n")
                .append("        return 0;\n")
                .append("    }\n")
                .append("    size_t new_capacity = *capacity == 0 ? 256 : *capacity;\n")
                .append("    while (new_capacity < needed) {\n")
                .append("        if (new_capacity > SIZE_MAX / 2) {\n")
                .append("            new_capacity = needed;\n")
                .append("            break;\n")
                .append("        }\n")
                .append("        new_capacity *= 2;\n")
                .append("    }\n")
                .append("    char *new_buffer = (char *) realloc(*buffer, new_capacity);\n")
                .append("    if (new_buffer == NULL) {\n")
                .append("        perror(\"realloc\");\n")
                .append("        return -1;\n")
                .append("    }\n")
                .append("    *buffer = new_buffer;\n")
                .append("    *capacity = new_capacity;\n")
                .append("    return 0;\n")
                .append("}\n")
                .append("\n")
                .append("static int server_buffer_append(char **buffer, size_t *length, size_t *capacity, const char *data, size_t data_length) {\n")
                .append("    if (*length == SIZE_MAX || data_length > SIZE_MAX - *length - 1U\n")
                .append("            || server_reserve(buffer, capacity, *length + data_length + 1U) != 0) {\n")
                .append("        return -1;\n")
                .append("    }\n")
                .append("    memcpy(*buffer + *length, data, data_length);\n")
                .append("    *length += data_length;\n")
                .append("    (*buffer)[*length] = '\\0';\n")
                .append("    return 0;\n")
                .append("}\n")
                .append("\n")
                .append("static int server_buffer_append_char(char **buffer, size_t *length, size_t *capacity, int value) {\n")
                .append("    char character = (char) value;\n")
                .append("    return server_buffer_append(buffer, length, capacity, &character, 1);\n")
                .append("}\n")
                .append("\n")
                .append("static void server_set_input(const char *data, size_t length) {\n")
                .append("    server_input_length = 0;\n")
                .append("    server_input_position = 0;\n")
                .append("    if (server_reserve(&server_input, &server_input_capacity, length + 1) != 0) {\n")
                .append("        return;\n")
                .append("    }\n")
                .append("    if (length > 0) {\n")
                .append("        memcpy(server_input, data, length);\n")
                .append("    }\n")
                .append("    server_input[length] = '\\0';\n")
                .append("    server_input_length = length;\n")
                .append("}\n")
                .append("\n")
                .append("void server_append_input(const char *data, size_t length) {\n")
                .append("    if (server_buffer_append(&server_input, &server_input_length, &server_input_capacity, data, length) != 0) {\n")
                .append("        return;\n")
                .append("    }\n")
                .append("}\n")
                .append("\n")
                .append("void server_set_request_input(const char *data, size_t length) {\n")
                .append("    server_set_input(data, length);\n")
                .append("}\n")
                .append("\n")
                .append("void server_set_query_handler(server_query_handler_t handler) {\n")
                .append("    server_query_handler = handler;\n")
                .append("}\n")
                .append("\n")
                .append("static int server_handle_command(size_t command_length) {\n")
                .append("    if (command_length == 0) {\n")
                .append("        return 0;\n")
                .append("    }\n")
                .append("    if (server_line[0] == 'R') {\n")
                .append("        server_response_length = 0;\n")
                .append("        if (server_response != NULL) {\n")
                .append("            server_response[0] = '\\0';\n")
                .append("        }\n")
                .append("        size_t body_offset = 1;\n")
                .append("        int status = 200;\n")
                .append("        if (command_length >= 2) {\n")
                .append("            switch (server_line[1]) {\n")
                .append("                case '1': status = 200; body_offset = 2; break;\n")
                .append("                case '2': status = 201; body_offset = 2; break;\n")
                .append("                case '3': status = 400; body_offset = 2; break;\n")
                .append("                case '4': status = 401; body_offset = 2; break;\n")
                .append("                case '5': status = 404; body_offset = 2; break;\n")
                .append("                case '6': status = 500; body_offset = 2; break;\n")
                .append("                case '7': status = 503; body_offset = 2; break;\n")
                .append("                default: break;\n")
                .append("            }\n")
                .append("        }\n")
                .append("        server_response_status = status;\n")
                .append("        if (server_buffer_append(&server_response, &server_response_length, &server_response_capacity,\n")
                .append("                                 server_line + body_offset, command_length - body_offset) != 0) {\n")
                .append("            return -1;\n")
                .append("        }\n")
                .append("        server_has_response = 1;\n")
                .append("        return 1;\n")
                .append("    }\n")
                .append("    if (server_line[0] == 'S') {\n")
                .append("        if (server_query_handler != NULL) {\n")
                .append("            server_query_handler(server_line + 1, command_length - 1);\n")
                .append("        }\n")
                .append("        return 1;\n")
                .append("    }\n")
                .append("    return 0;\n")
                .append("}\n")
                .append("\n")
                .append("static int server_emit(int value) {\n")
                .append("    if (!server_capture) {\n")
                .append("        if (putchar(value) == EOF) {\n")
                .append("            perror(\"putchar\");\n")
                .append("            return -1;\n")
                .append("        }\n")
                .append("        return 0;\n")
                .append("    }\n")
                .append("    if (!server_protocol) {\n")
                .append("        return server_buffer_append_char(&server_output, &server_output_length, &server_output_capacity, value);\n")
                .append("    }\n")
                .append("    if (server_buffer_append_char(&server_line, &server_line_length, &server_line_capacity, value) != 0) {\n")
                .append("        return -1;\n")
                .append("    }\n")
                .append("    if (value == ';' && server_line_length > 0 && server_line[0] == 'S') {\n")
                .append("        if (server_handle_command(server_line_length) != 1) {\n")
                .append("            return -1;\n")
                .append("        }\n")
                .append("        server_line_length = 0;\n")
                .append("        if (server_line != NULL) {\n")
                .append("            server_line[0] = '\\0';\n")
                .append("        }\n")
                .append("        return 0;\n")
                .append("    }\n")
                .append("    if (value == '\\n') {\n")
                .append("        size_t command_length = server_line_length;\n")
                .append("        if (command_length > 0 && server_line[command_length - 1] == '\\n') {\n")
                .append("            command_length--;\n")
                .append("        }\n")
                .append("        if (command_length > 0 && server_line[command_length - 1] == '\\r') {\n")
                .append("            command_length--;\n")
                .append("        }\n")
                .append("        int handled = server_handle_command(command_length);\n")
                .append("        if (handled < 0) {\n")
                .append("            return -1;\n")
                .append("        }\n")
                .append("        if (handled == 0) {\n")
                .append("            if (server_buffer_append(&server_output, &server_output_length, &server_output_capacity,\n")
                .append("                                     server_line, server_line_length) != 0) {\n")
                .append("                return -1;\n")
                .append("            }\n")
                .append("        }\n")
                .append("        server_line_length = 0;\n")
                .append("        if (server_line != NULL) {\n")
                .append("            server_line[0] = '\\0';\n")
                .append("        }\n")
                .append("    }\n")
                .append("    return 0;\n")
                .append("}\n")
                .append("\n")
                .append("static void server_flush_command(void) {\n")
                .append("    if (server_line_length > 0) {\n")
                .append("        if (server_handle_command(server_line_length) == 0) {\n")
                .append("            server_buffer_append(&server_output, &server_output_length, &server_output_capacity,\n")
                .append("                                 server_line, server_line_length);\n")
                .append("        }\n")
                .append("        server_line_length = 0;\n")
                .append("        if (server_line != NULL) {\n")
                .append("            server_line[0] = '\\0';\n")
                .append("        }\n")
                .append("    }\n")
                .append("}\n")
                .append("\n")
                .append("static int server_read(void) {\n")
                .append("    if (server_input != NULL) {\n")
                .append("        if (server_input_position >= server_input_length) {\n")
                .append("            return EOF;\n")
                .append("        }\n")
                .append("        return (unsigned char) server_input[server_input_position++];\n")
                .append("    }\n")
                .append("    return getchar();\n")
                .append("}\n")
                .append("\n")
                .append("static int server_run(void) {\n")
                .append("    server_tape_reset();\n")
                .append("    size_t dp = 0;\n\n");

        for (int i = 0; i < tokenCount; ) {
            Token token = tokens.get(i);
            Op op = token.op;
            int end = i + 1;

            // Combine adjacent pointer moves in the same direction. The generated
            // check reports the original instruction that would first fail.
            if (op == Op.I || op == Op.USE) {
                while (end < tokenCount && tokens.get(end).op == op) {
                    end++;
                }
                int count = end - i;
                appendGroupComment(out, i, end, token, op.keyword);
                if (op == Op.I) {
                    out.append("    if ((size_t) ").append(count)
                            .append(" > (size_t)(MEMORY_SIZE - 1) - dp) {\n")
                            .append("        fprintf(stderr, \"Runtime error: data pointer out of bounds at instruction %zu (i).\\n\", ")
                            .append("(size_t) (MEMORY_SIZE - 1) - dp + (size_t) ").append(i).append(");\n")
                            .append("        return EXIT_FAILURE;\n")
                            .append("    }\n")
                            .append("    dp += (size_t) ").append(count).append(";\n");
                } else {
                    out.append("    if ((size_t) ").append(count).append(" > dp) {\n")
                            .append("        fprintf(stderr, \"Runtime error: data pointer out of bounds at instruction %zu (use).\\n\", ")
                            .append("(size_t) ").append(i).append(" + dp);\n")
                            .append("        return EXIT_FAILURE;\n")
                            .append("    }\n")
                            .append("    dp -= (size_t) ").append(count).append(";\n");
                }
                out.append('\n');
                i = end;
                continue;
            }

            // Consecutive increments/decrements are equivalent modulo 256.
            if (op == Op.ARCH || op == Op.LINUX) {
                int delta = op == Op.ARCH ? 1 : -1;
                while (end < tokenCount) {
                    Op next = tokens.get(end).op;
                    if (next != Op.ARCH && next != Op.LINUX) break;
                    delta += next == Op.ARCH ? 1 : -1;
                    end++;
                }
                delta &= 0xff;
                if (delta > 127) delta -= 256;
                if (delta != 0) {
                    appendGroupComment(out, i, end, token, "cell arithmetic");
                    out.append("    server_mark_cell(dp);\n");
                    if (delta > 0) {
                        out.append("    server_tape[dp] = (uint8_t)(server_tape[dp] + ")
                                .append(delta).append(");\n\n");
                    } else {
                        out.append("    server_tape[dp] = (uint8_t)(server_tape[dp] - ")
                                .append(-delta).append(");\n\n");
                    }
                }
                i = end;
                continue;
            }

            // Linear transfer loops (for example [->+<]) are equivalent to
            // one arithmetic update per destination cell and a source clear.
            if (op == Op.THE) {
                int close = matchingLoop[i];
                TransferLoop transfer = analyzeTransferLoop(tokens, i + 1, close);
                if (transfer != null) {
                    appendGroupComment(out, i, close + 1, token, "affine transfer loop");
                    out.append("    if (server_tape[dp] != 0) {\n");
                    if (transfer.minOffset < 0 || transfer.maxOffset > 0) {
                        out.append("        if (");
                        if (transfer.minOffset < 0) {
                            out.append("dp < (size_t) ").append(-transfer.minOffset);
                        }
                        if (transfer.minOffset < 0 && transfer.maxOffset > 0) {
                            out.append(" || ");
                        }
                        if (transfer.maxOffset > 0) {
                            out.append("dp > (size_t)(MEMORY_SIZE - 1U - ")
                                    .append(transfer.maxOffset).append("U)");
                        }
                        out.append(") {\n")
                                .append("            fprintf(stderr, \"Runtime error: data pointer out of bounds in optimized loop at instruction %zu.\\n\", (size_t) ")
                                .append(i).append(");\n")
                                .append("            return EXIT_FAILURE;\n")
                                .append("        }\n");
                    }
                    out.append("        size_t transfer_value = (size_t) server_tape[dp];\n")
                            .append("        server_mark_cell(dp);\n")
                            .append("        server_tape[dp] = 0;\n");
                    for (Map.Entry<Integer, Integer> entry : transfer.deltas.entrySet()) {
                        int offset = entry.getKey();
                        int delta = entry.getValue();
                        if (offset == 0 || delta == 0) continue;
                        String cell = offset > 0
                                ? "dp + (size_t) " + offset
                                : "dp - (size_t) " + (-offset);
                        out.append("        server_mark_cell(").append(cell).append(");\n")
                                .append("        server_tape[").append(cell)
                                .append("] = (uint8_t)(server_tape[").append(cell)
                                .append("] + (uint8_t)(transfer_value * ")
                                .append(delta).append("U));\n");
                    }
                    out.append("    }\n\n");
                    i = close + 1;
                    continue;
                }

                // Odd arithmetic-only loops always clear an 8-bit cell.
                int barrierCount = nonArithmeticPrefix[close]
                        - nonArithmeticPrefix[i + 1];
                int delta = arithmeticDeltaPrefix[close]
                        - arithmeticDeltaPrefix[i + 1];
                if (barrierCount == 0 && (delta & 1) != 0) {
                    appendGroupComment(out, i, close + 1, token, "clear loop");
                    out.append("    server_tape[dp] = 0;\n\n");
                    i = close + 1;
                    continue;
                }
            }

            appendGroupComment(out, i, end, token, op.keyword);
            switch (op) {
                case BTW -> out.append("    if (server_emit((int) server_tape[dp]) != 0) {\n")
                        .append("        fprintf(stderr, \"Runtime error: output failed at instruction ")
                        .append(i)
                        .append(" (btw).\\n\");\n")
                        .append("        return EXIT_FAILURE;\n")
                        .append("    }\n");
                case BY -> out.append("    {\n")
                        .append("        int input_char = server_read();\n")
                        .append("        if (input_char == EOF) {\n")
                        .append("            if (server_input == NULL && ferror(stdin)) {\n")
                        .append("                perror(\"getchar\");\n")
                        .append("            } else {\n")
                        .append("                fprintf(stderr, \"Runtime error: end of input at instruction ")
                        .append(i)
                        .append(" (by).\\n\");\n")
                        .append("            }\n")
                        .append("            return EXIT_FAILURE;\n")
                        .append("        }\n")
                        .append("        server_mark_cell(dp);\n")
                        .append("        server_tape[dp] = (uint8_t) input_char;\n")
                        .append("    }\n");
                case THE -> out.append("    while (server_tape[dp] != 0) {\n");
                case WAY -> out.append("    }\n");
                case GENTOO -> out.append("    fprintf(stderr, \"gentoo: ip = %zu; dp = %zu; *dp = %u\\n\", (size_t) ")
                        .append(i)
                        .append(", dp, (unsigned int) server_tape[dp]);\n");
                default -> throw new AssertionError("Unreachable operation: " + op);
            }
            out.append('\n');
            i++;
        }

        out.append("    return EXIT_SUCCESS;\n")
                .append("}\n")
                .append("\n")
                .append("int server_main(void) {\n")
                .append("    free(server_input);\n")
                .append("    server_input = NULL;\n")
                .append("    server_input_length = 0;\n")
                .append("    server_input_position = 0;\n")
                .append("    server_input_capacity = 0;\n")
                .append("    server_capture = 0;\n")
                .append("    server_protocol = 0;\n")
                .append("    return server_run();\n")
                .append("}\n")
                .append("\n")
                .append("void server_create_request_context(request_context_t *ctx, char *data) {\n")
                .append("    if (ctx == NULL) return;\n")
                .append("    memset(ctx, 0, sizeof(*ctx));\n")
                .append("    if (data == NULL) return;\n")
                .append("\n")
                .append("    char *line_end = strpbrk(data, \"\\r\\n\");\n")
                .append("    if (line_end == NULL) return;\n")
                .append("    size_t request_line_length = (size_t) (line_end - data);\n")
                .append("    char *first_space = (char *) memchr(data, ' ', request_line_length);\n")
                .append("    if (first_space == NULL) return;\n")
                .append("    char *path_start = first_space + 1;\n")
                .append("    size_t path_available = request_line_length - (size_t) (path_start - data);\n")
                .append("    char *second_space = (char *) memchr(path_start, ' ', path_available);\n")
                .append("    if (second_space == NULL) return;\n")
                .append("\n")
                .append("    *first_space = '\\0';\n")
                .append("    *second_space = '\\0';\n")
                .append("    ctx->method = data;\n")
                .append("    ctx->path = path_start;\n")
                .append("\n")
                .append("    char *header_start = line_end;\n")
                .append("    if (*header_start == '\\r') header_start++;\n")
                .append("    if (*header_start == '\\n') header_start++;\n")
                .append("    char *separator = strstr(header_start, \"\\r\\n\\r\\n\");\n")
                .append("    if (separator != NULL) {\n")
                .append("        *separator = '\\0';\n")
                .append("        ctx->headers = header_start;\n")
                .append("        ctx->body = separator + 4;\n")
                .append("    } else {\n")
                .append("        ctx->headers = header_start;\n")
                .append("    }\n")
                .append("}\n")
                .append("\n")
                .append("void server_free_request_context(request_context_t *ctx) {\n")
                .append("    if (ctx != NULL) memset(ctx, 0, sizeof(*ctx));\n")
                .append("}\n")
                .append("\n")
                .append("void server_generate_response(response_context_t *resp) {\n")
                .append("    if (resp == NULL) {\n")
                .append("        return;\n")
                .append("    }\n")
                .append("    resp->status = 200;\n")
                .append("    resp->content_type = \"text/plain\";\n")
                .append("    resp->body = \"\";\n")
                .append("\n")
                .append("    server_response_status = 200;\n")
                .append("    server_capture = 1;\n")
                .append("    server_protocol = 1;\n")
                .append("    server_output_length = 0;\n")
                .append("    server_response_length = 0;\n")
                .append("    server_line_length = 0;\n")
                .append("    server_has_response = 0;\n")
                .append("    if (server_output != NULL) {\n")
                .append("        server_output[0] = '\\0';\n")
                .append("    }\n")
                .append("    if (server_response != NULL) {\n")
                .append("        server_response[0] = '\\0';\n")
                .append("    }\n")
                .append("    if (server_line != NULL) {\n")
                .append("        server_line[0] = '\\0';\n")
                .append("    }\n")
                .append("\n")
                .append("    int result = server_run();\n")
                .append("    server_flush_command();\n")
                .append("    server_capture = 0;\n")
                .append("    server_protocol = 0;\n")
                .append("\n")
                .append("    if (server_has_response) {\n")
                .append("        if (server_response == NULL) {\n")
                .append("            server_response = (char *) calloc(1, 1);\n")
                .append("        }\n")
                .append("        if (server_response != NULL) {\n")
                .append("            resp->body = server_response;\n")
                .append("            resp->content_type = \"application/json\";\n")
                .append("            resp->status = server_response_status;\n")
                .append("        }\n")
                .append("    } else {\n")
                .append("        if (server_output == NULL) {\n")
                .append("            server_output = (char *) calloc(1, 1);\n")
                .append("        }\n")
                .append("        if (server_output != NULL) {\n")
                .append("            resp->body = server_output;\n")
                .append("        }\n")
                .append("    }\n")
                .append("\n")
                .append("    if (result != EXIT_SUCCESS && !server_has_response) {\n")
                .append("        resp->status = 500;\n")
                .append("    }\n")
                .append("}\n");
        return out.toString();
    }

    /** Recognizes affine loops with only pointer moves and byte arithmetic. */
    private static TransferLoop analyzeTransferLoop(List<Token> tokens, int start, int close) {
        Map<Integer, Integer> deltas = new HashMap<>();
        int offset = 0;
        int minOffset = 0;
        int maxOffset = 0;

        for (int i = start; i < close; i++) {
            Op op = tokens.get(i).op;
            switch (op) {
                case I -> {
                    offset++;
                    if (offset > maxOffset) maxOffset = offset;
                }
                case USE -> {
                    offset--;
                    if (offset < minOffset) minOffset = offset;
                }
                case ARCH -> deltas.merge(offset, 1, Integer::sum);
                case LINUX -> deltas.merge(offset, -1, Integer::sum);
                default -> { return null; }
            }
        }

        if (offset != 0 || Math.floorMod(deltas.getOrDefault(0, 0), 256) != 255) {
            return null;
        }
        deltas.replaceAll((ignored, value) -> Math.floorMod(value, 256));
        deltas.entrySet().removeIf(entry -> entry.getValue() == 0);
        if (deltas.size() <= 1) return null;
        return new TransferLoop(deltas, minOffset, maxOffset);
    }

    private static final class TransferLoop {
        private final Map<Integer, Integer> deltas;
        private final int minOffset;
        private final int maxOffset;

        private TransferLoop(Map<Integer, Integer> deltas, int minOffset, int maxOffset) {
            this.deltas = deltas;
            this.minOffset = minOffset;
            this.maxOffset = maxOffset;
        }
    }

    private static void appendGroupComment(StringBuilder out, int start, int end,
                                           Token token, String description) {
        out.append("    /* instruction");
        if (end - start != 1) {
            out.append("s ").append(start).append("..").append(end - 1);
        } else {
            out.append(' ').append(start);
        }
        out.append(" (line ").append(token.line).append(", column ")
                .append(token.column).append("): ").append(description).append(" */\n");
    }

    private enum Op {
        I("i"), USE("use"), ARCH("arch"), LINUX("linux"), BTW("btw"),
        BY("by"), THE("the"), WAY("way"), GENTOO("gentoo");

        private final String keyword;

        Op(String keyword) {
            this.keyword = keyword;
        }
    }

    private static final class Token {
        private final Op op;
        private final int line;
        private final int column;

        private Token(Op op, int line, int column) {
            this.op = op;
            this.line = line;
            this.column = column;
        }
    }

    private static final class CompilationException extends Exception {
        private static final long serialVersionUID = 1L;

        private CompilationException(String message) {
            super(message);
        }
    }
}
