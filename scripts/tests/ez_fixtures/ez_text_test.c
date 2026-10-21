#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "device_media_control.h"

static device_control_envelope_t* frame(const char* type, const char* json) {
    cJSON* payload = cJSON_Parse(json);
    assert(payload);
    char* wire = malloc(DEVICE_CONTROL_MESSAGE_MAX_BYTES + 1);
    size_t size = 0;
    assert(device_control_envelope_encode("message-1", type, payload, wire,
                                          DEVICE_CONTROL_MESSAGE_MAX_BYTES + 1,
                                          &size) == DEVICE_PROTOCOL_RESULT_OK);
    device_control_envelope_t* envelope = NULL;
    assert(device_control_envelope_decode(wire, size, &envelope) == DEVICE_PROTOCOL_RESULT_OK);
    cJSON_Delete(payload);
    free(wire);
    return envelope;
}

int main(void) {
    const char* nul =
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\",\"type\":\"transcript\","
        "\"payload\":{\"client_input_id\":\"i\",\"text\":\"a\\u0000b\"}}";
    device_control_envelope_t* invalid = NULL;
    assert(device_control_envelope_decode(nul, strlen(nul), &invalid) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    assert(invalid == NULL);
    device_media_transcript_t transcript;
    device_media_text_output_t output;
    device_control_envelope_t* e =
        frame("transcript", "{\"client_input_id\":\"input-1\",\"text\":\"你好，BOX-3\"}");
    assert(device_media_control_decode_transcript(e, &transcript) == DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(transcript.client_input_id, "input-1") == 0);
    assert(strcmp(transcript.text, "你好，BOX-3") == 0);
    device_control_envelope_release(e);
    e = frame(
        "text_output",
        "{\"run_id\":\"run-1\",\"output_id\":\"out-1\",\"text\":\"中文与 English\\n第二行。\"}");
    assert(device_media_control_decode_text_output(e, &output) == DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(output.text, "中文与 English\n第二行。") == 0);
    assert(strcmp(output.run_id, "run-1") == 0 && strcmp(output.output_id, "out-1") == 0);
    assert(device_media_control_decode_transcript(e, &transcript) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);
    device_control_envelope_release(e);
    e = frame("text_output", "{\"run_id\":\"r\",\"output_id\":\"o\",\"text\":\"\\\\u0000\"}");
    assert(device_media_control_decode_text_output(e, &output) == DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(output.text, "\\u0000") == 0);
    device_control_envelope_release(e);
    const char* bad[] = {"{\"run_id\":\"r\",\"output_id\":\"o\",\"text\":\" \"}",
                         "{\"run_id\":\"r\",\"output_id\":\"o\",\"text\":4}",
                         "{\"run_id\":\"r\",\"text\":\"x\"}",
                         "{\"run_id\":\"r\",\"output_id\":\"o\",\"text\":\"x\",\"extra\":true}",
                         "{\"run_id\":\"r\",\"output_id\":\"o\",\"text\":\"x\",\"text\":\"y\"}",
                         "{\"run_id\":\"r\",\"output_id\":\"o\",\"text\":\"\xc0\xaf\"}",
                         "{\"run_id\":\"r\",\"output_id\":\"o\",\"text\":\"\xed\xa0\x80\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        e = frame("text_output", bad[i]);
        assert(device_media_control_decode_text_output(e, &output) ==
               DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
        assert(output.text == NULL);
        device_control_envelope_release(e);
    }
    char* text = malloc(DEVICE_MEDIA_TEXT_MAX_BYTES + 2);
    memset(text, 'a', DEVICE_MEDIA_TEXT_MAX_BYTES + 1);
    text[DEVICE_MEDIA_TEXT_MAX_BYTES + 1] = 0;
    for (size_t n = DEVICE_MEDIA_TEXT_MAX_BYTES; n <= DEVICE_MEDIA_TEXT_MAX_BYTES + 1; ++n) {
        text[n] = 0;
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "client_input_id", "input-1");
        cJSON_AddStringToObject(payload, "text", text);
        char* json = cJSON_PrintUnformatted(payload);
        e = frame("transcript", json);
        assert(device_media_control_decode_transcript(e, &transcript) ==
               (n == DEVICE_MEDIA_TEXT_MAX_BYTES ? DEVICE_PROTOCOL_RESULT_OK
                                                 : DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE));
        device_control_envelope_release(e);
        cJSON_free(json);
        cJSON_Delete(payload);
        text[n] = 'a';
    }
    free(text);
    char wire[2048];
    size_t length = 0;
    assert(device_media_control_encode_listen_start(
               "m", "input-1", 1, DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO, wire, sizeof(wire),
               &length) == DEVICE_PROTOCOL_RESULT_OK);
    assert(strstr(wire, "text_and_audio"));
}
