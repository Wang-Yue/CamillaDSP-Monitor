#include "dither.h"

struct noise_shaper {
  double* filter;
  double* buffer;
  size_t filter_count;
  size_t write_index;
};

struct dither_filter {
  char name[64];
  dither_type_t type;
  double scalefact;
  double amplitude;
  noise_shaper_t* shaper;
  double previous_sample;
  uint32_t rng_state;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

// MARK: - NoiseShaper
noise_shaper_t* noise_shaper_create(const double* filter_coeffs, size_t count) {
  if (!filter_coeffs || count == 0) return NULL;
  noise_shaper_t* shaper = (noise_shaper_t*)calloc(1, sizeof(noise_shaper_t));
  if (!shaper) return NULL;
  shaper->filter = (double*)malloc(count * sizeof(double));
  shaper->buffer = (double*)calloc(count, sizeof(double));
  if (!shaper->filter || !shaper->buffer) {
    noise_shaper_free(shaper);
    return NULL;
  }
  memcpy(shaper->filter, filter_coeffs, count * sizeof(double));
  shaper->filter_count = count;
  shaper->write_index = 0;
  return shaper;
}

double noise_shaper_process(noise_shaper_t* shaper, double scaled,
                            double dither) {
  if (!shaper || shaper->filter_count == 0) return round(scaled + dither);
  double filt_buf = 0.0;
  size_t count = shaper->filter_count;
  // Apply feedback filter to past quantization errors stored in the circular
  // buffer. The buffer stores [error[n-count], ..., error[n-1]]. The filter
  // coefficients are applied in reverse order.
  for (size_t i = 0; i < count; i++) {
    size_t buf_idx = (shaper->write_index + i) % count;
    size_t coeff_idx = count - 1 - i;
    filt_buf += shaper->filter[coeff_idx] * shaper->buffer[buf_idx];
  }
  // Add filtered error to current sample.
  double scaled_plus_err = scaled + filt_buf;
  // Add dither and perform quantization (rounding to nearest integer).
  double result = scaled_plus_err + dither;
  double result_r = round(result);
  // Calculate new quantization error (input to quantizer minus quantized
  // output). Note: we exclude the dither from the error calculation to avoid
  // shaping the dither itself, or rather we shape the total error including
  // feedback.
  double error = scaled_plus_err - result_r;
  // Save error in circular buffer and advance write index.
  shaper->buffer[shaper->write_index] = error;
  shaper->write_index = (shaper->write_index + 1) % count;
  return result_r;
}

void noise_shaper_free(noise_shaper_t* shaper) {
  if (!shaper) return;
  if (shaper->filter) free(shaper->filter);
  if (shaper->buffer) free(shaper->buffer);
  free(shaper);
}

// MARK: - Noise Shaper Factory
noise_shaper_t* noise_shaper_create_for_type(dither_type_t type) {
  switch (type) {
    case DITHER_TYPE_FWEIGHTED_441: {
      const double c[] = {2.412,  -3.370, 3.937,  -4.174, 3.353,
                          -2.205, 1.281,  -0.569, 0.0847};
      return noise_shaper_create(c, 9);
    }
    case DITHER_TYPE_FWEIGHTED_LONG_441: {
      const double c[] = {2.391510,  -3.284444, 3.679506,  -3.635044, 2.524185,
                          -1.146701, 0.115354,  0.513745,  -0.749277, 0.512386,
                          -0.749277, 0.512386,  -0.188997, -0.043705, 0.149843,
                          -0.151186, 0.076302,  -0.012070, -0.021127, 0.025232,
                          -0.016121, 0.004453,  0.000876,  -0.001799, 0.000774,
                          -0.000128};
      return noise_shaper_create(c, 26);
    }
    case DITHER_TYPE_FWEIGHTED_SHORT_441: {
      const double c[] = {1.623, -0.982, 0.109};
      return noise_shaper_create(c, 3);
    }
    case DITHER_TYPE_GESEMANN_441: {
      const double c[] = {2.2061, -0.4706, -0.2534, -0.6214,
                          1.0587, 0.0676,  -0.6054, -0.2738};
      return noise_shaper_create(c, 8);
    }
    case DITHER_TYPE_GESEMANN_48: {
      const double c[] = {2.2374, -0.7339, -0.1251, -0.6033,
                          0.903,  0.0116,  -0.5853, -0.2571};
      return noise_shaper_create(c, 8);
    }
    case DITHER_TYPE_LIPSHITZ_441: {
      const double c[] = {2.033, -2.165, 1.959, -1.590, 0.6149};
      return noise_shaper_create(c, 5);
    }
    case DITHER_TYPE_LIPSHITZ_LONG_441: {
      const double c[] = {2.847,  -4.685, 6.214,  -7.184, 6.639,
                          -5.032, 3.263,  -1.632, 0.4191};
      return noise_shaper_create(c, 9);
    }
    case DITHER_TYPE_SHIBATA_441: {
      const double c[] = {1.3568638563156128,      -1.225293517112732,
                          0.623555064201355,       -0.22562094032764435,
                          -0.23557975888252258,    0.1353636234998703,
                          -0.09153814613819122,    -0.056445639580488205,
                          3.9614424167666584e-5,   -0.02356191910803318,
                          -0.010756319388747215,   -0.00031949131516739726,
                          0.0014337620232254267,   -0.008455123752355576,
                          -0.00021318180370144546, 7.617592200404033e-5,
                          0.0010102330707013607,   4.50302759418264e-5,
                          0.001343382173217833,    0.0013937242329120636,
                          0.000433067005360499,    0.0004694978706538677,
                          0.00014775841555092484,  -4.1060175135498866e-5};
      return noise_shaper_create(c, 24);
    }
    case DITHER_TYPE_SHIBATA_HIGH_441: {
      const double c[] = {
          2.826326608657837,      -5.35343599319458,    7.804205894470215,
          -9.67936897277832,      10.157135009765625,   -9.439995765686035,
          7.614612579345703,      -5.424517631530762,   3.247828245162964,
          -1.6301852464675903,    0.5853801965713501,   -0.11710002273321152,
          -0.0335436686873436,    0.008884146809577942, 0.017314357683062553,
          -0.03326272964477539,   0.018168220296502113, -0.006801502779126167,
          -0.0009691194863989949, 0.0009648934355936944};
      return noise_shaper_create(c, 20);
    }
    case DITHER_TYPE_SHIBATA_LOW_441: {
      const double c[] = {
          0.5954378247261047,     -0.002507873112335801, -0.18518058955669403,
          -0.0010374293196946383, -0.10366342961788177,  -0.053248628973960876,
          -8.403004903811961e-5,  -3.856993302520095e-8, -0.02641301043331623,
          -0.000684383965563029,  3.1580505037709372e-6, 0.03173962980508804};
      return noise_shaper_create(c, 12);
    }
    case DITHER_TYPE_SHIBATA_48: {
      const double c[] = {1.4919577836990356,      -1.3089178800582886,
                          0.5405163168907166,      -0.00036113749956712127,
                          -0.36303195357322693,    0.10911127924919128,
                          0.007310638204216957,    -0.115459144115448,
                          0.003772285534068942,    -0.012545258738100529,
                          -0.02927248738706112,    -0.00500220013782382,
                          -0.00020218851568643004, -0.0049057346768677235,
                          -0.005127976182848215,   -0.002505671000108123};
      return noise_shaper_create(c, 16);
    }
    case DITHER_TYPE_SHIBATA_HIGH_48: {
      const double c[] = {
          3.2601516246795654,    -6.55756950378418,     9.748664855957031,
          -11.713088989257813,   11.50462818145752,     -9.485962867736816,
          6.40427303314209,      -3.4772820472717285,   1.3327382802963257,
          -0.2646457552909851,   -0.08182330429553986,  0.04464340955018997,
          0.021642472594976425,  -0.04283212125301361,  0.0033832620829343796,
          0.016050558537244797,  -0.01944376900792122,  0.0020140456035733223,
          0.005101846531033516,  -0.004944144282490015, -0.001399693894200027,
          0.003581011900678277,  -0.002209919737651944, -0.00010120005026692525,
          0.0007712086662650108, -4.772754982695915e-5, -0.00047057875781320035,
          0.0005352201405912638};
      return noise_shaper_create(c, 28);
    }
    case DITHER_TYPE_SHIBATA_LOW_48: {
      const double c[] = {0.6481543779373169,      -0.0001329232909483835,
                          -0.1528443992137909,     -0.024795081466436386,
                          -0.02887929417192936,    -0.09774130582809448,
                          3.7233345210552216e-5,   3.0361816243384965e-6,
                          -2.6851517759496346e-5,  -0.015118855983018875,
                          -0.00011908156011486426, 4.020391770609422e-6,
                          0.03214230760931969};
      return noise_shaper_create(c, 13);
    }
    case DITHER_TYPE_SHIBATA_882: {
      const double c[] = {2.0752036571502686,      -1.4316110610961914,
                          -4.1018622141564265e-5,  0.3074778616428375,
                          0.015034947544336319,    -0.002069007372483611,
                          -0.09544544667005539,    -0.017573365941643715,
                          0.001514684408903122,    0.00971572007983923,
                          0.0032300157472491264,   -0.0011662221513688564,
                          -0.012702429667115211,   -0.01368053536862135,
                          -0.00032695711706764996, -0.00033481238642707467,
                          0.0019418919691815972,   -0.006559844594448805,
                          -0.003184868488460779,   -0.001185707631520927};
      return noise_shaper_create(c, 20);
    }
    case DITHER_TYPE_SHIBATA_LOW_882: {
      const double c[] = {0.8127508163452148,     1.3415416333373287e-7,
                          -1.4003169781062752e-5, -0.02736665867269039,
                          -0.06308479607105255,   -0.00041124963900074363,
                          -0.0014667811337858438, -0.0034636424388736486,
                          -0.014447951689362526,  -0.05068640038371086};
      return noise_shaper_create(c, 10);
    }
    case DITHER_TYPE_SHIBATA_96: {
      const double c[] = {
          2.104111433029175,    -1.4101417064666748,  -0.003514738753437996,
          0.18617971241474152,  0.11117676645517349,  -0.0013629450695589185,
          -0.05544671788811684, -0.05685991421341896, -0.0039573232643306255,
          0.002566334791481495, 0.01409075316041708,  0.006225708406418562,
          -0.00653973501175642, -0.019066527485847473};
      return noise_shaper_create(c, 14);
    }
    case DITHER_TYPE_SHIBATA_LOW_96: {
      const double c[] = {0.8336278200149536,    4.7663510827078426e-7,
                          -5.592720481217839e-5, -0.0009176760795526206,
                          -0.0850192978978157,   -0.0003086409706156701,
                          -2.747484904830344e-5, -3.4470554965082556e-5,
                          -0.006816617213189602, -0.005103240255266428,
                          -0.0483102910220623};
      return noise_shaper_create(c, 11);
    }
    case DITHER_TYPE_SHIBATA_192: {
      const double c[] = {
          2.1174826622009277,     -0.7930012941360474,   -0.5887165069580078,
          -0.004517062101513147,  -2.240059620817192e-5, 0.3498106598854065,
          0.0014674699632450938,  -0.03528605028986931,  -0.030574915930628777,
          -0.008099924772977829,  -0.024920884519815445, -0.010276389308273792,
          -0.0028273388743400574, 0.011965871788561344};
      return noise_shaper_create(c, 14);
    }
    case DITHER_TYPE_SHIBATA_LOW_192: {
      const double c[] = {0.9298678636550903,     2.375700432821759e-6,
                          1.3239204008641536e-6,  4.53364457086991e-8,
                          -1.0855699201783864e-6, -7.519394671362534e-7,
                          -0.01057471428066492,   -0.01539737917482853,
                          -0.007173464633524418,  -0.004041632637381554};
      return noise_shaper_create(c, 10);
    }
    default:
      return NULL;
  }
}

// MARK: - Ditherers
/**
 * @brief Generates a pseudo-random 32-bit unsigned integer using XORShift.
 *
 * XORShift is a class of pseudorandom number generators that are simple and
 * fast.
 *
 * @param state Pointer to the 32-bit seed state.
 * @return A pseudo-random 32-bit integer.
 */
static inline uint32_t xorshift32(uint32_t* state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/**
 * @brief Generates a pseudo-random double value uniformly distributed in [0,
 * 1].
 *
 * Uses xorshift32 and scales the output.
 *
 * @param state Pointer to the RNG state.
 * @return A double between 0.0 and 1.0.
 */
static inline double sample_rng_0_1(uint32_t* state) {
  uint32_t val = xorshift32(state);
  return (double)val / (double)4294967295.0;  // 2^32 - 1
}

/**
 * @brief Samples a dither value based on the filter's configured dither type.
 *
 * Supports:
 * - Flat TPDF (Triangular Probability Density Function) dither, which has a
 * flat frequency spectrum (white noise). It is generated using inverse
 * transform sampling.
 * - High-pass TPDF dither, which is created by high-pass filtering (subtracting
 * the previous sample) a uniform (rectangular) white noise distribution. This
 * shifts dither energy to higher, less audible frequencies.
 *
 * @param filter The dither filter instance containing RNG state and dither
 * parameters.
 * @return The generated dither sample.
 */
static double sample_dither(dither_filter_t* filter) {
  if (filter->type == DITHER_TYPE_NONE) return 0.0;
  double half_amp = filter->amplitude / 2.0;
  if (filter->type == DITHER_TYPE_FLAT) {
    // Generate TPDF dither using inverse transform sampling on [a, b] with peak
    // at c.
    double u = sample_rng_0_1(&filter->rng_state);
    double a = -half_amp;
    double b = half_amp;
    double c = 0.0;
    double fc = (c - a) / (b - a);
    if (u < fc) {
      return a + sqrt(u * (b - a) * (c - a));
    } else {
      return b - sqrt((1.0 - u) * (b - a) * (b - c));
    }
  } else if (filter->type == DITHER_TYPE_HIGHPASS) {
    // Generate high-pass TPDF dither by subtracting previous rectangular dither
    // sample from the current rectangular dither sample.
    double u = sample_rng_0_1(&filter->rng_state);
    double new_sample = (2.0 * u - 1.0) * half_amp;
    double high_passed = new_sample - filter->previous_sample;
    filter->previous_sample = new_sample;
    return high_passed;
  }
  return 0.0;
}

// MARK: - DitherFilter
dither_filter_t* dither_filter_create(const char* name,
                                      const dither_parameters_t* params) {
  dither_filter_t* filter = (dither_filter_t*)malloc(sizeof(dither_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "dither");
  }

  // Initialize RNG seed
  filter->rng_state = 123456789U;
  if (name) {
    uint32_t hash = 5381;
    for (const char* p = name; *p; p++) {
      hash = ((hash << 5) + hash) + (uint8_t)*p;
    }
    if (hash != 0) {
      filter->rng_state = hash;
    }
  }

  int bits = params ? params->bits : 16;
  dither_type_t dither_type = params ? params->type : DITHER_TYPE_NONE;
  filter->scalefact = pow(2.0, (double)(bits - 1));
  filter->type = dither_type;
  filter->previous_sample = 0.0;

  if (dither_type == DITHER_TYPE_FLAT) {
    filter->amplitude =
        (params && params->has_amplitude) ? params->amplitude : 2.0;
  } else if (dither_type == DITHER_TYPE_HIGHPASS) {
    filter->amplitude = 2.0;
  } else {
    filter->amplitude = 0.0;
  }

  if (dither_type != DITHER_TYPE_NONE && dither_type != DITHER_TYPE_FLAT &&
      dither_type != DITHER_TYPE_HIGHPASS) {
    filter->shaper = noise_shaper_create_for_type(dither_type);
    if (!filter->shaper) {
      free(filter);
      return NULL;
    }
  } else {
    filter->shaper = NULL;
  }
  return filter;
}

void dither_filter_process(dither_filter_t* filter, mutable_waveform_t waveform,
                           size_t count) {
  if (!filter || !waveform || count == 0) return;
  double scalefact = filter->scalefact;
  for (size_t i = 0; i < count; i++) {
    double scaled = waveform[i] * scalefact;
    double dither = sample_dither(filter);
    double result_r = 0.0;
    if (filter->shaper) {
      result_r = noise_shaper_process(filter->shaper, scaled, dither);
    } else {
      result_r = round(scaled + dither);
    }
    waveform[i] = result_r / scalefact;
  }
}

void dither_filter_free(dither_filter_t* filter) {
  if (!filter) return;
  if (filter->shaper) noise_shaper_free(filter->shaper);
  free(filter);
}
