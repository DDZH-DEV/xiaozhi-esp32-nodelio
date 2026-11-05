#pragma once

#include <cJSON.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// The selected app remains the routing authority for all workbench interactions.
static inline const char* nl_workbench_key(const char* id) {
    if (!id)
        return "";
    const char* sep = strstr(id, "::");
    return sep ? sep + 2 : id;
}

static inline bool nl_workbench_app(const char* id) {
    const char* key = nl_workbench_key(id);
    const char* dot = strchr(key, '.');
    if (!dot)
        return false;
    const size_t n = (size_t)(dot - key);
    const bool known =
        (n == 3 && strncmp(key, "dsh", n) == 0) || (n == 6 && strncmp(key, "openai", n) == 0) ||
        (n == 9 && (strncmp(key, "workbuddy", n) == 0 || strncmp(key, "workbench", n) == 0));
    return known && (strcmp(dot, ".status") == 0 || strcmp(dot, ".task") == 0 ||
                     strcmp(dot, ".answer") == 0);
}

static inline bool nl_workbench_helper(const char* id) {
    return nl_workbench_app(id) && strcmp(strchr(nl_workbench_key(id), '.'), ".status") != 0;
}

static inline const char* nl_workbench_named_app(const char* name) {
    if (!name)
        return NULL;
    char lower[256];
    snprintf(lower, sizeof(lower), "%s", name);
    for (char* p = lower; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    }
    if (strstr(lower, "workbuddy"))
        return "workbuddy.status";
    if (strstr(lower, "chatgpt") || strstr(lower, "openai"))
        return "openai.status";
    if (strstr(lower, "deepseek") || strstr(lower, "dsh"))
        return "dsh.status";
    if (strstr(lower, "workbench") || strstr(lower, "默认工作台"))
        return "workbench.status";
    return NULL;
}

static inline bool nl_workbench_mutation(const char* id, const char* body) {
    if (!nl_workbench_app(id))
        return false;
    if (nl_workbench_helper(id))
        return true;
    cJSON* root = cJSON_Parse(body ? body : "{}");
    const cJSON* op = cJSON_GetObjectItemCaseSensitive(root, "operation");
    const bool mutation = cJSON_IsString(op) && (strcmp(op->valuestring, "task") == 0 ||
                                                 strcmp(op->valuestring, "answer") == 0);
    cJSON_Delete(root);
    return mutation;
}

static inline void nl_workbench_status_id(const char* id, char* dst, size_t size) {
    if (!nl_workbench_app(id)) {
        snprintf(dst, size, "dsh.status");
        return;
    }
    const char* dot = strchr(nl_workbench_key(id), '.');
    snprintf(dst, size, "%.*s.status", (int)(dot - id), id);
}
