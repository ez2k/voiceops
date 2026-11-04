#include "settings.h"

#include <stdlib.h>
#include <string.h>

#include "nvs.h"

#define NVS_NAMESPACE   "cb_settings"
#define DEFAULT_MODEL   "gemini-3.8-live"
#define DEFAULT_RINGS   2

extern const char default_prompt[] asm("_binary_prompt_md_start");

static void get_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

void settings_load(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->model, DEFAULT_MODEL, sizeof(s->model));
    s->rings = DEFAULT_RINGS;
    s->prompt = (char *)default_prompt;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;  /* 아직 아무것도 저장되지 않았다 */
    }
    get_str(h, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid));
    get_str(h, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass));
    get_str(h, "api_key", s->api_key, sizeof(s->api_key));

    char model[sizeof(s->model)];
    get_str(h, "model", model, sizeof(model));
    if (model[0]) {
        strlcpy(s->model, model, sizeof(s->model));
    }
    int32_t rings;
    if (nvs_get_i32(h, "rings", &rings) == ESP_OK) {
        s->rings = (int)rings;
    }

    size_t len = 0;
    if (nvs_get_blob(h, "prompt", NULL, &len) == ESP_OK && len > 0 && len <= SETTINGS_PROMPT_MAX) {
        char *p = malloc(len + 1);
        if (p != NULL && nvs_get_blob(h, "prompt", p, &len) == ESP_OK) {
            p[len] = '\0';
            s->prompt = p;
            s->prompt_custom = true;
        } else {
            free(p);
        }
    }
    nvs_close(h);
}

static esp_err_t with_nvs(esp_err_t (*write)(nvs_handle_t, const void *), const void *arg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = write(h, arg);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

typedef struct {
    const char *key;
    const char *value;
} kv_t;

static esp_err_t write_str(nvs_handle_t h, const void *arg)
{
    const kv_t *kv = arg;
    return nvs_set_str(h, kv->key, kv->value);
}

static esp_err_t write_rings(nvs_handle_t h, const void *arg)
{
    return nvs_set_i32(h, "rings", *(const int32_t *)arg);
}

esp_err_t settings_set(const char *key, const char *value)
{
    static const struct {
        const char *key;
        size_t max;
    } STRINGS[] = {
        {"wifi_ssid", sizeof(((settings_t *)0)->wifi_ssid) - 1},
        {"wifi_pass", sizeof(((settings_t *)0)->wifi_pass) - 1},
        {"api_key", sizeof(((settings_t *)0)->api_key) - 1},
        {"model", sizeof(((settings_t *)0)->model) - 1},
    };

    if (strcmp(key, "rings") == 0) {
        int32_t rings = (int32_t)strtol(value, NULL, 10);
        if (rings < 1 || rings > 10) {
            return ESP_ERR_INVALID_ARG;
        }
        return with_nvs(write_rings, &rings);
    }
    for (size_t i = 0; i < sizeof(STRINGS) / sizeof(STRINGS[0]); i++) {
        if (strcmp(key, STRINGS[i].key) == 0) {
            if (strlen(value) > STRINGS[i].max) {
                return ESP_ERR_INVALID_SIZE;
            }
            const kv_t kv = {key, value};
            return with_nvs(write_str, &kv);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

typedef struct {
    const char *text;
    size_t len;
} blob_t;

static esp_err_t write_prompt(nvs_handle_t h, const void *arg)
{
    const blob_t *b = arg;
    if (b->len == 0) {
        esp_err_t err = nvs_erase_key(h, "prompt");
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    return nvs_set_blob(h, "prompt", b->text, b->len);
}

esp_err_t settings_set_prompt(const char *text, size_t len)
{
    if (len > SETTINGS_PROMPT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const blob_t b = {text, len};
    return with_nvs(write_prompt, &b);
}
