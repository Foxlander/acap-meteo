/*
 * Tests de la classification sur des scènes synthétiques (à lancer sur PC).
 *   make -C tests
 */
#include "../app/weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 360

typedef struct {
    int sky_y, sky_u, sky_v;    /* couleur du ciel (haut de l'image) */
    int ground_y, texture;      /* luminosité du sol et amplitude des motifs */
    int ground_chroma;          /* saturation du sol */
    int specks;                 /* nombre de gouttes/flocons par image */
    int speck_y;                /* luminosité des gouttes/flocons */
} Scene;

static uint8_t clamp8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void render(const Scene *s, uint8_t *y, uint8_t *uv, unsigned *seed) {
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            int v;
            if (r < H / 3) {
                v = s->sky_y;
            } else {
                /* Sol avec des motifs (bâtiments, routes...) */
                int checker = ((r / 24) + (c / 24)) & 1;
                v = s->ground_y + (checker ? s->texture : -s->texture);
            }
            y[r * W + c] = clamp8(v);
        }
    }
    for (int r = 0; r < H / 2; r++) {
        for (int c = 0; c < W / 2; c++) {
            int u = 128, v = 128;
            if (r * 2 < H / 3) {
                u = s->sky_u;
                v = s->sky_v;
            } else {
                int checker = ((r * 2 / 24) + (c * 2 / 24)) & 1;
                u = 128 + (checker ? s->ground_chroma : -s->ground_chroma);
                v = 128 - (checker ? s->ground_chroma : -s->ground_chroma) / 2;
            }
            uv[r * W + 2 * c] = clamp8(u);
            uv[r * W + 2 * c + 1] = clamp8(v);
        }
    }
    /* Gouttes / flocons : petits traits à des positions aléatoires. */
    for (int k = 0; k < s->specks; k++) {
        int x = rand_r(seed) % (W - 2), r0 = rand_r(seed) % (H - 6);
        for (int d = 0; d < 6; d++) y[(r0 + d) * W + x] = (uint8_t)s->speck_y;
    }
}

static WeatherCondition run(const char *name, const Scene *s, WeatherCondition expected) {
    static uint8_t y[W * H], uv[W * H / 2];
    WeatherAccumulator *acc = malloc(sizeof(*acc));
    WeatherThresholds t;
    WeatherFeatures f;
    WeatherResult r;
    unsigned seed = 42;

    weather_thresholds_default(&t);
    weather_acc_reset(acc);
    for (int i = 0; i < 5; i++) {
        render(s, y, uv, &seed);
        weather_acc_add_nv12(acc, y, uv, W, H, W);
    }
    weather_compute_features(acc, t.sky_ratio, &f);
    weather_classify(&f, &t, &r);

    char json[1024];
    weather_to_json(json, sizeof(json), &r, r.condition, &f, 0);
    printf("%-9s attendu=%-7s obtenu=%-7s %s\n          %s\n", name, weather_condition_code(expected),
           weather_condition_code(r.condition), r.condition == expected ? "OK" : "ECHEC", json);
    free(acc);
    return r.condition;
}

int main(void) {
    int failures = 0;
    /*                    ciel Y  U    V    sol Y tex sat gouttes lum */
    Scene sunny  = {210, 160, 108, 120, 60, 30, 0,   0};
    Scene cloudy = {185, 128, 128, 110, 45, 8,  0,   0};
    Scene fog    = {175, 128, 128, 165, 4,  2,  0,   0};
    Scene night  = {12,  128, 128, 20,  8,  0,  0,   0};
    Scene rain   = {150, 128, 128, 100, 40, 6,  400, 200};
    Scene snow   = {200, 128, 128, 190, 15, 3,  400, 255};

    failures += run("sunny", &sunny, WEATHER_SUNNY) != WEATHER_SUNNY;
    failures += run("cloudy", &cloudy, WEATHER_CLOUDY) != WEATHER_CLOUDY;
    failures += run("fog", &fog, WEATHER_FOG) != WEATHER_FOG;
    failures += run("night", &night, WEATHER_NIGHT) != WEATHER_NIGHT;
    failures += run("rain", &rain, WEATHER_RAIN) != WEATHER_RAIN;
    failures += run("snow", &snow, WEATHER_SNOW) != WEATHER_SNOW;

    WeatherCondition h[] = {WEATHER_RAIN, WEATHER_CLOUDY, WEATHER_RAIN, WEATHER_SUNNY};
    if (weather_majority(h, 4) != WEATHER_RAIN) {
        printf("majority ECHEC\n");
        failures++;
    }

    printf("%s (%d échec(s))\n", failures ? "ECHEC" : "OK", failures);
    return failures ? 1 : 0;
}
