#include "verdandi/c/verdandi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    verdandi_error error;
    verdandi_client* client = NULL;
    const verdandi_bytes_view empty = {NULL, 0U};

    if (verdandi_c_abi_version() != VERDANDI_C_ABI_VERSION) {
        return 1;
    }
    memset(&error, 0x7f, sizeof(error));
    verdandi_error_reset(&error);
    if (error.code[0] != '\0' || error.field[0] != '\0' || error.detail[0] != '\0' || error.has_revision != 0U) {
        return 2;
    }
    if (verdandi_client_open_json(empty, &client, &error) != 0 || client != NULL || strcmp(error.code, "capacity") != 0) {
        return 3;
    }
    return 0;
}
