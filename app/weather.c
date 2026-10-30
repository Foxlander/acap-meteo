/*
 * weather.c - Estimation de la météo à partir d'images NV12.
 *
 * Principe :
 *   1. On réduit chaque image d'une rafale à une grille de 160x90.
 *   2. On fait la moyenne des images : les éléments fugaces (gouttes, flocons,
 *      passants) disparaissent, la scène "de fond" reste.
 *   3. On mesure aussi ce qui change d'une image à l'autre : des petits points
 *      isolés qui scintillent trahissent la pluie ou la neige.
 *   4. Des indicateurs simples (luminosité, contraste, saturation, densité de
 *      contours, couleur du ciel) sont comparés à des seuils réglables.
 */
#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define GRID_SIZE (WEATHER_GRID_W * WEATHER_GRID_H)

/* Différence de luminance (sur la grille) à partir de laquelle un pixel "scintille". */
#define SPECK_DIFF 6.0f
/* Magnitude Sobel (|gx|+|gy|) à partir de laquelle on compte un contour. */
#define EDGE_THRESHOLD 48.0

void weather_thresholds_default(WeatherThresholds *t) {
    t->night_luma       = 35.0;
    t->ir_saturation    = 0.5;
    t->fog_edge_density = 0.04;
    t->fog_contrast     = 28.0;
    t->precip_specks    = 0.004;
    t->snow_luma        = 150.0;
    t->sunny_sky_luma   = 170.0;
    t->sunny_saturation = 14.0;
    t->sky_ratio        = 0.3;
}

void weather_acc_reset(WeatherAccumulator *acc) {
    memset(acc, 0, sizeof(*acc));
}

void weather_acc_add_nv12(WeatherAccumulator *acc, const uint8_t *y_plane, const uint8_t *uv_plane,
                          int width, int height, int stride) {
    float cur_y[GRID_SIZE];

    /* Réduction par moyenne de blocs. */
    for (int gy = 0; gy < WEATHER_GRID_H; gy++) {
        int y0 = gy * height / WEATHER_GRID_H;
        int y1 = (gy + 1) * height / WEATHER_GRID_H;
        if (y1 <= y0) y1 = y0 + 1;
        for (int gx = 0; gx < WEATHER_GRID_W; gx++) {
            int x0 = gx * width / WEATHER_GRID_W;
            int x1 = (gx + 1) * width / WEATHER_GRID_W;
            if (x1 <= x0) x1 = x0 + 1;

            uint32_t sy = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *row = y_plane + (size_t)y * stride;
                for (int x = x0; x < x1; x++) {
                    sy += row[x];
                    n++;
                }
            }
            /* Chroma NV12 : demi-résolution, U et V entrelacés. */
            uint32_t su = 0, sv = 0, nc = 0;
            for (int y = y0 / 2; y < (y1 + 1) / 2; y++) {
                const uint8_t *row = uv_plane + (size_t)y * stride;
                for (int x = x0 / 2; x < (x1 + 1) / 2; x++) {
                    su += row[2 * x];
                    sv += row[2 * x + 1];
                    nc++;
                }
            }
            int i = gy * WEATHER_GRID_W + gx;
            cur_y[i] = (float)sy / (float)n;
            acc->sum_y[i] += cur_y[i];
            acc->sum_u[i] += (float)su / (float)nc;
            acc->sum_v[i] += (float)sv / (float)nc;
        }
    }

    /* Changements entre deux images successives. */
    if (acc->frames > 0) {
        static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
        static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        double sum_diff = 0.0;
        int specks = 0, inner = 0;

        for (int i = 0; i < GRID_SIZE; i++) sum_diff += fabsf(cur_y[i] - acc->prev_y[i]);

        for (int gy = 1; gy < WEATHER_GRID_H - 1; gy++) {
            for (int gx = 1; gx < WEATHER_GRID_W - 1; gx++) {
                int i = gy * WEATHER_GRID_W + gx;
                inner++;
                if (fabsf(cur_y[i] - acc->prev_y[i]) < SPECK_DIFF) continue;
                /* Un point isolé (gouttes, flocons) et pas une grosse zone
                 * en mouvement (voiture, piéton, arbre). */
                int moving_neighbours = 0;
                for (int k = 0; k < 8; k++) {
                    int j = (gy + dy[k]) * WEATHER_GRID_W + gx + dx[k];
                    if (fabsf(cur_y[j] - acc->prev_y[j]) >= SPECK_DIFF) moving_neighbours++;
                }
                if (moving_neighbours <= 2) specks++;
            }
        }
        acc->sum_temporal_diff += sum_diff / GRID_SIZE;
        acc->sum_temporal_specks += (double)specks / inner;
    }

    memcpy(acc->prev_y, cur_y, sizeof(cur_y));
    acc->frames++;
}

int weather_compute_features(const WeatherAccumulator *acc, double sky_ratio, WeatherFeatures *f) {
    if (acc->frames == 0) return -1;
    memset(f, 0, sizeof(*f));
    f->frames = acc->frames;

    static float y[GRID_SIZE], u[GRID_SIZE], v[GRID_SIZE];
    const float inv = 1.0f / (float)acc->frames;
    for (int i = 0; i < GRID_SIZE; i++) {
        y[i] = acc->sum_y[i] * inv;
        u[i] = acc->sum_u[i] * inv - 128.0f;
        v[i] = acc->sum_v[i] * inv - 128.0f;
    }

    int sky_rows = (int)(sky_ratio * WEATHER_GRID_H);
    if (sky_rows < 1) sky_rows = 1;
    if (sky_rows > WEATHER_GRID_H) sky_rows = WEATHER_GRID_H;

    double sum = 0, sum2 = 0, sat = 0;
    double sky_l = 0, sky_b = 0, sky_s = 0;
    for (int gy = 0; gy < WEATHER_GRID_H; gy++) {
        for (int gx = 0; gx < WEATHER_GRID_W; gx++) {
            int i = gy * WEATHER_GRID_W + gx;
            double s = sqrt((double)u[i] * u[i] + (double)v[i] * v[i]);
            sum += y[i];
            sum2 += (double)y[i] * y[i];
            sat += s;
            if (gy < sky_rows) {
                sky_l += y[i];
                sky_b += u[i] - v[i];
                sky_s += s;
            }
        }
    }
    f->luma_mean = sum / GRID_SIZE;
    f->luma_std = sqrt(fmax(0.0, sum2 / GRID_SIZE - f->luma_mean * f->luma_mean));
    f->saturation = sat / GRID_SIZE;
    int sky_n = sky_rows * WEATHER_GRID_W;
    f->sky_luma = sky_l / sky_n;
    f->sky_blue = sky_b / sky_n;
    f->sky_saturation = sky_s / sky_n;

    /* Contours (Sobel) et netteté (laplacien) : le brouillard les écrase. */
    int edges = 0, inner = 0;
    double lap = 0;
    const int W = WEATHER_GRID_W;
    for (int gy = 1; gy < WEATHER_GRID_H - 1; gy++) {
        for (int gx = 1; gx < W - 1; gx++) {
            const float *p = &y[gy * W + gx];
            double sx = (p[-W + 1] + 2 * p[1] + p[W + 1]) - (p[-W - 1] + 2 * p[-1] + p[W - 1]);
            double sy = (p[W - 1] + 2 * p[W] + p[W + 1]) - (p[-W - 1] + 2 * p[-W] + p[-W + 1]);
            if (fabs(sx) + fabs(sy) > EDGE_THRESHOLD) edges++;
            lap += fabs(4.0 * p[0] - p[-1] - p[1] - p[-W] - p[W]);
            inner++;
        }
    }
    f->edge_density = (double)edges / inner;
    f->sharpness = lap / inner;

    if (acc->frames > 1) {
        f->temporal_noise = acc->sum_temporal_diff / (acc->frames - 1);
        f->temporal_specks = acc->sum_temporal_specks / (acc->frames - 1);
    }
    return 0;
}

/* Écart relatif à un seuil, ramené entre 0 et 1. */
static double margin(double value, double threshold) {
    if (fabs(threshold) < 1e-9) return 1.0;
    double m = fabs(value - threshold) / fabs(threshold);
    return m > 1.0 ? 1.0 : m;
}

static double confidence(double m) {
    return 0.5 + 0.5 * m;
}

void weather_classify(const WeatherFeatures *f, const WeatherThresholds *t, WeatherResult *r) {
    r->precipitation = f->temporal_specks > t->precip_specks;

    /* Nuit : image sombre, ou caméra passée en mode IR (image en noir et blanc). */
    if (f->luma_mean < t->night_luma) {
        r->condition = WEATHER_NIGHT;
        r->confidence = confidence(margin(f->luma_mean, t->night_luma));
        return;
    }
    if (f->saturation < t->ir_saturation && f->luma_mean < t->snow_luma) {
        r->condition = WEATHER_NIGHT;
        r->confidence = 0.6;
        return;
    }

    if (r->precipitation) {
        if (f->luma_mean > t->snow_luma && f->saturation < t->sunny_saturation) {
            r->condition = WEATHER_SNOW;
            r->confidence = confidence(fmin(margin(f->temporal_specks, t->precip_specks),
                                            margin(f->luma_mean, t->snow_luma)));
        } else {
            r->condition = WEATHER_RAIN;
            r->confidence = confidence(margin(f->temporal_specks, t->precip_specks));
        }
        return;
    }

    if (f->edge_density < t->fog_edge_density && f->luma_std < t->fog_contrast) {
        r->condition = WEATHER_FOG;
        r->confidence = confidence(fmin(margin(f->edge_density, t->fog_edge_density),
                                        margin(f->luma_std, t->fog_contrast)));
        return;
    }

    if (f->sky_luma > t->sunny_sky_luma && f->saturation > t->sunny_saturation) {
        r->condition = WEATHER_SUNNY;
        r->confidence = confidence(fmin(margin(f->sky_luma, t->sunny_sky_luma),
                                        margin(f->saturation, t->sunny_saturation)));
        return;
    }

    r->condition = WEATHER_CLOUDY;
    r->confidence = confidence(fmin(margin(f->saturation, t->sunny_saturation),
                                    margin(f->edge_density, t->fog_edge_density)));
}

const char *weather_condition_code(WeatherCondition c) {
    static const char *codes[WEATHER_COUNT] = {"sunny", "cloudy", "fog", "rain", "snow", "night"};
    return (c >= 0 && c < WEATHER_COUNT) ? codes[c] : "unknown";
}

const char *weather_condition_label(WeatherCondition c) {
    static const char *labels[WEATHER_COUNT] = {"Ensoleillé", "Nuageux", "Brouillard",
                                                "Pluie", "Neige", "Nuit"};
    return (c >= 0 && c < WEATHER_COUNT) ? labels[c] : "Inconnu";
}

WeatherCondition weather_majority(const WeatherCondition *history, int count) {
    int votes[WEATHER_COUNT] = {0};
    for (int i = 0; i < count; i++) votes[history[i]]++;
    /* En cas d'égalité, la plus récente l'emporte. */
    WeatherCondition best = count > 0 ? history[count - 1] : WEATHER_CLOUDY;
    for (int c = 0; c < WEATHER_COUNT; c++)
        if (votes[c] > votes[best]) best = (WeatherCondition)c;
    return best;
}

int weather_to_json(char *buf, size_t size, const WeatherResult *instant, WeatherCondition stable,
                    const WeatherFeatures *f, long timestamp) {
    return snprintf(buf, size,
                    "{\"timestamp\":%ld,"
                    "\"condition\":\"%s\",\"label\":\"%s\","
                    "\"instant\":{\"condition\":\"%s\",\"label\":\"%s\",\"confidence\":%.2f},"
                    "\"precipitation\":%s,"
                    "\"features\":{\"frames\":%d,\"luma_mean\":%.1f,\"luma_std\":%.1f,"
                    "\"saturation\":%.2f,\"edge_density\":%.4f,\"sharpness\":%.2f,"
                    "\"sky_luma\":%.1f,\"sky_blue\":%.2f,\"sky_saturation\":%.2f,"
                    "\"temporal_noise\":%.2f,\"temporal_specks\":%.5f}}",
                    timestamp, weather_condition_code(stable), weather_condition_label(stable),
                    weather_condition_code(instant->condition),
                    weather_condition_label(instant->condition), instant->confidence,
                    instant->precipitation ? "true" : "false", f->frames, f->luma_mean, f->luma_std,
                    f->saturation, f->edge_density, f->sharpness, f->sky_luma, f->sky_blue,
                    f->sky_saturation, f->temporal_noise, f->temporal_specks);
}
