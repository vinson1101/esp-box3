#include "wss_frame_codec.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    const uint8_t mask[4] = {0x37U, 0xfaU, 0x21U, 0x3dU};
    const uint8_t hello[] = "Hello";
    const uint8_t expected[] = {
        0x81U, 0x85U, 0x37U, 0xfaU, 0x21U, 0x3dU, 0x7fU, 0x9fU, 0x4dU, 0x51U, 0x58U,
    };
    uint8_t encoded[32U];
    size_t encoded_length = 0U;
    assert(wss_frame_encode_client(
               WSS_OPCODE_TEXT, true, mask, hello, sizeof(hello) - 1U, encoded,
               sizeof(encoded), &encoded_length) == WSS_CODEC_RESULT_OK);
    assert(encoded_length == sizeof(expected));
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);

    const uint8_t server_header[] = {0x82U, 126U, 0x02U, 0x90U};
    wss_frame_header_t header;
    assert(wss_frame_decode_server_header(server_header, sizeof(server_header), &header) ==
           WSS_CODEC_RESULT_OK);
    assert(header.final && header.opcode == WSS_OPCODE_BINARY && header.payload_length == 656U &&
           header.header_length == sizeof(server_header));

    const uint8_t masked_server[] = {0x81U, 0x80U};
    assert(wss_frame_decode_server_header(masked_server, sizeof(masked_server), &header) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);
    const uint8_t fragmented_ping[] = {0x09U, 0x00U};
    assert(wss_frame_decode_server_header(fragmented_ping, sizeof(fragmented_ping), &header) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);
    const uint8_t noncanonical_length[] = {0x82U, 126U, 0x00U, 0x7dU};
    assert(wss_frame_decode_server_header(
               noncanonical_length, sizeof(noncanonical_length), &header) ==
           WSS_CODEC_RESULT_PROTOCOL_ERROR);

    puts("WSS frame codec tests passed");
    return 0;
}
