/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * extract.c -- Entity extraction via external LLM (OpenAI-compatible API)
 *
 * When ENABLE_CURL is defined (libcurl available), POSTs a structured
 * extraction prompt to a localhost endpoint (e.g., llama-server) and
 * parses the response into entities and relations.
 *
 * Without libcurl, extraction is unavailable but the daemon runs normally.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_CURL
# include <curl/curl.h>
#endif

#include <cJSON.h>

#include "extract.h"
#include "id.h"
#include "memory.h"
#include "log.h"

struct mnemon_extract {
    char *endpoint;
    char *model;
    int   timeout_ms;
    bool  available;
};

#ifdef ENABLE_CURL
/* curl write callback: accumulate response body */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
                            void *userp)
{
    size_t total = size * nmemb;
    curl_buf_t *buf = (curl_buf_t *)userp;

    if (buf->len + total + 1 > buf->cap) {
        size_t newcap = buf->cap ? buf->cap * 2 : 4096;
        while (newcap < buf->len + total + 1) newcap *= 2;
        if (newcap > 10 * 1024 * 1024) return 0; /* 10MB response cap */
        char *p = realloc(buf->data, newcap);
        if (!p) return 0;
        buf->data = p;
        buf->cap = newcap;
    }

    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}
#endif /* ENABLE_CURL */

/* The extraction prompt template */
static const char *EXTRACTION_PROMPT =
    "Extract entities and relations from the following text. "
    "Respond with JSON only, no prose. Format:\n"
    "{\"entities\": [{\"name\": \"...\", \"type\": \"...\", "
    "\"observations\": [\"...\"]}], "
    "\"relations\": [{\"source\": \"...\", \"target\": \"...\", "
    "\"type\": \"...\", \"description\": \"...\"}]}\n\n"
    "Text: ";

mnemon_err_t mnemon_extract_init(mnemon_extract_t **out,
                                 const char *endpoint,
                                 const char *model,
                                 int timeout_ms)
{
    mnemon_extract_t *e;

    if (!out)
        return MNEMON_ERR_INVALID_INPUT;

    *out = NULL;

    if (!endpoint || endpoint[0] == '\0') {
        mnemon_log(MNEMON_LOG_INFO,
                   "entity extraction not configured -- disabled");
        return MNEMON_OK;
    }

    e = calloc(1, sizeof(*e));
    if (!e)
        return MNEMON_ERR_OOM;

    e->endpoint = strdup(endpoint);
    e->model = model && model[0] ? strdup(model) : NULL;
    e->timeout_ms = timeout_ms > 0 ? timeout_ms : 10000;

#ifdef ENABLE_CURL
    e->available = true;
    mnemon_log(MNEMON_LOG_INFO,
               "entity extraction configured: endpoint=%s model=%s",
               e->endpoint, e->model ? e->model : "(default)");
#else
    e->available = false;
    mnemon_log(MNEMON_LOG_WARNING,
               "entity extraction configured but libcurl not available "
               "(build with -DENABLE_CURL=ON)");
#endif

    *out = e;
    return MNEMON_OK;
}

void mnemon_extract_free(mnemon_extract_t *e)
{
    if (!e)
        return;
    free(e->endpoint);
    free(e->model);
    free(e);
}

mnemon_err_t mnemon_extract_from_text(mnemon_extract_t *e,
                                      const char *text,
                                      mnemon_extraction_result_t *result)
{
    if (!result)
        return MNEMON_ERR_INVALID_INPUT;

    memset(result, 0, sizeof(*result));

    if (!e || !e->available) {
        mnemon_err_set(MNEMON_ERR_EXTRACTION, 0,
                       "entity extraction not available");
        return MNEMON_ERR_EXTRACTION;
    }

    if (!text || text[0] == '\0')
        return MNEMON_OK;

#ifdef ENABLE_CURL
    /* Build the chat completion request */
    cJSON *req = cJSON_CreateObject();
    if (e->model)
        cJSON_AddStringToObject(req, "model", e->model);

    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    /* Concatenate prompt + text */
    size_t prompt_len = strlen(EXTRACTION_PROMPT) + strlen(text) + 1;
    char *full_prompt = malloc(prompt_len);
    if (!full_prompt) {
        cJSON_Delete(req);
        return MNEMON_ERR_OOM;
    }
    snprintf(full_prompt, prompt_len, "%s%s", EXTRACTION_PROMPT, text);
    cJSON_AddStringToObject(msg, "content", full_prompt);
    free(full_prompt);

    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(req, "messages", messages);
    cJSON_AddNumberToObject(req, "max_tokens", 2048);
    cJSON_AddNumberToObject(req, "temperature", 0.0);

    char *req_body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_body)
        return MNEMON_ERR_OOM;

    /* HTTP POST via libcurl */
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(req_body);
        return MNEMON_ERR_EXTRACTION;
    }

    curl_buf_t resp_buf = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, e->endpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)e->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(req_body);

    if (res != CURLE_OK) {
        mnemon_err_set(MNEMON_ERR_EXTRACTION, (int)res,
                       "curl failed: %s", curl_easy_strerror(res));
        free(resp_buf.data);
        return MNEMON_ERR_EXTRACTION;
    }

    /* Parse the LLM response */
    cJSON *resp = cJSON_Parse(resp_buf.data);
    free(resp_buf.data);
    if (!resp) {
        mnemon_err_set(MNEMON_ERR_EXTRACTION, 0, "invalid JSON response");
        return MNEMON_ERR_EXTRACTION;
    }

    /* Extract the assistant message content */
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(resp, "choices");
    const char *content = NULL;
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
        cJSON *cont = cJSON_GetObjectItemCaseSensitive(message, "content");
        if (cJSON_IsString(cont))
            content = cont->valuestring;
    }

    if (!content) {
        cJSON_Delete(resp);
        mnemon_err_set(MNEMON_ERR_EXTRACTION, 0, "no content in response");
        return MNEMON_ERR_EXTRACTION;
    }

    /* Parse the extraction JSON from the content */
    cJSON *extraction = cJSON_Parse(content);
    if (!extraction) {
        cJSON_Delete(resp);
        mnemon_err_set(MNEMON_ERR_EXTRACTION, 0,
                       "extraction response not valid JSON");
        return MNEMON_ERR_EXTRACTION;
    }

    /* Convert entities */
    cJSON *entities = cJSON_GetObjectItemCaseSensitive(extraction, "entities");
    if (cJSON_IsArray(entities)) {
        int count = cJSON_GetArraySize(entities);
        result->entities = calloc((size_t)count, sizeof(mnemon_entity_t));
        if (result->entities) {
            for (int i = 0; i < count; i++) {
                cJSON *ent = cJSON_GetArrayItem(entities, i);
                mnemon_entity_t *e_out = &result->entities[i];

                mnemon_uuid_t u;
                mnemon_uuid_generate(&u);
                memcpy(e_out->id, u.bytes, 16);

                cJSON *name = cJSON_GetObjectItemCaseSensitive(ent, "name");
                e_out->name = cJSON_IsString(name) ? strdup(name->valuestring) : strdup("");

                cJSON *etype = cJSON_GetObjectItemCaseSensitive(ent, "type");
                e_out->entity_type = cJSON_IsString(etype) ? strdup(etype->valuestring) : strdup("unknown");

                cJSON *obs = cJSON_GetObjectItemCaseSensitive(ent, "observations");
                if (cJSON_IsArray(obs)) {
                    int ocount = cJSON_GetArraySize(obs);
                    e_out->observations = calloc((size_t)ocount, sizeof(char *));
                    e_out->observation_count = (uint32_t)ocount;
                    for (int j = 0; j < ocount; j++) {
                        cJSON *o = cJSON_GetArrayItem(obs, j);
                        e_out->observations[j] = cJSON_IsString(o) ?
                            strdup(o->valuestring) : strdup("");
                    }
                }

                e_out->importance = 0.5f;
                e_out->created_at = mnemon_time_ms();
                e_out->updated_at = e_out->created_at;
                result->entity_count++;
            }
        }
    }

    /* Convert relations to edges */
    cJSON *relations = cJSON_GetObjectItemCaseSensitive(extraction, "relations");
    if (cJSON_IsArray(relations)) {
        int count = cJSON_GetArraySize(relations);
        result->edges = calloc((size_t)count, sizeof(mnemon_edge_t));
        if (result->edges) {
            for (int i = 0; i < count; i++) {
                cJSON *rel = cJSON_GetArrayItem(relations, i);
                mnemon_edge_t *edge = &result->edges[i];

                mnemon_uuid_t u;
                mnemon_uuid_generate(&u);
                memcpy(edge->id, u.bytes, 16);

                cJSON *etype = cJSON_GetObjectItemCaseSensitive(rel, "type");
                edge->edge_type = cJSON_IsString(etype) ? strdup(etype->valuestring) : strdup("related_to");

                cJSON *desc = cJSON_GetObjectItemCaseSensitive(rel, "description");
                edge->description = cJSON_IsString(desc) ? strdup(desc->valuestring) : NULL;

                edge->weight = 1.0f;
                edge->valid_from = mnemon_time_ms();
                edge->created_at = edge->valid_from;

                /* Note: source_id and target_id need to be resolved from
                 * entity names to UUIDs by the caller (store_memory) after
                 * entities are created. */
                result->edge_count++;
            }
        }
    }

    cJSON_Delete(extraction);
    cJSON_Delete(resp);

    mnemon_log(MNEMON_LOG_INFO, "extraction: %d entities, %d relations",
               result->entity_count, result->edge_count);
    return MNEMON_OK;

#else /* !ENABLE_CURL */
    (void)text;
    mnemon_err_set(MNEMON_ERR_EXTRACTION, 0,
                   "libcurl not available (build with -DENABLE_CURL=ON)");
    return MNEMON_ERR_EXTRACTION;
#endif
}

void mnemon_extraction_result_free(mnemon_extraction_result_t *r)
{
    if (!r)
        return;
    for (int i = 0; i < r->entity_count; i++)
        mnemon_entity_free(&r->entities[i]);
    free(r->entities);
    for (int i = 0; i < r->edge_count; i++)
        mnemon_edge_free(&r->edges[i]);
    free(r->edges);
    memset(r, 0, sizeof(*r));
}

bool mnemon_extract_available(const mnemon_extract_t *e)
{
    return e && e->available;
}
