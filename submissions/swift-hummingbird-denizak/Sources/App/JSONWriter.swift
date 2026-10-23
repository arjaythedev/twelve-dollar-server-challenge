// Hand-rolled JSON string writing into a ByteBuffer.
//
// The spec allows leaving non-ASCII and <>&/ unescaped, so we only escape what
// JSON requires: `"` and `\` and control characters. Typical load-test payloads
// are plain ASCII with no escapes at all, which hits the fast scan-and-copy path.

import NIOCore

private let hexLower: [UInt8] = Array("0123456789abcdef".utf8)

/// Write `bytes` as a complete JSON string literal (including the quotes).
func writeJSONString(_ buffer: inout ByteBuffer, _ bytes: UnsafeRawBufferPointer) {
    buffer.writeInteger(UInt8(ascii: "\""))
    let count = bytes.count
    var start = 0
    var index = 0
    while index < count {
        let byte = bytes[index]
        let needsEscape = byte == 0x22 || byte == 0x5C || byte < 0x20
        if needsEscape {
            if index > start {
                bytes.copyBytes(to: &buffer, from: start..<index)
            }
            switch byte {
            case 0x22: // "
                buffer.writeBytes([0x5C, 0x22])
            case 0x5C: // backslash
                buffer.writeBytes([0x5C, 0x5C])
            case 0x08: // \b
                buffer.writeBytes([0x5C, 0x62])
            case 0x09: // \t
                buffer.writeBytes([0x5C, 0x74])
            case 0x0A: // \n
                buffer.writeBytes([0x5C, 0x6E])
            case 0x0C: // \f
                buffer.writeBytes([0x5C, 0x66])
            case 0x0D: // \r
                buffer.writeBytes([0x5C, 0x72])
            default: // other control character: \u00XX
                buffer.writeBytes([
                    0x5C, 0x75, 0x30, 0x30,
                    hexLower[Int(byte >> 4)],
                    hexLower[Int(byte & 0x0F)],
                ])
            }
            index += 1
            start = index
        } else {
            index += 1
        }
    }
    if count > start {
        bytes.copyBytes(to: &buffer, from: start..<count)
    }
    buffer.writeInteger(UInt8(ascii: "\""))
}

/// Write a Swift string as a JSON string literal.
@inline(__always)
func writeJSONString(_ buffer: inout ByteBuffer, _ string: String) {
    var string = string
    string.withUTF8 { utf8 in
        writeJSONString(&buffer, UnsafeRawBufferPointer(start: utf8.baseAddress, count: utf8.count))
    }
}

private extension UnsafeRawBufferPointer {
    func copyBytes(to buffer: inout ByteBuffer, from range: Range<Int>) {
        withUnsafeBytes { (whole: UnsafeRawBufferPointer) in
            let sub = UnsafeRawBufferPointer(rebasing: whole[range])
            _ = buffer.writeBytes(sub)
        }
    }
}
