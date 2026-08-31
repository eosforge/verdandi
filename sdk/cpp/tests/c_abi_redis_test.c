#include "verdandi/c/verdandi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct field_expectation {
    const char* name;
    const char* value;
    int found;
} field_expectation;

typedef struct policy_context {
    verdandi_fields_view replacement;
    field_expectation expected;
} policy_context;

static verdandi_string_view text_view(const char* value) {
    verdandi_string_view output = {value, strlen(value)};
    return output;
}

static verdandi_bytes_view bytes_view(const char* value) {
    verdandi_bytes_view output = {(const uint8_t*)value, strlen(value)};
    return output;
}

static void make_zone(const char* prefix, char* output, size_t capacity) {
    uint64_t value = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)output;
    size_t length = strlen(prefix);
    size_t index;
    memcpy(output, prefix, length);
    for (index = 0; index < 10U && length + 1U < capacity; ++index) {
        output[length++] = (char)('A' + (value % 26U));
        value = value / 26U + 17U;
    }
    output[length] = '\0';
}

static int VERDANDI_C_CALL expect_field(void* context, verdandi_string_view name, verdandi_bytes_view value) {
    field_expectation* expected = (field_expectation*)context;
    const size_t name_size = strlen(expected->name);
    const size_t value_size = strlen(expected->value);
    if (name.size == name_size && value.size == value_size && memcmp(name.data, expected->name, name_size) == 0 &&
        memcmp(value.data, expected->value, value_size) == 0) {
        expected->found = 1;
    }
    return 1;
}

static int VERDANDI_C_CALL select_first(void* context, verdandi_candidates* candidates, verdandi_selection* selection, verdandi_error* error) {
    policy_context* policy = (policy_context*)context;
    verdandi_registration_metadata metadata;
    if (verdandi_candidates_size(candidates) == 0U || verdandi_candidates_metadata(candidates, 0U, &metadata) == 0 || metadata.uuid.size != 32U) {
        return 0;
    }
    if (verdandi_candidates_visit_data(candidates, 0U, expect_field, &policy->expected, error) == 0 || policy->expected.found == 0) {
        return 0;
    }
    if (verdandi_candidates_mutate(candidates, 0U, policy->replacement, error) == 0) {
        return 0;
    }
    return verdandi_selection_add(selection, 0U, error);
}

static int report_failure(const char* step, const verdandi_error* error) {
    fprintf(stderr, "%s failed: %s %s %s\n", step, error->code, error->field, error->detail);
    return 1;
}

static void erase_key(verdandi_client* client, const char* key) {
    int removed = 0;
    verdandi_error ignored;
    if (client != NULL) {
        (void)verdandi_key_erase(client, text_view(key), &removed, &ignored);
    }
}

int main(void) {
    const char* address = getenv("VERDANDI_REDIS_ADDRESS");
    char registration_zone[32];
    char catalog_zone[32];
    char json[1024];
    char key[256];
    char hash[256];
    char cleanup[512];
    int result = 0;
    int flag = 0;
    uint64_t revision = 0;
    verdandi_error error = {0};
    verdandi_client* root = NULL;
    verdandi_blob* blob = NULL;
    verdandi_field_set* loaded_fields = NULL;
    verdandi_registration_client* registration_client = NULL;
    verdandi_registration* registration = NULL;
    verdandi_selector* selector = NULL;
    verdandi_candidate_list* selected = NULL;
    verdandi_selector_snapshot* selector_snapshot = NULL;
    verdandi_catalog_client* catalog_client = NULL;
    verdandi_catalog_publisher* publisher = NULL;
    verdandi_catalog_subscriber* subscriber = NULL;
    verdandi_catalog_entry* entry = NULL;

    verdandi_field_view attr_field = {0};
    verdandi_field_view data_field = {0};
    verdandi_field_view predicted_field = {0};
    verdandi_field_view catalog_field = {0};
    verdandi_fields_view attr = {&attr_field, 1U};
    verdandi_fields_view data = {&data_field, 1U};
    verdandi_fields_view predicted = {&predicted_field, 1U};
    verdandi_fields_view catalog_value = {&catalog_field, 1U};
    verdandi_registration_options options = {0};
    verdandi_catalog_path_view path = {0};
    verdandi_string_view part = {0};
    verdandi_catalog_subscription subscription = {0};
    policy_context policy = {0};
    field_expectation expected = {0};

    if (address == NULL || address[0] == '\0') {
        return 77;
    }

    make_zone("CRegistration", registration_zone, sizeof(registration_zone));
    make_zone("CCatalog", catalog_zone, sizeof(catalog_zone));
    (void)snprintf(json, sizeof(json),
                   "{\"version\":\"v1\",\"redis\":{\"mode\":\"standalone\",\"addresses\":[\"%s\"]},"
                   "\"registration\":{\"zone\":\"%s\",\"selector\":{\"sync_timeout_ms\":5000}},"
                   "\"catalog\":{\"zone\":\"%s\",\"sync_timeout_ms\":5000}}",
                   address, registration_zone, catalog_zone);

    if (verdandi_client_open_json(bytes_view(json), &root, &error) == 0) {
        return report_failure("client open", &error);
    }
    if (verdandi_client_ping(root, &error) == 0) {
        result = report_failure("ping", &error);
        goto cleanup;
    }

    (void)snprintf(key, sizeof(key), "verdandi:cabi:%s:key", registration_zone);
    if (verdandi_key_store_ttl(root, text_view(key), bytes_view("payload"), 5000U, &error) == 0 ||
        verdandi_key_load(root, text_view(key), &flag, &blob, &error) == 0 || flag == 0 || verdandi_blob_view(blob).size != 7U) {
        result = report_failure("key round trip", &error);
        goto cleanup;
    }
    verdandi_blob_release(blob);
    blob = NULL;

    (void)snprintf(hash, sizeof(hash), "verdandi:cabi:%s:hash", registration_zone);
    data_field.name = text_view("power");
    data_field.value = bytes_view("1");
    if (verdandi_hash_store(root, text_view(hash), data, &error) == 0 || verdandi_hash_load(root, text_view(hash), &loaded_fields, &error) == 0 ||
        verdandi_field_set_size(loaded_fields) != 1U) {
        result = report_failure("hash round trip", &error);
        goto cleanup;
    }
    expected.name = "power";
    expected.value = "1";
    expected.found = 0;
    if (verdandi_field_set_visit(loaded_fields, expect_field, &expected, &error) == 0 || expected.found == 0) {
        result = report_failure("hash visit", &error);
        goto cleanup;
    }
    verdandi_field_set_release(loaded_fields);
    loaded_fields = NULL;

    if (verdandi_registration_client_open(root, &registration_client, &error) == 0) {
        result = report_failure("registration client", &error);
        goto cleanup;
    }
    options.type = text_view("Proxy");
    options.ttl_ms = 5000U;
    options.version = 1U;
    if (verdandi_registration_create(registration_client, &options, &registration, &error) == 0) {
        result = report_failure("registration create", &error);
        goto cleanup;
    }
    attr_field.name = text_view("region");
    attr_field.value = bytes_view("east");
    if (verdandi_registration_publish(registration, attr, data, &error) == 0 || verdandi_registration_is_published(registration) == 0 ||
        verdandi_registration_uuid(registration).size != 32U) {
        result = report_failure("registration publish", &error);
        goto cleanup;
    }
    if (verdandi_selector_create(registration_client, text_view("Proxy"), &selector, &error) == 0) {
        result = report_failure("selector create", &error);
        goto cleanup;
    }
    predicted_field.name = text_view("power");
    predicted_field.value = bytes_view("2");
    policy.replacement = predicted;
    policy.expected.name = "power";
    policy.expected.value = "1";
    policy.expected.found = 0;
    if (verdandi_selector_one(selector, select_first, &policy, &selected, &error) == 0 || verdandi_candidate_list_size(selected) != 1U) {
        result = report_failure("selector one", &error);
        goto cleanup;
    }
    expected.name = "power";
    expected.value = "2";
    expected.found = 0;
    if (verdandi_candidate_list_visit_data(selected, 0U, expect_field, &expected, &error) == 0 || expected.found == 0) {
        result = report_failure("selector prediction", &error);
        goto cleanup;
    }
    if (verdandi_selector_snapshot_create(selector, &selector_snapshot, &error) == 0 || verdandi_selector_snapshot_size(selector_snapshot, 0) != 1U) {
        result = report_failure("selector snapshot", &error);
        goto cleanup;
    }

    if (verdandi_catalog_client_open(root, &catalog_client, &error) == 0 || verdandi_catalog_publisher_create(catalog_client, &publisher, &error) == 0) {
        result = report_failure("catalog client", &error);
        goto cleanup;
    }
    path.part = text_view("routing");
    path.id = text_view("primary");
    catalog_field.name = text_view("power");
    catalog_field.value = bytes_view("11");
    if (verdandi_catalog_replace(publisher, path, text_view("map"), catalog_value, &revision, &error) == 0 || revision == 0U) {
        result = report_failure("catalog replace", &error);
        goto cleanup;
    }
    part = text_view("routing");
    subscription.parts = &part;
    subscription.part_count = 1U;
    if (verdandi_catalog_subscriber_create(catalog_client, &subscription, &subscriber, &error) == 0 ||
        verdandi_catalog_subscriber_find(subscriber, path, &flag, &entry, &error) == 0 || flag == 0) {
        result = report_failure("catalog subscriber", &error);
        goto cleanup;
    }
    expected.name = "power";
    expected.value = "11";
    expected.found = 0;
    {
        const char* status = NULL;
        int synchronized = 0;
        int present = 0;
        if (verdandi_catalog_entry_load(entry, &revision, &status, &synchronized, &present, &loaded_fields, &error) == 0 || synchronized == 0 || present == 0 ||
            strcmp(status, "present") != 0 || verdandi_field_set_visit(loaded_fields, expect_field, &expected, &error) == 0 || expected.found == 0) {
            result = report_failure("catalog entry", &error);
            goto cleanup;
        }
    }
    if (verdandi_catalog_erase(publisher, path, &revision, &error) == 0) {
        result = report_failure("catalog erase", &error);
        goto cleanup;
    }

cleanup:
    verdandi_field_set_release(loaded_fields);
    verdandi_selector_snapshot_release(selector_snapshot);
    verdandi_candidate_list_release(selected);
    verdandi_catalog_entry_release(entry);
    verdandi_catalog_subscriber_release(subscriber);
    verdandi_catalog_publisher_release(publisher);
    verdandi_catalog_client_release(catalog_client);
    verdandi_selector_release(selector);
    verdandi_registration_release(registration);
    verdandi_registration_client_release(registration_client);
    verdandi_blob_release(blob);

    erase_key(root, key);
    erase_key(root, hash);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:config:%s", registration_zone);
    erase_key(root, cleanup);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:registry:%s:Proxy", registration_zone);
    erase_key(root, cleanup);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:catalog:%s:@meta", catalog_zone);
    erase_key(root, cleanup);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:catalog:%s:@live", catalog_zone);
    erase_key(root, cleanup);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:catalog:%s:@deleted", catalog_zone);
    erase_key(root, cleanup);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:catalog:%s:@deleted_time", catalog_zone);
    erase_key(root, cleanup);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:catalog:%s:routing:primary", catalog_zone);
    erase_key(root, cleanup);
    (void)snprintf(cleanup, sizeof(cleanup), "verdandi:catalog:%s:routing:primary:@field_revisions", catalog_zone);
    erase_key(root, cleanup);
    verdandi_client_release(root);
    return result;
}
