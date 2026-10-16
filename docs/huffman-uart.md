# Huffman-compressed UART output

The AVR has little Flash and SRAM, so fixed firmware messages are compressed at build time. The decoder runs on the PC, not on the Arduino. Interactive text and binary guest uploads use separate framing and are not forced through the Huffman codec.

## Build pipeline

1. Fixed messages are listed in the logging manifest.
2. The generator builds one shared Huffman codebook.
3. It writes the generated C tables used by the firmware and the matching Python table used by the serial monitor.
4. Firmware sends a framed bit stream; the PC reconstructs the original text.

The firmware and host decoder must always come from the same build. A stale decoder can report `invalid huffman frame` even when the UART bytes are electrically correct.

Run the codec tests with:

```sh
make test-codec
```

## Stream framing

The monitor consumes a byte stream incrementally, so a frame may be split across several serial reads.

- Ordinary nonzero bytes are raw text.
- `00 00` represents a literal zero byte.
- `00 01 <bit-count-low> <bit-count-high> <payload...>` is a Huffman frame.
- `00 03 <length> <payload...>` is a length-delimited dynamic-text frame.

Fixed strings benefit most from compression. Shell prompts, command echoes, guest names, numbers, and other runtime-generated data use raw or length-delimited output.

## Why changing one message changes the table

Huffman coding assigns shorter bit patterns to frequent symbols. Adding or removing a message changes the frequency distribution and can therefore change every code. The generator deliberately rebuilds both sides together.

## Failure diagnosis

If the monitor reports an invalid frame:

1. Stop every process that owns the COM port.
2. Rebuild the firmware and generated decoder from the same checkout.
3. Flash the new firmware.
4. Start the repository's monitor again.

Do not decode arbitrary binary loader traffic as text. Loader mode has its own framing, CRC, acknowledgement, and retry rules.
