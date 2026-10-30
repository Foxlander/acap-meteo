/*
 * meteo.c - ACAP qui estime la météo à partir de l'image de la caméra.
 *
 * Toutes les IntervalSeconds secondes :
 *   - ouvre un flux YUV (NV12) basse résolution via VDO,
 *   - capture une rafale de FrameCount images espacées de FrameSpacingMs,
 *   - fait la moyenne des images et calcule des indicateurs (voir weather.c),
 *   - classe la météo, lisse le résultat sur les HistorySize dernières rafales,
 *   - publie le résultat dans le syslog, en JSON sur
 *     http://<camera>/local/meteo/status (reverse proxy vers localhost:2001)
 *     et sous forme d'événement Axis (utilisable dans les règles d'action).
 */
#include "weather.h"

#include <axsdk/axevent.h>
#include <axsdk/axparameter.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <vdo-buffer.h>
#include <vdo-error.h>
#include <vdo-frame.h>
#include <vdo-map.h>
#include <vdo-stream.h>
#include <vdo-types.h>

#define APP_NAME "meteo"
#define HTTP_PORT 2001
#define STREAM_WIDTH 640
#define STREAM_HEIGHT 360
#define WARMUP_FRAMES 2
#define MAX_HISTORY 24
#define JSON_SIZE 2048

typedef struct {
    int interval_s;
    int frame_count;
    int frame_spacing_ms;
    int history_size;
    int channel;
    WeatherThresholds thresholds;
} Config;

static GMainLoop *main_loop;
static AXParameter *params;

/* Événement Axis "Météo" : n'est manipulé que depuis la boucle principale. */
static AXEventHandler *event_handler;
static guint event_declaration;
static gboolean event_ready;

/* Dernier résultat, lu par le serveur HTTP depuis d'autres threads. */
static GMutex result_lock;
static char result_json[JSON_SIZE] = "{\"condition\":\"unknown\",\"label\":\"En attente de la première mesure\"}";

/* Arrêt propre du thread de capture. */
static GMutex stop_lock;
static GCond stop_cond;
static gboolean stopping;

static int param_int(const char *name, int fallback) {
    gchar *value = NULL;
    GError *error = NULL;
    int result = fallback;
    if (ax_parameter_get(params, name, &value, &error)) {
        char *end = NULL;
        long v = strtol(value, &end, 10);
        if (end != value) result = (int)v;
    } else {
        syslog(LOG_WARNING, "Paramètre %s illisible : %s", name, error->message);
        g_error_free(error);
    }
    g_free(value);
    return result;
}

static double param_double(const char *name, double fallback) {
    gchar *value = NULL;
    GError *error = NULL;
    double result = fallback;
    if (ax_parameter_get(params, name, &value, &error)) {
        /* g_ascii_strtod : indépendant de la locale (point décimal). */
        char *end = NULL;
        double v = g_ascii_strtod(value, &end);
        if (end != value) result = v;
    } else {
        syslog(LOG_WARNING, "Paramètre %s illisible : %s", name, error->message);
        g_error_free(error);
    }
    g_free(value);
    return result;
}

static void read_config(Config *c) {
    WeatherThresholds d;
    weather_thresholds_default(&d);

    c->interval_s = CLAMP(param_int("IntervalSeconds", 300), 10, 86400);
    c->frame_count = CLAMP(param_int("FrameCount", 8), 2, 100);
    c->frame_spacing_ms = CLAMP(param_int("FrameSpacingMs", 500), 40, 10000);
    c->history_size = CLAMP(param_int("HistorySize", 3), 1, MAX_HISTORY);
    c->channel = CLAMP(param_int("Channel", 1), 1, 16);

    WeatherThresholds *t = &c->thresholds;
    t->night_luma = param_double("NightLuma", d.night_luma);
    t->ir_saturation = param_double("IrSaturation", d.ir_saturation);
    t->fog_edge_density = param_double("FogEdgeDensity", d.fog_edge_density);
    t->fog_contrast = param_double("FogContrast", d.fog_contrast);
    t->precip_specks = param_double("PrecipSpecks", d.precip_specks);
    t->snow_luma = param_double("SnowLuma", d.snow_luma);
    t->sunny_sky_luma = param_double("SunnySkyLuma", d.sunny_sky_luma);
    t->sunny_saturation = param_double("SunnySaturation", d.sunny_saturation);
    t->sky_ratio = CLAMP(param_double("SkyRatio", d.sky_ratio), 0.05, 1.0);
}

/*
 * Construit le contenu de l'événement : la condition en texte, plus un
 * booléen par condition pour pouvoir écrire une règle du type
 * "tant qu'il pleut, faire ...".
 */
static AXEventKeyValueSet *weather_event_values(WeatherCondition condition) {
    AXEventKeyValueSet *kvs = ax_event_key_value_set_new();
    const gchar *code = weather_condition_code(condition);

    ax_event_key_value_set_add_key_values(kvs, NULL,
                                          "topic0", "tnsaxis", "CameraApplicationPlatform", AX_VALUE_TYPE_STRING,
                                          "topic1", "tnsaxis", "Meteo", AX_VALUE_TYPE_STRING,
                                          "topic2", "tnsaxis", "Weather", AX_VALUE_TYPE_STRING,
                                          "condition", NULL, code, AX_VALUE_TYPE_STRING,
                                          NULL);
    ax_event_key_value_set_add_nice_names(kvs, "topic1", "tnsaxis", NULL, "Météo Caméra", NULL);
    ax_event_key_value_set_add_nice_names(kvs, "topic2", "tnsaxis", NULL, "Météo", NULL);
    ax_event_key_value_set_mark_as_data(kvs, "condition", NULL, NULL);

    for (int c = 0; c < WEATHER_COUNT; c++) {
        gboolean active = (int)condition == c;
        const gchar *key = weather_condition_code((WeatherCondition)c);
        ax_event_key_value_set_add_key_value(kvs, key, NULL, &active, AX_VALUE_TYPE_BOOL, NULL);
        ax_event_key_value_set_mark_as_data(kvs, key, NULL, NULL);
        ax_event_key_value_set_add_nice_names(kvs, key, NULL,
                                              weather_condition_label((WeatherCondition)c), NULL, NULL);
    }
    return kvs;
}

static void on_event_declared(guint declaration, gpointer data) {
    (void)data;
    syslog(LOG_INFO, "Événement Météo déclaré (%u)", declaration);
    event_ready = TRUE;
}

static void declare_weather_event(void) {
    GError *error = NULL;
    event_handler = ax_event_handler_new();
    AXEventKeyValueSet *kvs = weather_event_values(WEATHER_COUNT);
    /* stateless = FALSE : événement à état, la caméra connaît la météo courante. */
    if (!ax_event_handler_declare(event_handler, kvs, FALSE, &event_declaration, on_event_declared,
                                  NULL, &error)) {
        syslog(LOG_ERR, "Impossible de déclarer l'événement : %s", error->message);
        g_error_free(error);
    }
    ax_event_key_value_set_free(kvs);
}

/* Appelé dans la boucle principale (via g_idle_add) quand la météo stable change. */
static gboolean send_weather_event(gpointer data) {
    WeatherCondition condition = (WeatherCondition)GPOINTER_TO_INT(data);
    GError *error = NULL;
    if (!event_ready) return G_SOURCE_REMOVE;

    AXEventKeyValueSet *kvs = weather_event_values(condition);
    AXEvent *event = ax_event_new2(kvs, NULL);
    if (!ax_event_handler_send_event(event_handler, event_declaration, event, &error)) {
        syslog(LOG_ERR, "Impossible d'envoyer l'événement : %s", error->message);
        g_error_free(error);
    }
    ax_event_free(event);
    ax_event_key_value_set_free(kvs);
    return G_SOURCE_REMOVE;
}

/* Capture une rafale d'images et l'ajoute à l'accumulateur. Retourne FALSE en cas d'erreur. */
static gboolean capture_burst(const Config *c, WeatherAccumulator *acc) {
    GError *error = NULL;
    VdoStream *stream = NULL;
    VdoMap *info = NULL;
    gboolean ok = FALSE;

    VdoMap *settings = vdo_map_new();
    vdo_map_set_uint32(settings, "channel", c->channel);
    vdo_map_set_uint32(settings, "format", VDO_FORMAT_YUV);
    vdo_map_set_string(settings, "subformat", "NV12");
    vdo_map_set_uint32(settings, "width", STREAM_WIDTH);
    vdo_map_set_uint32(settings, "height", STREAM_HEIGHT);
    vdo_map_set_double(settings, "framerate", 1000.0 / c->frame_spacing_ms);

    stream = vdo_stream_new(settings, NULL, &error);
    g_object_unref(settings);
    if (!stream) goto out;

    info = vdo_stream_get_info(stream, &error);
    if (!info) goto out;
    int width = vdo_map_get_uint32(info, "width", STREAM_WIDTH);
    int height = vdo_map_get_uint32(info, "height", STREAM_HEIGHT);
    int pitch = vdo_map_get_uint32(info, "pitch", width);

    if (!vdo_stream_start(stream, &error)) goto out;

    for (int i = 0; i < WARMUP_FRAMES + c->frame_count; i++) {
        VdoBuffer *buffer = vdo_stream_get_buffer(stream, &error);
        if (!buffer) goto out;
        if (i >= WARMUP_FRAMES) {
            const uint8_t *data = vdo_buffer_get_data(buffer);
            /* NV12 : plan Y (pitch * height) suivi du plan UV entrelacé. */
            weather_acc_add_nv12(acc, data, data + (size_t)pitch * height, width, height, pitch);
        }
        if (!vdo_stream_buffer_unref(stream, &buffer, &error)) goto out;
    }
    ok = TRUE;

out:
    if (error) {
        syslog(LOG_ERR, "Erreur de capture VDO : %s", error->message);
        g_error_free(error);
    }
    if (info) g_object_unref(info);
    if (stream) g_object_unref(stream);
    return ok;
}

static gpointer capture_thread(gpointer data) {
    (void)data;
    WeatherCondition history[MAX_HISTORY];
    int history_count = 0;
    WeatherCondition last_stable = WEATHER_COUNT;
    WeatherAccumulator *acc = g_new0(WeatherAccumulator, 1);
    Config cfg;

    for (;;) {
        read_config(&cfg);
        weather_acc_reset(acc);

        WeatherFeatures f;
        if (capture_burst(&cfg, acc) && weather_compute_features(acc, cfg.thresholds.sky_ratio, &f) == 0) {
            WeatherResult r;
            weather_classify(&f, &cfg.thresholds, &r);

            /* Historique glissant pour un résultat stable dans le temps. */
            if (history_count >= cfg.history_size) {
                int drop = history_count - cfg.history_size + 1;
                memmove(history, history + drop, (history_count - drop) * sizeof(history[0]));
                history_count -= drop;
            }
            history[history_count++] = r.condition;
            WeatherCondition stable = weather_majority(history, history_count);

            char json[JSON_SIZE];
            weather_to_json(json, sizeof(json), &r, stable, &f, (long)time(NULL));
            g_mutex_lock(&result_lock);
            g_strlcpy(result_json, json, sizeof(result_json));
            g_mutex_unlock(&result_lock);

            syslog(LOG_INFO, "Mesure : %s (%.0f%%), stable : %s, lum=%.0f contraste=%.0f sat=%.1f "
                   "contours=%.3f scintillement=%.4f",
                   weather_condition_label(r.condition), r.confidence * 100.0,
                   weather_condition_label(stable), f.luma_mean, f.luma_std, f.saturation,
                   f.edge_density, f.temporal_specks);
            if (stable != last_stable) {
                syslog(LOG_NOTICE, "Météo : %s", weather_condition_label(stable));
                last_stable = stable;
                g_idle_add(send_weather_event, GINT_TO_POINTER(stable));
            }
        }

        /* Attente jusqu'à la prochaine rafale (interruptible à l'arrêt). */
        gint64 deadline = g_get_monotonic_time() + (gint64)cfg.interval_s * G_TIME_SPAN_SECOND;
        g_mutex_lock(&stop_lock);
        while (!stopping && g_cond_wait_until(&stop_cond, &stop_lock, deadline))
            ;
        gboolean stop = stopping;
        g_mutex_unlock(&stop_lock);
        if (stop) break;
    }
    g_free(acc);
    return NULL;
}

/* Mini serveur HTTP : toute requête reçoit le dernier résultat en JSON. */
static gboolean on_http_request(GThreadedSocketService *service, GSocketConnection *connection,
                                GObject *source, gpointer data) {
    (void)service;
    (void)source;
    (void)data;
    char request[4096];
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    if (g_input_stream_read(in, request, sizeof(request), NULL, NULL) <= 0) return TRUE;

    char json[JSON_SIZE];
    g_mutex_lock(&result_lock);
    g_strlcpy(json, result_json, sizeof(json));
    g_mutex_unlock(&result_lock);

    gchar *response = g_strdup_printf("HTTP/1.1 200 OK\r\n"
                                      "Content-Type: application/json; charset=utf-8\r\n"
                                      "Cache-Control: no-store\r\n"
                                      "Content-Length: %zu\r\n"
                                      "Connection: close\r\n\r\n%s",
                                      strlen(json), json);
    g_output_stream_write_all(out, response, strlen(response), NULL, NULL, NULL);
    g_free(response);
    return TRUE;
}

static GSocketService *start_http_server(void) {
    GError *error = NULL;
    GSocketService *service = g_threaded_socket_service_new(4);
    GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new(loopback, HTTP_PORT);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_TCP, NULL, NULL, &error)) {
        syslog(LOG_ERR, "Impossible d'écouter sur le port %d : %s", HTTP_PORT, error->message);
        g_error_free(error);
    }
    g_object_unref(address);
    g_object_unref(loopback);
    g_signal_connect(service, "run", G_CALLBACK(on_http_request), NULL);
    g_socket_service_start(service);
    return service;
}

static gboolean on_signal(gpointer data) {
    (void)data;
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    GError *error = NULL;

    openlog(APP_NAME, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "Démarrage de l'ACAP météo");

    params = ax_parameter_new(APP_NAME, &error);
    if (!params) {
        syslog(LOG_ERR, "ax_parameter_new : %s", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }

    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT, on_signal, NULL);

    declare_weather_event();
    GSocketService *http = start_http_server();
    GThread *capture = g_thread_new("capture", capture_thread, NULL);

    g_main_loop_run(main_loop);

    g_mutex_lock(&stop_lock);
    stopping = TRUE;
    g_cond_signal(&stop_cond);
    g_mutex_unlock(&stop_lock);
    g_thread_join(capture);

    g_socket_service_stop(http);
    g_object_unref(http);
    if (event_handler) {
        ax_event_handler_undeclare(event_handler, event_declaration, NULL);
        ax_event_handler_free(event_handler);
    }
    ax_parameter_free(params);
    g_main_loop_unref(main_loop);
    syslog(LOG_INFO, "Arrêt de l'ACAP météo");
    closelog();
    return EXIT_SUCCESS;
}
