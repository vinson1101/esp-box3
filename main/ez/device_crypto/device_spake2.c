#include "device_spake2.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "device_transcript.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#define P256_SCALAR_BYTES 32U
#define SHA256_BYTES 32U
#define SHA256_BLOCK_BYTES 64U
#define SHA512_BYTES 64U
#define SESSION_KEY_BYTES 16U
#define CONFIRMATION_KEY_BYTES 16U
#define PASSWORD_TRANSCRIPT_MAX_BYTES 640U
#define SPAKE2_TRANSCRIPT_MAX_BYTES 1024U
#define MBEDTLS_RANDOM_FAILURE (-0x7e80)

static const uint8_t SPAKE2_M_COMPRESSED[33] = {
    0x02, 0x88, 0x6e, 0x2f, 0x97, 0xac, 0xe4, 0x6e, 0x55, 0xba, 0x9d,
    0xd7, 0x24, 0x25, 0x79, 0xf2, 0x99, 0x3b, 0x64, 0xe1, 0x6e, 0xf3,
    0xdc, 0xab, 0x95, 0xaf, 0xd4, 0x97, 0x33, 0x3d, 0x8f, 0xa1, 0x2f,
};

static const uint8_t SPAKE2_N_COMPRESSED[33] = {
    0x03, 0xd8, 0xbb, 0xd6, 0xc6, 0x39, 0xc6, 0x29, 0x37, 0xb0, 0x4d,
    0x99, 0x7f, 0x38, 0xc3, 0x77, 0x07, 0x19, 0xc6, 0x29, 0xd7, 0x01,
    0x4d, 0x49, 0xa2, 0x4b, 0x4f, 0x98, 0xba, 0xa1, 0x29, 0x2b, 0x49,
};

struct device_spake2_party_a {
    uint8_t private_scalar[P256_SCALAR_BYTES];
    uint8_t password_scalar[P256_SCALAR_BYTES];
    uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES];
    char pairing_request_id[DEVICE_SPAKE2_IDENTITY_MAX_BYTES + 1U];
    char installation_id[DEVICE_SPAKE2_IDENTITY_MAX_BYTES + 1U];
    device_random_source_t random_source;
    bool finished;
};

struct device_spake2_keys {
    uint8_t session_key[SESSION_KEY_BYTES];
    uint8_t device_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES];
    uint8_t expected_host_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES];
};

typedef struct {
    uint8_t bytes[SPAKE2_TRANSCRIPT_MAX_BYTES];
    size_t size;
    bool overflow;
} spake2_transcript_builder_t;

static bool bounded_nonempty_string(const char *value, size_t maximum_size)
{
    if (value == NULL) {
        return false;
    }
    const size_t size = strlen(value);
    return size > 0U && size <= maximum_size;
}

static device_crypto_result_t map_mbedtls_result(int result)
{
    if (result == 0) {
        return DEVICE_CRYPTO_RESULT_OK;
    }
    if (result == MBEDTLS_ERR_MPI_ALLOC_FAILED) {
        return DEVICE_CRYPTO_RESULT_RESOURCE_EXHAUSTED;
    }
    if (result == MBEDTLS_ERR_MD_ALLOC_FAILED) {
        return DEVICE_CRYPTO_RESULT_RESOURCE_EXHAUSTED;
    }
    if (result == MBEDTLS_RANDOM_FAILURE) {
        return DEVICE_CRYPTO_RESULT_RANDOM_FAILED;
    }
    return DEVICE_CRYPTO_RESULT_OPERATION_FAILED;
}

static device_crypto_result_t hash_message(
    mbedtls_md_type_t hash_type,
    const uint8_t *message,
    size_t message_size,
    uint8_t *digest)
{
    const mbedtls_md_info_t *hash = mbedtls_md_info_from_type(hash_type);
    if (hash == NULL) {
        return DEVICE_CRYPTO_RESULT_OPERATION_FAILED;
    }
    return map_mbedtls_result(mbedtls_md(hash, message, message_size, digest));
}

static int mbedtls_random_fill(void *context, unsigned char *output, size_t output_size)
{
    const device_random_source_t *source = context;
    return source != NULL && source->fill != NULL
            && source->fill(source->context, output, output_size)
        ? 0
        : MBEDTLS_RANDOM_FAILURE;
}

static bool scalar_is_valid(const mbedtls_mpi *scalar, const mbedtls_ecp_group *group)
{
    return mbedtls_mpi_cmp_int(scalar, 0) > 0
        && mbedtls_mpi_cmp_mpi(scalar, &group->N) < 0;
}

static device_crypto_result_t random_scalar(
    const device_random_source_t *random_source,
    const mbedtls_ecp_group *group,
    uint8_t encoded[P256_SCALAR_BYTES])
{
    mbedtls_mpi scalar;
    mbedtls_mpi_init(&scalar);
    device_crypto_result_t result = DEVICE_CRYPTO_RESULT_RANDOM_FAILED;
    for (size_t attempt = 0U; attempt < 128U; ++attempt) {
        if (!random_source->fill(random_source->context, encoded, P256_SCALAR_BYTES)) {
            break;
        }
        if (mbedtls_mpi_read_binary(&scalar, encoded, P256_SCALAR_BYTES) != 0) {
            result = DEVICE_CRYPTO_RESULT_RESOURCE_EXHAUSTED;
            break;
        }
        if (scalar_is_valid(&scalar, group)) {
            result = DEVICE_CRYPTO_RESULT_OK;
            break;
        }
    }
    mbedtls_mpi_free(&scalar);
    if (result != DEVICE_CRYPTO_RESULT_OK) {
        mbedtls_platform_zeroize(encoded, P256_SCALAR_BYTES);
    }
    return result;
}

static device_crypto_result_t password_scalar(
    const char *pairing_code,
    const char *pairing_request_id,
    const char *installation_id,
    const mbedtls_ecp_group *group,
    uint8_t encoded[P256_SCALAR_BYTES])
{
    uint8_t transcript[PASSWORD_TRANSCRIPT_MAX_BYTES];
    uint8_t digest[SHA512_BYTES];
    size_t transcript_size = 0U;
    device_crypto_result_t result = device_pairing_password_transcript_encode(
        pairing_code,
        pairing_request_id,
        installation_id,
        transcript,
        sizeof(transcript),
        &transcript_size);
    if (result != DEVICE_CRYPTO_RESULT_OK) {
        return result;
    }

    mbedtls_mpi wide;
    mbedtls_mpi reduced;
    mbedtls_mpi_init(&wide);
    mbedtls_mpi_init(&reduced);
    result = hash_message(MBEDTLS_MD_SHA512, transcript, transcript_size, digest);
    int operation = 0;
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_mpi_read_binary(&wide, digest, sizeof(digest));
    }
    if (result == DEVICE_CRYPTO_RESULT_OK && operation == 0) {
        operation = mbedtls_mpi_mod_mpi(&reduced, &wide, &group->N);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK && operation == 0
        && !scalar_is_valid(&reduced, group)) {
        operation = MBEDTLS_ERR_ECP_INVALID_KEY;
    }
    if (result == DEVICE_CRYPTO_RESULT_OK && operation == 0) {
        operation = mbedtls_mpi_write_binary(&reduced, encoded, P256_SCALAR_BYTES);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = map_mbedtls_result(operation);
    }
    mbedtls_mpi_free(&reduced);
    mbedtls_mpi_free(&wide);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_platform_zeroize(transcript, sizeof(transcript));
    return result;
}

static device_crypto_result_t load_point(
    mbedtls_ecp_group *group,
    mbedtls_ecp_point *point,
    const uint8_t *encoded,
    size_t encoded_size)
{
    const int read_result = mbedtls_ecp_point_read_binary(group, point, encoded, encoded_size);
    if (read_result != 0 || mbedtls_ecp_is_zero(point)
        || mbedtls_ecp_check_pubkey(group, point) != 0) {
        return DEVICE_CRYPTO_RESULT_INVALID_POINT;
    }
    return DEVICE_CRYPTO_RESULT_OK;
}

static device_crypto_result_t add_points(
    mbedtls_ecp_group *group,
    mbedtls_ecp_point *result,
    const mbedtls_ecp_point *left,
    const mbedtls_ecp_point *right)
{
    mbedtls_mpi one;
    mbedtls_mpi_init(&one);
    int operation = mbedtls_mpi_lset(&one, 1);
    if (operation == 0) {
        operation = mbedtls_ecp_muladd(group, result, &one, left, &one, right);
    }
    mbedtls_mpi_free(&one);
    return map_mbedtls_result(operation);
}

static device_crypto_result_t point_to_uncompressed(
    const mbedtls_ecp_group *group,
    const mbedtls_ecp_point *point,
    uint8_t encoded[DEVICE_SPAKE2_SHARE_BYTES])
{
    size_t encoded_size = 0U;
    const int operation = mbedtls_ecp_point_write_binary(
        group,
        point,
        MBEDTLS_ECP_PF_UNCOMPRESSED,
        &encoded_size,
        encoded,
        DEVICE_SPAKE2_SHARE_BYTES);
    if (operation != 0 || encoded_size != DEVICE_SPAKE2_SHARE_BYTES) {
        return map_mbedtls_result(operation == 0 ? MBEDTLS_ERR_ECP_BAD_INPUT_DATA : operation);
    }
    return DEVICE_CRYPTO_RESULT_OK;
}

static device_crypto_result_t create_device_share(
    const uint8_t private_scalar_bytes[P256_SCALAR_BYTES],
    const uint8_t password_scalar_bytes[P256_SCALAR_BYTES],
    const device_random_source_t *random_source,
    uint8_t encoded[DEVICE_SPAKE2_SHARE_BYTES])
{
    mbedtls_ecp_group group;
    mbedtls_ecp_point m;
    mbedtls_ecp_point private_base;
    mbedtls_ecp_point password_m;
    mbedtls_ecp_point share;
    mbedtls_mpi private_scalar_value;
    mbedtls_mpi password_scalar_value;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&m);
    mbedtls_ecp_point_init(&private_base);
    mbedtls_ecp_point_init(&password_m);
    mbedtls_ecp_point_init(&share);
    mbedtls_mpi_init(&private_scalar_value);
    mbedtls_mpi_init(&password_scalar_value);

    int operation = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    device_crypto_result_t result = map_mbedtls_result(operation);
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = load_point(&group, &m, SPAKE2_M_COMPRESSED, sizeof(SPAKE2_M_COMPRESSED));
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_mpi_read_binary(
            &private_scalar_value, private_scalar_bytes, P256_SCALAR_BYTES);
        if (operation == 0) {
            operation = mbedtls_mpi_read_binary(
                &password_scalar_value, password_scalar_bytes, P256_SCALAR_BYTES);
        }
        result = map_mbedtls_result(operation);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_ecp_mul(
            &group,
            &private_base,
            &private_scalar_value,
            &group.G,
            mbedtls_random_fill,
            (void *)random_source);
        result = map_mbedtls_result(operation);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_ecp_mul(
            &group,
            &password_m,
            &password_scalar_value,
            &m,
            mbedtls_random_fill,
            (void *)random_source);
        result = map_mbedtls_result(operation);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = add_points(&group, &share, &private_base, &password_m);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = point_to_uncompressed(&group, &share, encoded);
    }

    mbedtls_mpi_free(&password_scalar_value);
    mbedtls_mpi_free(&private_scalar_value);
    mbedtls_ecp_point_free(&share);
    mbedtls_ecp_point_free(&password_m);
    mbedtls_ecp_point_free(&private_base);
    mbedtls_ecp_point_free(&m);
    mbedtls_ecp_group_free(&group);
    return result;
}

static void write_u64_le(uint8_t destination[8], uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        destination[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void spake2_transcript_append(
    spake2_transcript_builder_t *builder,
    const uint8_t *part,
    size_t part_size)
{
    if (builder->overflow || (part_size > 0U && part == NULL)
        || part_size > sizeof(builder->bytes) - 8U
        || builder->size > sizeof(builder->bytes) - 8U - part_size) {
        builder->overflow = true;
        return;
    }
    write_u64_le(&builder->bytes[builder->size], (uint64_t)part_size);
    builder->size += 8U;
    if (part_size > 0U) {
        memcpy(&builder->bytes[builder->size], part, part_size);
        builder->size += part_size;
    }
}

static device_crypto_result_t hmac_sha256_parts(
    const uint8_t *key,
    size_t key_size,
    const uint8_t *first,
    size_t first_size,
    const uint8_t *second,
    size_t second_size,
    const uint8_t *third,
    size_t third_size,
    uint8_t output[SHA256_BYTES])
{
    if ((key_size > 0U && key == NULL) || (first_size > 0U && first == NULL)
        || (second_size > 0U && second == NULL) || (third_size > 0U && third == NULL)
        || output == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }

    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t normalized_key[SHA256_BYTES] = {0};
    const uint8_t *hmac_key = key;
    size_t hmac_key_size = key_size;
    device_crypto_result_t result = DEVICE_CRYPTO_RESULT_OK;
    if (key_size > SHA256_BLOCK_BYTES) {
        result = hash_message(MBEDTLS_MD_SHA256, key, key_size, normalized_key);
        hmac_key = normalized_key;
        hmac_key_size = sizeof(normalized_key);
    }

    uint8_t inner_pad[SHA256_BLOCK_BYTES];
    uint8_t outer_pad[SHA256_BLOCK_BYTES];
    uint8_t inner_digest[SHA256_BYTES] = {0};
    memset(inner_pad, 0x36, sizeof(inner_pad));
    memset(outer_pad, 0x5c, sizeof(outer_pad));
    for (size_t index = 0U; index < hmac_key_size; ++index) {
        inner_pad[index] ^= hmac_key[index];
        outer_pad[index] ^= hmac_key[index];
    }

    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    int operation = result == DEVICE_CRYPTO_RESULT_OK && sha256 != NULL
        ? mbedtls_md_setup(&context, sha256, 0)
        : MBEDTLS_ERR_MD_FEATURE_UNAVAILABLE;
    if (operation == 0) {
        operation = mbedtls_md_starts(&context);
    }
    if (operation == 0) {
        operation = mbedtls_md_update(&context, inner_pad, sizeof(inner_pad));
    }
    if (operation == 0 && first_size > 0U) {
        operation = mbedtls_md_update(&context, first, first_size);
    }
    if (operation == 0 && second_size > 0U) {
        operation = mbedtls_md_update(&context, second, second_size);
    }
    if (operation == 0 && third_size > 0U) {
        operation = mbedtls_md_update(&context, third, third_size);
    }
    if (operation == 0) {
        operation = mbedtls_md_finish(&context, inner_digest);
    }
    if (operation == 0) {
        operation = mbedtls_md_starts(&context);
    }
    if (operation == 0) {
        operation = mbedtls_md_update(&context, outer_pad, sizeof(outer_pad));
    }
    if (operation == 0) {
        operation = mbedtls_md_update(&context, inner_digest, sizeof(inner_digest));
    }
    if (operation == 0) {
        operation = mbedtls_md_finish(&context, output);
    }
    mbedtls_md_free(&context);
    mbedtls_platform_zeroize(inner_digest, sizeof(inner_digest));
    mbedtls_platform_zeroize(outer_pad, sizeof(outer_pad));
    mbedtls_platform_zeroize(inner_pad, sizeof(inner_pad));
    mbedtls_platform_zeroize(normalized_key, sizeof(normalized_key));
    return result == DEVICE_CRYPTO_RESULT_OK ? map_mbedtls_result(operation) : result;
}

static device_crypto_result_t derive_keys(
    const uint8_t *transcript,
    size_t transcript_size,
    const uint8_t *associated_data,
    size_t associated_data_size,
    device_spake2_keys_t *keys)
{
    uint8_t schedule[SHA256_BYTES];
    uint8_t extract_key[SHA256_BYTES];
    uint8_t confirmation_keys[SHA256_BYTES];
    static const uint8_t extract_salt[SHA256_BYTES] = {0};
    static const uint8_t confirmation_label[] = "ConfirmationKeys";
    static const uint8_t first_block = 1U;
    device_crypto_result_t result =
        hash_message(MBEDTLS_MD_SHA256, transcript, transcript_size, schedule);
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = hmac_sha256_parts(
            extract_salt,
            sizeof(extract_salt),
            &schedule[SESSION_KEY_BYTES],
            SESSION_KEY_BYTES,
            NULL,
            0U,
            NULL,
            0U,
            extract_key);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = hmac_sha256_parts(
            extract_key,
            sizeof(extract_key),
            confirmation_label,
            sizeof(confirmation_label) - 1U,
            associated_data,
            associated_data_size,
            &first_block,
            1U,
            confirmation_keys);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        memcpy(keys->session_key, schedule, sizeof(keys->session_key));
        result = hmac_sha256_parts(
            confirmation_keys,
            CONFIRMATION_KEY_BYTES,
            transcript,
            transcript_size,
            NULL,
            0U,
            NULL,
            0U,
            keys->device_confirmation);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = hmac_sha256_parts(
            &confirmation_keys[CONFIRMATION_KEY_BYTES],
            CONFIRMATION_KEY_BYTES,
            transcript,
            transcript_size,
            NULL,
            0U,
            NULL,
            0U,
            keys->expected_host_confirmation);
    }
    mbedtls_platform_zeroize(confirmation_keys, sizeof(confirmation_keys));
    mbedtls_platform_zeroize(extract_key, sizeof(extract_key));
    mbedtls_platform_zeroize(schedule, sizeof(schedule));
    return result;
}

device_crypto_result_t device_spake2_party_a_begin(
    const char *pairing_code,
    const char *pairing_request_id,
    const char *installation_id,
    const device_random_source_t *random_source,
    device_spake2_party_a_t **session,
    uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES])
{
    if (session == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    *session = NULL;
    if (device_share == NULL || random_source == NULL || random_source->fill == NULL
        || !bounded_nonempty_string(pairing_request_id, DEVICE_SPAKE2_IDENTITY_MAX_BYTES)
        || !bounded_nonempty_string(installation_id, DEVICE_SPAKE2_IDENTITY_MAX_BYTES)) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    memset(device_share, 0, DEVICE_SPAKE2_SHARE_BYTES);
    mbedtls_ecp_group group;
    mbedtls_ecp_group_init(&group);
    device_crypto_result_t result = map_mbedtls_result(
        mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1));
    device_spake2_party_a_t *created = NULL;
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        created = calloc(1U, sizeof(*created));
        if (created == NULL) {
            result = DEVICE_CRYPTO_RESULT_RESOURCE_EXHAUSTED;
        }
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        created->random_source = *random_source;
        memcpy(created->pairing_request_id, pairing_request_id, strlen(pairing_request_id) + 1U);
        memcpy(created->installation_id, installation_id, strlen(installation_id) + 1U);
        result = password_scalar(
            pairing_code,
            pairing_request_id,
            installation_id,
            &group,
            created->password_scalar);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = random_scalar(random_source, &group, created->private_scalar);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = create_device_share(
            created->private_scalar,
            created->password_scalar,
            random_source,
            created->device_share);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        memcpy(device_share, created->device_share, DEVICE_SPAKE2_SHARE_BYTES);
        *session = created;
    } else {
        device_spake2_party_a_release(created);
    }
    mbedtls_ecp_group_free(&group);
    return result;
}

device_crypto_result_t device_spake2_party_a_finish(
    device_spake2_party_a_t *session,
    const uint8_t host_share[DEVICE_SPAKE2_SHARE_BYTES],
    const uint8_t *associated_data,
    size_t associated_data_size,
    device_spake2_keys_t **keys)
{
    if (keys == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    *keys = NULL;
    if (session == NULL || host_share == NULL
        || (associated_data_size > 0U && associated_data == NULL)
        || associated_data_size > DEVICE_SPAKE2_ASSOCIATED_DATA_MAX_BYTES) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    if (session->finished) {
        return DEVICE_CRYPTO_RESULT_INVALID_STATE;
    }
    session->finished = true;

    mbedtls_ecp_group group;
    mbedtls_ecp_point host;
    mbedtls_ecp_point n;
    mbedtls_ecp_point password_n;
    mbedtls_ecp_point negative_password_n;
    mbedtls_ecp_point unmasked_host;
    mbedtls_ecp_point shared;
    mbedtls_mpi private_scalar_value;
    mbedtls_mpi password_scalar_value;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&host);
    mbedtls_ecp_point_init(&n);
    mbedtls_ecp_point_init(&password_n);
    mbedtls_ecp_point_init(&negative_password_n);
    mbedtls_ecp_point_init(&unmasked_host);
    mbedtls_ecp_point_init(&shared);
    mbedtls_mpi_init(&private_scalar_value);
    mbedtls_mpi_init(&password_scalar_value);

    int operation = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    device_crypto_result_t result = map_mbedtls_result(operation);
    if (result == DEVICE_CRYPTO_RESULT_OK
        && (host_share[0] != 0x04U
            || load_point(&group, &host, host_share, DEVICE_SPAKE2_SHARE_BYTES)
                != DEVICE_CRYPTO_RESULT_OK)) {
        result = DEVICE_CRYPTO_RESULT_INVALID_POINT;
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = load_point(&group, &n, SPAKE2_N_COMPRESSED, sizeof(SPAKE2_N_COMPRESSED));
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_mpi_read_binary(
            &private_scalar_value, session->private_scalar, P256_SCALAR_BYTES);
        if (operation == 0) {
            operation = mbedtls_mpi_read_binary(
                &password_scalar_value, session->password_scalar, P256_SCALAR_BYTES);
        }
        result = map_mbedtls_result(operation);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_ecp_mul(
            &group,
            &password_n,
            &password_scalar_value,
            &n,
            mbedtls_random_fill,
            &session->random_source);
        result = map_mbedtls_result(operation);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_ecp_copy(&negative_password_n, &password_n);
        if (operation == 0) {
            operation = mbedtls_mpi_sub_mpi(
                &negative_password_n.MBEDTLS_PRIVATE(Y),
                &group.P,
                &password_n.MBEDTLS_PRIVATE(Y));
        }
        if (operation == 0) {
            operation = mbedtls_mpi_mod_mpi(
                &negative_password_n.MBEDTLS_PRIVATE(Y),
                &negative_password_n.MBEDTLS_PRIVATE(Y),
                &group.P);
        }
        result = map_mbedtls_result(operation);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = add_points(&group, &unmasked_host, &host, &negative_password_n);
        if (result == DEVICE_CRYPTO_RESULT_OK && mbedtls_ecp_is_zero(&unmasked_host)) {
            result = DEVICE_CRYPTO_RESULT_INVALID_POINT;
        }
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        operation = mbedtls_ecp_mul(
            &group,
            &shared,
            &private_scalar_value,
            &unmasked_host,
            mbedtls_random_fill,
            &session->random_source);
        result = map_mbedtls_result(operation);
    }

    uint8_t shared_bytes[DEVICE_SPAKE2_SHARE_BYTES] = {0};
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = point_to_uncompressed(&group, &shared, shared_bytes);
    }
    spake2_transcript_builder_t transcript = {0};
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        spake2_transcript_append(
            &transcript,
            (const uint8_t *)session->pairing_request_id,
            strlen(session->pairing_request_id));
        spake2_transcript_append(
            &transcript,
            (const uint8_t *)session->installation_id,
            strlen(session->installation_id));
        spake2_transcript_append(
            &transcript, session->device_share, DEVICE_SPAKE2_SHARE_BYTES);
        spake2_transcript_append(&transcript, host_share, DEVICE_SPAKE2_SHARE_BYTES);
        spake2_transcript_append(&transcript, shared_bytes, sizeof(shared_bytes));
        spake2_transcript_append(
            &transcript, session->password_scalar, P256_SCALAR_BYTES);
        if (transcript.overflow) {
            result = DEVICE_CRYPTO_RESULT_OPERATION_FAILED;
        }
    }
    device_spake2_keys_t *derived = NULL;
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        derived = calloc(1U, sizeof(*derived));
        if (derived == NULL) {
            result = DEVICE_CRYPTO_RESULT_RESOURCE_EXHAUSTED;
        }
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        result = derive_keys(
            transcript.bytes,
            transcript.size,
            associated_data,
            associated_data_size,
            derived);
    }
    if (result == DEVICE_CRYPTO_RESULT_OK) {
        *keys = derived;
    } else {
        device_spake2_keys_release(derived);
    }

    mbedtls_platform_zeroize(&transcript, sizeof(transcript));
    mbedtls_platform_zeroize(shared_bytes, sizeof(shared_bytes));
    mbedtls_platform_zeroize(session->private_scalar, sizeof(session->private_scalar));
    mbedtls_platform_zeroize(session->password_scalar, sizeof(session->password_scalar));
    mbedtls_mpi_free(&password_scalar_value);
    mbedtls_mpi_free(&private_scalar_value);
    mbedtls_ecp_point_free(&shared);
    mbedtls_ecp_point_free(&unmasked_host);
    mbedtls_ecp_point_free(&negative_password_n);
    mbedtls_ecp_point_free(&password_n);
    mbedtls_ecp_point_free(&n);
    mbedtls_ecp_point_free(&host);
    mbedtls_ecp_group_free(&group);
    return result;
}

void device_spake2_party_a_release(device_spake2_party_a_t *session)
{
    if (session != NULL) {
        mbedtls_platform_zeroize(session, sizeof(*session));
        free(session);
    }
}

device_crypto_result_t device_spake2_keys_copy_device_confirmation(
    const device_spake2_keys_t *keys,
    uint8_t confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES])
{
    if (keys == NULL || confirmation == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    memcpy(confirmation, keys->device_confirmation, DEVICE_SPAKE2_CONFIRMATION_BYTES);
    return DEVICE_CRYPTO_RESULT_OK;
}

device_crypto_result_t device_spake2_keys_verify_host_confirmation(
    const device_spake2_keys_t *keys,
    const uint8_t confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES])
{
    if (keys == NULL || confirmation == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    return mbedtls_ct_memcmp(
               keys->expected_host_confirmation,
               confirmation,
               DEVICE_SPAKE2_CONFIRMATION_BYTES)
            == 0
        ? DEVICE_CRYPTO_RESULT_OK
        : DEVICE_CRYPTO_RESULT_CONFIRMATION_FAILED;
}

device_crypto_result_t device_spake2_keys_binding_mac(
    const device_spake2_keys_t *keys,
    const uint8_t *label,
    size_t label_size,
    const uint8_t *transcript,
    size_t transcript_size,
    uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES])
{
    if (keys == NULL || binding_mac == NULL || (label_size > 0U && label == NULL)
        || (transcript_size > 0U && transcript == NULL)) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    return hmac_sha256_parts(
        keys->session_key,
        sizeof(keys->session_key),
        label,
        label_size,
        transcript,
        transcript_size,
        NULL,
        0U,
        binding_mac);
}

device_crypto_result_t device_spake2_keys_verify_binding_mac(
    const device_spake2_keys_t *keys,
    const uint8_t *label,
    size_t label_size,
    const uint8_t *transcript,
    size_t transcript_size,
    const uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES])
{
    if (binding_mac == NULL) {
        return DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT;
    }
    uint8_t expected[DEVICE_SPAKE2_BINDING_MAC_BYTES];
    const device_crypto_result_t result = device_spake2_keys_binding_mac(
        keys,
        label,
        label_size,
        transcript,
        transcript_size,
        expected);
    if (result != DEVICE_CRYPTO_RESULT_OK) {
        mbedtls_platform_zeroize(expected, sizeof(expected));
        return result;
    }
    const int difference = mbedtls_ct_memcmp(expected, binding_mac, sizeof(expected));
    mbedtls_platform_zeroize(expected, sizeof(expected));
    return difference == 0 ? DEVICE_CRYPTO_RESULT_OK : DEVICE_CRYPTO_RESULT_BINDING_FAILED;
}

void device_spake2_keys_release(device_spake2_keys_t *keys)
{
    if (keys != NULL) {
        mbedtls_platform_zeroize(keys, sizeof(*keys));
        free(keys);
    }
}
