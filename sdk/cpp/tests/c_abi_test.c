#include "verdandi/c/verdandi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    verdandi_error error;
    verdandi_client* client = NULL;
    const verdandi_bytes_view empty = {NULL, 0U};
    static const char valid_json[] = "{\"version\":\"v1\",\"redis\":{\"mode\":\"standalone\",\"addresses\":[\"127.0.0.1:6379\"]}}";
    const verdandi_bytes_view valid = {(const uint8_t*)valid_json, sizeof(valid_json) - 1U};

    if (verdandi_c_abi_version() != VERDANDI_C_ABI_VERSION) {
        return 1;
    }
    {
        static const char known[] = "redis.sentinel_tls";
        static const char unknown[] = "future.capability";
        const verdandi_string_view supported = {known, sizeof(known) - 1U};
        const verdandi_string_view unsupported = {unknown, sizeof(unknown) - 1U};
        const verdandi_string_view invalid = {NULL, 1U};
        if (verdandi_c_has_capability(supported) == 0 || verdandi_c_has_capability(unsupported) != 0 || verdandi_c_has_capability(invalid) != 0 ||
            verdandi_c_has_capability((verdandi_string_view){NULL, 0U}) != 0) {
            return 2;
        }
    }
    memset(&error, 0x7f, sizeof(error));
    verdandi_error_reset(&error);
    if (error.code[0] != '\0' || error.field[0] != '\0' || error.detail[0] != '\0' || error.has_revision != 0U) {
        return 3;
    }
    if (verdandi_client_open_json(empty, &client, &error) != 0 || client != NULL || strcmp(error.code, "invalid") != 0) {
        return 4;
    }
    if (verdandi_configuration_validate_json(valid, &error) == 0 || error.code[0] != '\0') {
        return 5;
    }
    if (verdandi_configuration_validate_json(empty, &error) != 0 || strcmp(error.code, "invalid") != 0 || strcmp(error.field, "json") != 0) {
        return 6;
    }
    return 0;
}
