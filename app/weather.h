/*
 * weather.h - Estimation de la météo à partir d'images NV12.
 *
 * Code indépendant de VDO / du SDK Axis, pour pouvoir le tester sur PC.
 */
#ifndef WEATHER_H
#define WEATHER_H

#include <stddef.h>
#include <stdint.h>

/* Résolution de la grille d'analyse (les images sont réduites à cette taille). */
#define WEATHER_GRID_W 160
#define WEATHER_GRID_H 90

typedef enum {
    WEATHER_SUNNY = 0,
    WEATHER_CLOUDY,
    WEATHER_FOG,
    WEATHER_RAIN,
    WEATHER_SNOW,
    WEATHER_NIGHT,
    WEATHER_COUNT
} WeatherCondition;

/* Accumule plusieurs images d'une même rafale pour en faire la moyenne. */
typedef struct {
    float sum_y[WEATHER_GRID_W * WEATHER_GRID_H];
    float sum_u[WEATHER_GRID_W * WEATHER_GRID_H];
    float sum_v[WEATHER_GRID_W * WEATHER_GRID_H];
    float prev_y[WEATHER_GRID_W * WEATHER_GRID_H];
    double sum_temporal_diff;   /* moyenne |Y(t) - Y(t-1)| cumulée */
    double sum_temporal_specks; /* part de pixels qui "scintillent" cumulée */
    int frames;
} WeatherAccumulator;

/* Indicateurs calculés sur l'image moyenne (exposés en JSON pour calibrer). */
typedef struct {
    double luma_mean;        /* luminosité moyenne 0..255 */
    double luma_std;         /* contraste global */
    double saturation;       /* saturation moyenne (norme chroma), ~0 en mode IR */
    double edge_density;     /* part de pixels avec un contour net, 0..1 */
    double sharpness;        /* moyenne |laplacien| */
    double sky_luma;         /* luminosité de la zone ciel (haut de l'image) */
    double sky_blue;         /* "bleuité" du ciel : (U-128) - (V-128) */
    double sky_saturation;
    double temporal_noise;   /* variation moyenne entre images successives */
    double temporal_specks;  /* part de pixels qui scintillent (pluie/neige) */
    int frames;
} WeatherFeatures;

/* Seuils de décision, réglables via les paramètres de l'ACAP. */
typedef struct {
    double night_luma;          /* en dessous : nuit */
    double ir_saturation;       /* en dessous : image N&B, caméra en mode IR => nuit */
    double fog_edge_density;    /* en dessous : brouillard possible */
    double fog_contrast;        /* et contraste en dessous : brouillard */
    double precip_specks;       /* au dessus : précipitations */
    double snow_luma;           /* précipitations + image claire et peu saturée : neige */
    double sunny_sky_luma;      /* ciel au dessus : ensoleillé possible */
    double sunny_saturation;    /* et saturation au dessus : ensoleillé */
    double sky_ratio;           /* part haute de l'image considérée comme ciel (0..1) */
} WeatherThresholds;

typedef struct {
    WeatherCondition condition;
    double confidence;          /* 0..1, heuristique */
    int precipitation;          /* 1 si pluie/neige détectée (même la nuit) */
} WeatherResult;

void weather_thresholds_default(WeatherThresholds *t);

void weather_acc_reset(WeatherAccumulator *acc);

/* Ajoute une image NV12 (plan Y puis plan UV entrelacé) à l'accumulateur. */
void weather_acc_add_nv12(WeatherAccumulator *acc, const uint8_t *y_plane, const uint8_t *uv_plane,
                          int width, int height, int stride);

/* Calcule les indicateurs sur l'image moyenne. Retourne 0 si OK, -1 si aucune image. */
int weather_compute_features(const WeatherAccumulator *acc, double sky_ratio, WeatherFeatures *f);

void weather_classify(const WeatherFeatures *f, const WeatherThresholds *t, WeatherResult *r);

const char *weather_condition_code(WeatherCondition c);  /* "sunny", "fog", ... */
const char *weather_condition_label(WeatherCondition c); /* "Ensoleillé", ... */

/* Vote majoritaire sur l'historique des rafales (lissage dans le temps). */
WeatherCondition weather_majority(const WeatherCondition *history, int count);

/* Écrit le résultat en JSON dans buf. Retourne le nombre de caractères écrits. */
int weather_to_json(char *buf, size_t size, const WeatherResult *instant, WeatherCondition stable,
                    const WeatherFeatures *f, long timestamp);

#endif
