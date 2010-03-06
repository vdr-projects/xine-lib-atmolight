/*
 * Copyright (C) 2009, 2010 Andreas Auras
 *
 * This file is part of the atmo post plugin, a plugin for the free xine video player.
 *
 * atmo post plugin is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * atmo post plugin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 *
 * Many ideas and algorithm in this module had been derived from the fantastic
 * Atmolight-plugin for the Video Disk Recorder (VDR) that was developed by
 * Eike Edener.
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>
#include <sys/time.h>

#include <xine/post.h>

extern long int lround(double); /* Missing in math.h? */

#undef LOG_MODULE
#define LOG_MODULE      "atmo"
#define LOG_1           1
#define LOG_2           0


#define OUTPUT_RATE                     20     /* rate of output loop [ms] */
#define GRAB_TIMEOUT                   100     /* max. time waiting for next grab image [ms] */
#define THREAD_TERMINATION_WAIT_TIME   150     /* time waiting for thread termination [ms] */

#define NUM_AREAS                       9      /* Number of different areas (top, bottom ...) */

/* accuracy of color calculation */
#define h_MAX   255
#define s_MAX   255
#define v_MAX   255

/* macros */
#define MIN(X, Y)  ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y)  ((X) > (Y) ? (X) : (Y))
#define POS_DIV(a, b)  ( (a)/(b) + ( ((a)%(b) >= (b)/2 ) ? 1 : 0) )


typedef struct { uint8_t h, s, v; } hsv_color_t;
typedef struct { uint8_t r, g, b; } rgb_color_t;
typedef struct { uint64_t r, g, b; } rgb_color_sum_t;

/*
 * Plugin
 */

typedef struct {
  int enabled;
  int driver;
  char driver_param[256];
  int top;
  int bottom;
  int left;
  int right;
  int center;
  int top_left;
  int top_right;
  int bottom_left;
  int bottom_right;
  int analyze_rate;
  int analyze_size;
  int overscan;
  int darkness_limit;
  int edge_weighting;
  int hue_win_size;
  int sat_win_size;
  int hue_threshold;
  int brightness;
  int filter;
  int filter_smoothness;
  int filter_length;
  int filter_threshold;
  int wc_red;
  int wc_green;
  int wc_blue;
  int gamma;
  int start_delay;
} atmo_parameters_t;


#include "output_driver.h"


#define NUM_FILTERS     2
static char *filter_enum[NUM_FILTERS+1] = { "off", "percentage", "combined" };


START_PARAM_DESCR(atmo_parameters_t)
PARAM_ITEM(POST_PARAM_TYPE_BOOL, enabled, NULL, 0, 1, 0,
  "enable plugin")
PARAM_ITEM(POST_PARAM_TYPE_INT, driver, driver_enum, 0, NUM_DRIVERS, 0,
  "output driver")
PARAM_ITEM(POST_PARAM_TYPE_CHAR, driver_param, NULL, 0, 0, 0,
  "parameters for output driver")
PARAM_ITEM(POST_PARAM_TYPE_INT, top, NULL, 0, 25, 0,
  "number of areas at top border")
PARAM_ITEM(POST_PARAM_TYPE_INT, bottom, NULL, 0, 25, 0,
  "number of areas at bottom border")
PARAM_ITEM(POST_PARAM_TYPE_INT, left, NULL, 0, 25, 0,
  "number of areas at left border")
PARAM_ITEM(POST_PARAM_TYPE_INT, right, NULL, 0, 25, 0,
  "number of areas at right border")
PARAM_ITEM(POST_PARAM_TYPE_INT, center, NULL, 0, 1, 0,
  "activate center area")
PARAM_ITEM(POST_PARAM_TYPE_INT, top_left, NULL, 0, 1, 0,
  "activate top_left area")
PARAM_ITEM(POST_PARAM_TYPE_INT, top_right, NULL, 0, 1, 0,
  "activate top_right area")
PARAM_ITEM(POST_PARAM_TYPE_INT, bottom_left, NULL, 0, 1, 0,
  "activate bottom_left area")
PARAM_ITEM(POST_PARAM_TYPE_INT, bottom_right, NULL, 0, 1, 0,
  "activate bottom right area")
PARAM_ITEM(POST_PARAM_TYPE_INT, analyze_rate, NULL, 10, 500, 0,
  "analyze rate [ms]")
PARAM_ITEM(POST_PARAM_TYPE_INT, analyze_size, NULL, 0, 3, 0,
  "size of analyze image")
PARAM_ITEM(POST_PARAM_TYPE_INT, overscan, NULL, 0, 200, 0,
  "ignored overscan border of grabbed image [%1000]")
PARAM_ITEM(POST_PARAM_TYPE_INT, darkness_limit, NULL, 0, 100, 0,
  "limit for black pixel")
PARAM_ITEM(POST_PARAM_TYPE_INT, edge_weighting, NULL, 10, 200, 0,
  "power of edge weighting")
PARAM_ITEM(POST_PARAM_TYPE_INT, hue_win_size, NULL, 0, 5, 0,
  "hue windowing size")
PARAM_ITEM(POST_PARAM_TYPE_INT, sat_win_size, NULL, 0, 5, 0,
  "saturation windowing size")
PARAM_ITEM(POST_PARAM_TYPE_INT, hue_threshold, NULL, 0, 100, 0,
  "hue threshold [%]")
PARAM_ITEM(POST_PARAM_TYPE_INT, brightness, NULL, 50, 300, 0,
  "brightness [%]")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter, filter_enum, 0, NUM_FILTERS, 0,
  "filter mode")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter_smoothness, NULL, 1, 100, 0,
  "filter smoothness [%]")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter_length, NULL, 300, 5000, 0,
  "filter length [ms]")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter_threshold, NULL, 1, 100, 0,
  "filter threshold [%]")
PARAM_ITEM(POST_PARAM_TYPE_INT, wc_red, NULL, 0, 255, 0,
  "white calibration correction factor of red color channel")
PARAM_ITEM(POST_PARAM_TYPE_INT, wc_green, NULL, 0, 255, 0,
  "white calibration correction factor of green color channel")
PARAM_ITEM(POST_PARAM_TYPE_INT, wc_blue, NULL, 0, 255, 0,
  "white calibration correction factor of blue color channel")
PARAM_ITEM(POST_PARAM_TYPE_INT, gamma, NULL, 0, 30, 0,
  "gamma correction factor")
PARAM_ITEM(POST_PARAM_TYPE_INT, start_delay, NULL, 0, 5000, 0,
  "delay after stream start before first output is send [ms]")
END_PARAM_DESCR(atmo_param_descr)


typedef struct {
  post_class_t post_class;
  xine_t *xine;
} atmo_post_class_t;


typedef struct atmo_post_plugin_s
{
    /* xine related */
  post_plugin_t post_plugin;
  xine_post_in_t parameter_input;
  atmo_parameters_t parm;
  atmo_parameters_t default_parm;
  post_video_port_t *port;
  pthread_mutex_t port_lock;

    /* channel configuration related */
  atmo_parameters_t active_parm;
  int sum_channels;

  /* thread related */
  int *grab_running, *output_running;
  pthread_t grab_thread, output_thread;
  pthread_mutex_t lock;
  pthread_cond_t thread_started;

    /* output related */
  output_driver_t *output_driver;
  output_drivers_t output_drivers;
  int driver_opened;
  rgb_color_t *output_colors, *last_output_colors;

    /* analyze related */
  uint64_t *hue_hist, *sat_hist;
  uint64_t *w_hue_hist, *w_sat_hist;
  uint64_t *avg_bright;
  int *most_used_hue, *last_most_used_hue, *most_used_sat, *avg_cnt;
  rgb_color_t *analyzed_colors;

    /* filter related */
  rgb_color_t *filtered_colors;
  rgb_color_t *mean_filter_values;
  rgb_color_sum_t *mean_filter_sum_values;
  int old_mean_length;
} atmo_post_plugin_t;


static int build_post_api_parameter_string(char *buf, int size, xine_post_api_descr_t *descr, void *values, void *defaults) {
  xine_post_api_parameter_t *p = descr->parameter;
  int sep = 0;
  char arg[512];

  while (p->type != POST_PARAM_TYPE_LAST) {
    if (!p->readonly) {
      char *v = (char *)values + p->offset;
      char *d = (char *)defaults + p->offset;
      arg[0] = 0;
      switch (p->type) {
      case POST_PARAM_TYPE_INT:
      case POST_PARAM_TYPE_BOOL:
        if (*((int *)v) != *((int *)d))
          snprintf(arg, sizeof(arg), "%s=%d", p->name, *((int *)v));
        break;
      case POST_PARAM_TYPE_DOUBLE:
        if (*((double *)v) != *((double *)d))
          snprintf(arg, sizeof(arg), "%s=%f", p->name, *((double *)v));
        break;
      case POST_PARAM_TYPE_CHAR:
        if (strncmp(v, d, p->size))
          snprintf(arg, sizeof(arg), "%s=%.*s", p->name, p->size, v);
        break;
      }
      if (arg[0]) {
        int n = strlen(arg) + sep;
        if (size < n)
          break;
        if (sep)
          *buf = ',';
        strcpy(buf + sep, arg);
        buf += n;
        size -= n;
        sep = 1;
      }
    }
    ++p;
  }
  *buf = 0;
  return (sep);
}


static int parse_post_api_parameter_string(xine_post_api_descr_t *descr, void *values, char *param) {
  xine_post_api_parameter_t *p = descr->parameter;
  int changed = 0;

  while (p->type != POST_PARAM_TYPE_LAST) {
    if (!p->readonly) {
      char *arg = strstr(param, p->name);
      if (arg && arg[strlen(p->name)] == '=') {
        arg += strlen(p->name) + 1;
        char *v = (char *)values + p->offset;
        int iv;
        double dv;
        switch (p->type) {
        case POST_PARAM_TYPE_INT:
        case POST_PARAM_TYPE_BOOL:
          iv = atoi(arg);
          if (iv != *((int *)v)) {
            *((int *)v) = iv;
            changed = 1;
          }
          break;
        case POST_PARAM_TYPE_DOUBLE:
          dv = atof(arg);
          if (dv != *((double *)v)) {
            *((double *)v) = dv;
            changed = 1;
          }
          break;
        case POST_PARAM_TYPE_CHAR:
          while (isspace(*arg))
            ++arg;
          char *e = strchr(arg, ',');
          if (!e)
            e = arg + strlen(arg);
          while (e > arg && isspace(e[-1]))
            --e;
          int l = e - arg;
          if (l < (p->size - 1)) {
            if (l != strlen(v) || memcmp(arg, v, l)) {
              memset(v, 0, p->size);
              memcpy(v, arg, l);
              changed = 1;
            }
          }
          break;
        }
      }
    }
    ++p;
  }
  return (changed);
}


static int join_post_api_parameters(xine_post_api_descr_t *descr, void *dst, void *src) {
  xine_post_api_parameter_t *p = descr->parameter;
  int changed = 0;

  while (p->type != POST_PARAM_TYPE_LAST) {
    if (!p->readonly) {
      char *s = (char *)src + p->offset;
      char *d = (char *)dst + p->offset;

      switch (p->type) {
      case POST_PARAM_TYPE_INT:
      case POST_PARAM_TYPE_BOOL:
        if (*((int *)d) != *((int *)s)) {
          *((int *)d) = *((int *)s);
          changed = 1;
        }
        break;
      case POST_PARAM_TYPE_DOUBLE:
        if (*((double *)d) != *((double *)s)) {
          *((double *)d) = *((double *)s);
          changed = 1;
        }
        break;
      case POST_PARAM_TYPE_CHAR:
        if (strncmp(d, s, p->size)) {
          memcpy(d, s, p->size);
          changed = 1;
        }
        break;
      }
    }
    ++p;
  }
  return (changed);
}


static inline void rgb_to_hsv(hsv_color_t *hsv, int r, int g, int b) {
  int min, max, delta;
  int h = 0;

  min = MIN(MIN(r, g), b);
  max = MAX(MAX(r, g), b);

  delta = max - min;

  hsv->v = (uint8_t) POS_DIV(max * v_MAX, 255);

  if (delta == 0) {
    h = 0;
    hsv->s = 0;
  } else {
    hsv->s = (uint8_t) POS_DIV((delta * s_MAX) ,max);

    int dr = (max - r) + 3 * delta;
    int dg = (max - g) + 3 * delta;
    int db = (max - b) + 3 * delta;
    int divisor = 6 * delta;

    if (r == max) {
      h = POS_DIV(( (db - dg) * h_MAX ) , divisor);
    } else if (g == max) {
      h = POS_DIV( ((dr - db) * h_MAX) , divisor) + (h_MAX/3);
    } else if (b == max) {
      h = POS_DIV(( (dg - dr) * h_MAX) , divisor) + (h_MAX/3) * 2;
    }

    if (h < 0) {
      h += h_MAX;
    }
    if (h > h_MAX) {
      h -= h_MAX;
    }
  }
  hsv->h = (uint8_t) h;
}


static void calc_hsv_image(hsv_color_t *hsv, uint8_t *rgb, int img_size) {
  while (img_size--) {
    rgb_to_hsv(hsv, rgb[0], rgb[1], rgb[2]);
    ++hsv;
    rgb += 3;
  }
}


static void calc_weight(atmo_post_plugin_t *this, uint8_t *weight, const int width, const int height, const int edge_weighting) {
  int row, col, c;

  const double w = edge_weighting > 10 ? (double)edge_weighting / 10.0: 10.0;

  const int top_channels = this->active_parm.top;
  const int bottom_channels = this->active_parm.bottom;
  const int left_channels = this->active_parm.left;
  const int right_channels = this->active_parm.right;
  const int center_channel = this->active_parm.center;
  const int top_left_channel = this->active_parm.top_left;
  const int top_right_channel = this->active_parm.top_right;
  const int bottom_left_channel = this->active_parm.bottom_left;
  const int bottom_right_channel = this->active_parm.bottom_right;

  const int sum_top_channels = top_channels + top_left_channel + top_right_channel;
  const int sum_bottom_channels = bottom_channels + bottom_left_channel + bottom_right_channel;
  const int sum_left_channels = left_channels + bottom_left_channel + top_left_channel;
  const int sum_right_channels = right_channels + bottom_right_channel + top_right_channel;

  const int center_y = height / 2;
  const int center_x = width / 2;

  const double fheight = height - 1;
  const double fwidth = width - 1;

  for (row = 0; row < height; ++row)
  {
    double row_norm = (double)row / fheight;
    int top = (int)(255.0 * pow(1.0 - row_norm, w));
    int bottom = (int)(255.0 * pow(row_norm, w));

    for (col = 0; col < width; ++col)
    {
      double col_norm = (double)col / fwidth;
      int left = (int)(255.0 * pow((1.0 - col_norm), w));
      int right = (int)(255.0 * pow(col_norm, w));

      for (c = top_left_channel; c < (top_channels + top_left_channel); ++c)
        *weight++ = (col >= ((width * c) / sum_top_channels) && col < ((width * (c + 1)) / sum_top_channels) && row < center_y) ? top: 0;

      for (c = bottom_left_channel; c < (bottom_channels + bottom_left_channel); ++c)
        *weight++ = (col >= ((width * c) / sum_bottom_channels) && col < ((width * (c + 1)) / sum_bottom_channels) && row >= center_y) ? bottom: 0;

      for (c = top_left_channel; c < (left_channels + top_left_channel); ++c)
        *weight++ = (row >= ((height * c) / sum_left_channels) && row < ((height * (c + 1)) / sum_left_channels) && col < center_x) ? left: 0;

      for (c = top_right_channel; c < (right_channels + top_right_channel); ++c)
        *weight++ = (row >= ((height * c) / sum_right_channels) && row < ((height * (c + 1)) / sum_right_channels) && col >= center_x) ? right: 0;

      if (center_channel)
        *weight++ = 255;

      if (top_left_channel)
        *weight++ = (col < center_x && row < center_y) ? ((top > left) ? top: left) : 0;

      if (top_right_channel)
        *weight++ = (col >= center_x && row < center_y) ? ((top > right) ? top: right): 0;

      if (bottom_left_channel)
        *weight++ = (col < center_x && row >= center_y) ? ((bottom > left) ? bottom: left): 0;

      if (bottom_right_channel)
        *weight++ = (col >= center_x && row >= center_y) ? ((bottom > right) ? bottom: right): 0;
    }
  }
}


static void calc_hue_hist(atmo_post_plugin_t *this, hsv_color_t *hsv, uint8_t *weight, int img_size) {
  const int n = this->sum_channels;
  uint64_t * const hue_hist = this->hue_hist;
  const int darkness_limit = this->active_parm.darkness_limit;

  memset(hue_hist, 0, (n * (h_MAX+1) * sizeof(uint64_t)));

  while (img_size--) {
    if (hsv->v >= darkness_limit) {
      int c;
      for (c = 0; c < n; ++c)
        hue_hist[c * (h_MAX+1) + hsv->h] += weight[c] * hsv->v;
    }
    weight += n;
    ++hsv;
  }
}


static void calc_windowed_hue_hist(atmo_post_plugin_t *this) {
  int i, c, w;
  const int n = this->sum_channels;
  uint64_t * const hue_hist = this->hue_hist;
  uint64_t * const w_hue_hist = this->w_hue_hist;
  const int hue_win_size = this->active_parm.hue_win_size;

  memset(w_hue_hist, 0, (n * (h_MAX+1) * sizeof(uint64_t)));

  for (i = 0; i < (h_MAX+1); ++i)
  {
    for (w = -hue_win_size; w <= hue_win_size; w++)
    {
      int iw = i + w;

      if (iw < 0)
        iw = iw + h_MAX + 1;
      if (iw > h_MAX)
        iw = iw - h_MAX - 1;

      uint64_t win_weight = (hue_win_size + 1) - abs(w);

      for (c = 0; c < n; ++c)
        w_hue_hist[c * (h_MAX+1) + i] += hue_hist[c * (h_MAX+1) + iw] * win_weight;
    }
  }
}


static void calc_most_used_hue(atmo_post_plugin_t *this) {
  int i, c;

  const int n = this->sum_channels;
  uint64_t * const w_hue_hist = this->w_hue_hist;
  int * const most_used_hue = this->most_used_hue;
  int * const last_most_used_hue = this->last_most_used_hue;
  const double hue_threshold = (double)this->active_parm.hue_threshold / 100.0;

  memset(most_used_hue, 0, (n * sizeof(int)));

  for (c = 0; c < n; ++c) {
    uint64_t v = 0;
    for (i = 0; i < (h_MAX + 1); ++i) {
      if (w_hue_hist[c * (h_MAX+1) + i] > v) {
        v = w_hue_hist[c * (h_MAX+1) + i];
        most_used_hue[c] = i;
      }
    }
    if (((double) w_hue_hist[c * (h_MAX+1) + last_most_used_hue[c]] / (double) v) > hue_threshold)
      most_used_hue[c] = last_most_used_hue[c];
    else
      last_most_used_hue[c] = most_used_hue[c];
  }
}


static void calc_sat_hist(atmo_post_plugin_t *this, hsv_color_t *hsv, uint8_t *weight, int img_size) {
  const int n = this->sum_channels;
  uint64_t * const sat_hist = this->sat_hist;
  int * const most_used_hue = this->most_used_hue;
  const int darkness_limit = this->active_parm.darkness_limit;
  const int hue_win_size = this->active_parm.hue_win_size;

  memset(sat_hist, 0, (n * (s_MAX+1) * sizeof(uint64_t)));

  while (img_size--) {
    if (hsv->v >= darkness_limit) {
      int h = hsv->h;
      int c;
      for (c = 0; c < n; ++c) {
        if (h > (most_used_hue[c] - hue_win_size) && h < (most_used_hue[c] + hue_win_size))
          sat_hist[c * (s_MAX+1) + hsv->s] += weight[c] * hsv->v;
      }
    }
    weight += n;
    ++hsv;
  }
}


static void calc_windowed_sat_hist(atmo_post_plugin_t *this) {
  int i, c, w;
  const int n = this->sum_channels;
  uint64_t * const sat_hist = this->sat_hist;
  uint64_t * const w_sat_hist = this->w_sat_hist;
  const int sat_win_size = this->active_parm.sat_win_size;

  memset(w_sat_hist, 0, (n * (s_MAX+1) * sizeof(uint64_t)));

  for (i = 0; i < (s_MAX+1); ++i)
  {
    for (w = -sat_win_size; w <= sat_win_size; w++)
    {
      int iw = i + w;

      if (iw < 0)
        iw = iw + s_MAX + 1;
      if (iw > s_MAX)
        iw = iw - s_MAX - 1;

      uint64_t win_weight = (sat_win_size + 1) - abs(w);

      for (c = 0; c < n; ++c)
        w_sat_hist[c * (s_MAX+1) + i] += sat_hist[c * (s_MAX+1) + iw] * win_weight;
    }
  }
}


static void calc_most_used_sat(atmo_post_plugin_t *this) {
  int i, c;
  const int n = this->sum_channels;
  uint64_t * const w_sat_hist = this->w_sat_hist;
  int * const most_used_sat = this->most_used_sat;

  memset(most_used_sat, 0, (n * sizeof(int)));

  for (c = 0; c < n; ++c) {
    uint64_t v = 0;
    for (i = 0; i < (s_MAX + 1); ++i) {
      if (w_sat_hist[c * (s_MAX+1) + i] > v) {
        v = w_sat_hist[c * (s_MAX+1) + i];
        most_used_sat[c] = i;
      }
    }
  }
}


static void calc_average_brightness(atmo_post_plugin_t *this, hsv_color_t *hsv, uint8_t *weight, int img_size) {
  int c;
  const int n = this->sum_channels;
  const int darkness_limit = this->active_parm.darkness_limit;
  const uint64_t bright = this->active_parm.brightness;
  uint64_t * const avg_bright = this->avg_bright;
  int * const avg_cnt = this->avg_cnt;

  memset(avg_bright, 0, (n * sizeof(uint64_t)));
  memset(avg_cnt, 0, (n * sizeof(int)));

  while (img_size--) {
    const int v = hsv->v;
    if (v >= darkness_limit) {
      for (c = 0; c < n; ++c) {
        avg_bright[c] += v * weight[c];
        avg_cnt[c] += weight[c];
      }
    }
    weight += n;
    ++hsv;
  }

  for (c = 0; c < n; ++c) {
    if (avg_cnt[c]) {
      avg_bright[c] = (avg_bright[c] * bright) / (avg_cnt[c] * ((uint64_t)100));
      if (avg_bright[c] > v_MAX)
        avg_bright[c] = v_MAX;
    }
  }
}


static void hsv_to_rgb(rgb_color_t *rgb, double h, double s, double v) {
  rgb->r = rgb->g = rgb->b = 0;

  h /= h_MAX;
  s /= s_MAX;
  v /= v_MAX;

  if (s == 0.0) {
    rgb->r = (uint8_t) (v * 255.0 + 0.5);
    rgb->g = rgb->r;
    rgb->b = rgb->r;
  } else {
    h = h * 6.0;
    if (h == 6.0)
      h = 0.0;
    int i = (int) h;

    double f = h - i;
    double p = v * (1.0 - s);
    double q = v * (1.0 - (s * f));
    double t = v * (1.0 - (s * (1.0 - f)));

    if (i == 0) {
      rgb->r = (uint8_t) (v * 255.0 + 0.5);
      rgb->g = (uint8_t) (t * 255.0 + 0.5);
      rgb->b = (uint8_t) (p * 255.0 + 0.5);
    } else if (i == 1) {
      rgb->r = (uint8_t) (q * 255.0 + 0.5);
      rgb->g = (uint8_t) (v * 255.0 + 0.5);
      rgb->b = (uint8_t) (p * 255.0 + 0.5);
    } else if (i == 2) {
      rgb->r = (uint8_t) (p * 255.0 + 0.5);
      rgb->g = (uint8_t) (v * 255.0 + 0.5);
      rgb->b = (uint8_t) (t * 255.0 + 0.5);
    } else if (i == 3) {
      rgb->r = (uint8_t) (p * 255.0 + 0.5);
      rgb->g = (uint8_t) (q * 255.0 + 0.5);
      rgb->b = (uint8_t) (v * 255.0 + 0.5);
    } else if (i == 4) {
      rgb->r = (uint8_t) (t * 255.0 + 0.5);
      rgb->g = (uint8_t) (p * 255.0 + 0.5);
      rgb->b = (uint8_t) (v * 255.0 + 0.5);
    } else {
      rgb->r = (uint8_t) (v * 255.0 + 0.5);
      rgb->g = (uint8_t) (p * 255.0 + 0.5);
      rgb->b = (uint8_t) (q * 255.0 + 0.5);
    }
  }
}


static void calc_rgb_values(atmo_post_plugin_t *this)
{
  int c;
  const int n = this->sum_channels;

  for (c = 0; c < n; ++c)
    hsv_to_rgb(&this->analyzed_colors[c], this->most_used_hue[c], this->most_used_sat[c], this->avg_bright[c]);
}


static void *atmo_grab_loop (void *this_gen) {
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) this_gen;
  post_video_port_t *port = this->port;
  xine_video_port_t *video_port = port->original_port;
  xine_ticket_t *ticket = this->post_plugin.running_ticket;
  xine_grab_frame_t *frame = NULL;
  int rc;
  int grab_width, grab_height, analyze_width, analyze_height, overscan, img_size;
  int last_analyze_width = 0, last_analyze_height = 0;
  int alloc_img_size = 0;
  int edge_weighting = 0;
  int running = 1;
  hsv_color_t *hsv_img = NULL;
  uint8_t *weight = NULL;
  struct timeval tvnow, tvlast, tvdiff;
  useconds_t analyze_rate;

  _x_post_inc_usage(port);

  pthread_mutex_lock(&this->lock);
  this->grab_running = &running;
  pthread_mutex_unlock(&this->lock);
  pthread_cond_broadcast(&this->thread_started);

  llprintf(LOG_1, "grab thread running\n");

  ticket->acquire(ticket, 0);

  while (running) {

      /* allocate grab frame */
    if (!frame && xine_port_send_gui_data(video_port, XINE_GUI_SEND_ALLOC_GRAB_FRAME, &frame)) {
      xine_log(this->post_plugin.xine, XINE_LOG_PLUGIN, "atmo: frame grabbing not supported by this video driver!\n");
      break;
    }

    if (ticket->ticket_revoked) {
        /* free grab frame */
      if (frame) {
        xine_port_send_gui_data(video_port, XINE_GUI_SEND_FREE_GRAB_FRAME, frame);
        frame = NULL;
      }
      llprintf(LOG_1, "grab thread waiting for new ticket\n");
      ticket->renew(ticket, 0);
      llprintf(LOG_1, "grab thread got new ticket\n");
      continue;
    }

    gettimeofday(&tvlast, NULL);

      /* get actual displayed image size */
    grab_width = video_port->get_property(video_port, VO_PROP_WINDOW_WIDTH);
    grab_height = video_port->get_property(video_port, VO_PROP_WINDOW_HEIGHT);
    if (grab_width > 0 && grab_height > 0) {

        /* calculate size of analyze image */
      analyze_width = (this->active_parm.analyze_size + 1) * 64;
      analyze_height = (analyze_width * grab_height) / grab_width;

        /* calculate size of grab (sub) window */
      overscan = this->active_parm.overscan;
      if (overscan) {
        frame->crop_left = frame->crop_right = grab_width * overscan / 1000;
        frame->crop_top = frame->crop_bottom = grab_height * overscan / 1000;
        grab_width = grab_width - frame->crop_left - frame->crop_right;
        grab_height = grab_height - frame->crop_top - frame->crop_bottom;
      } else {
        frame->crop_bottom = 0;
        frame->crop_top = 0;
        frame->crop_left =  0;
        frame->crop_right = 0;
      }

        /* grab displayed video frame */
      frame->timeout = GRAB_TIMEOUT;
      frame->width = analyze_width;
      frame->height = analyze_height;
      if (!(rc = xine_port_send_gui_data(video_port, XINE_GUI_SEND_GRAB_FRAME, frame))) {
        if (frame->width == analyze_width && frame->height == analyze_height) {
          img_size = analyze_width * analyze_height;

            /* allocate hsv and weight images */
          if (img_size > alloc_img_size) {
            free(hsv_img);
            free(weight);
            alloc_img_size = img_size;
            hsv_img = (hsv_color_t *) malloc(img_size * sizeof(hsv_color_t));
            weight = (uint8_t *) malloc(img_size * this->sum_channels * sizeof(uint8_t));
            if (!hsv_img || !weight)
              break;
            last_analyze_width = 0;
            last_analyze_height = 0;
            edge_weighting = 0;
          }

            /* calculate weight image */
          if (analyze_width != last_analyze_width || analyze_height != last_analyze_height || edge_weighting != this->active_parm.edge_weighting) {
            edge_weighting = this->active_parm.edge_weighting;
            last_analyze_width = analyze_width;
            last_analyze_height = analyze_height;
            calc_weight(this, weight, analyze_width, analyze_height, edge_weighting);
            llprintf(LOG_1, "analyze size %dx%d, grab %dx%d@%d,%d\n", analyze_width, analyze_height, grab_width, grab_height, frame->crop_left, frame->crop_top);
          }

            /* analyze grabbed image */
          calc_hsv_image(hsv_img, frame->img, img_size);
          calc_hue_hist(this, hsv_img, weight, img_size);
          calc_windowed_hue_hist(this);
          calc_most_used_hue(this);
          calc_sat_hist(this, hsv_img, weight, img_size);
          calc_windowed_sat_hist(this);
          calc_most_used_sat(this);
          calc_average_brightness(this, hsv_img, weight, img_size);
          pthread_mutex_lock(&this->lock);
          calc_rgb_values(this);
          pthread_mutex_unlock(&this->lock);

          llprintf(LOG_2, "grab %ld.%03ld: vpts=%ld\n", tvlast.tv_sec, tvlast.tv_usec / 1000, frame->vpts);
        }
      } else {
        if (rc < 0)
          llprintf(LOG_1, "grab failed!\n");
        if (rc > 0)
          llprintf(LOG_2, "grab timed out!\n");
      }
    }

      /* loop with analyze rate duration */
    analyze_rate = this->active_parm.analyze_rate * 1000;
    gettimeofday(&tvnow, NULL);
    timersub(&tvnow, &tvlast, &tvdiff);
    if (tvdiff.tv_sec == 0 && tvdiff.tv_usec < analyze_rate)
      usleep(analyze_rate - tvdiff.tv_usec);
  }

    /* free grab frame */
  if (frame)
    xine_port_send_gui_data(video_port, XINE_GUI_SEND_FREE_GRAB_FRAME, frame);

  ticket->release(ticket, 0);

  free(hsv_img);
  free(weight);

  pthread_mutex_lock(&this->lock);
  if (running)
    this->grab_running = NULL;
  pthread_mutex_unlock(&this->lock);

  _x_post_dec_usage(port);

  llprintf(LOG_1, "grab thread terminated\n");
  return NULL;
}


static void reset_filters(atmo_post_plugin_t *this) {
  this->old_mean_length = 0;
}


static void percent_filter(atmo_post_plugin_t *this) {
  rgb_color_t *act = this->analyzed_colors;
  rgb_color_t *out = this->filtered_colors;
  const int old_p = this->active_parm.filter_smoothness;
  const int new_p = 100 - old_p;
  int n = this->sum_channels;

  while (n--) {
    out->r = (act->r * new_p + out->r * old_p) / 100;
    out->g = (act->g * new_p + out->g * old_p) / 100;
    out->b = (act->b * new_p + out->b * old_p) / 100;
    ++act;
    ++out;
  }
}


static void mean_filter(atmo_post_plugin_t *this) {
  rgb_color_t *act = this->analyzed_colors;
  rgb_color_t *out = this->filtered_colors;
  rgb_color_t *mean_values = this->mean_filter_values;
  rgb_color_sum_t *mean_sums = this->mean_filter_sum_values;
  const int64_t mean_threshold = (int64_t) ((double) this->active_parm.filter_threshold * 3.6);
  const int old_p = this->active_parm.filter_smoothness;
  const int new_p = 100 - old_p;
  int n = this->sum_channels;
  const int filter_length = this->active_parm.filter_length;
  const int64_t mean_length = (filter_length < OUTPUT_RATE) ? 1: filter_length / OUTPUT_RATE;
  const int reinitialize = ((int)mean_length != this->old_mean_length);
  this->old_mean_length = (int)mean_length;

  while (n--) {
    mean_sums->r += (act->r - mean_values->r);
    mean_values->r = (uint8_t) (mean_sums->r / mean_length);

    mean_sums->g += (act->g - mean_values->g);
    mean_values->g = (uint8_t) (mean_sums->g / mean_length);

    mean_sums->b += (act->b - mean_values->b);
    mean_values->b = (uint8_t) (mean_sums->b / mean_length);

      /*
       * check, if there is a jump -> check if differences between actual values and filter values are too big
       */
    int64_t dist = (int64_t)(mean_values->r - act->r) * (int64_t)(mean_values->r - act->r) +
                    (int64_t)(mean_values->g - act->g) * (int64_t)(mean_values->g - act->g) +
                    (int64_t)(mean_values->b - act->b) * (int64_t)(mean_values->b - act->b);

    if (dist > 0)
      dist = (int64_t) sqrt((double) dist);

      /* compare calculated distance with the filter threshold */
    if (dist > mean_threshold || reinitialize) {
        /* filter jump detected -> set the long filters to the result of the short filters */
      *out = *act;
      *mean_values = *act;
      mean_sums->r = act->r * mean_length;
      mean_sums->g = act->g * mean_length;
      mean_sums->b = act->b * mean_length;
    }
    else
    {
        /* apply an additional percent filter */
      out->r = (mean_values->r * new_p + out->r * old_p) / 100;
      out->g = (mean_values->g * new_p + out->g * old_p) / 100;
      out->b = (mean_values->b * new_p + out->b * old_p) / 100;
    }

    ++act;
    ++out;
    ++mean_sums;
    ++mean_values;
  }
}


static void apply_white_calibration(atmo_post_plugin_t *this) {
  const int wc_red = this->active_parm.wc_red;
  const int wc_green = this->active_parm.wc_green;
  const int wc_blue = this->active_parm.wc_blue;
  if (wc_red == 255 && wc_green == 255 && wc_blue == 255)
    return;

  rgb_color_t *out = this->output_colors;
  int n = this->sum_channels;
  while (n--) {
    out->r = (uint8_t)((int)out->r * wc_red / 255);
    out->g = (uint8_t)((int)out->g * wc_green / 255);
    out->b = (uint8_t)((int)out->b * wc_blue / 255);
    ++out;
  }
}


static void apply_gamma_correction(atmo_post_plugin_t *this) {
  const int igamma = this->active_parm.gamma;
  if (igamma <= 10)
    return;

  const double gamma = (double)igamma / 10.0;
  rgb_color_t *out = this->output_colors;
  int n = this->sum_channels;
  while (n--) {
    out->r = (uint8_t)(pow((double)out->r / 255.0, gamma) * 255.0);
    out->g = (uint8_t)(pow((double)out->g / 255.0, gamma) * 255.0);
    out->b = (uint8_t)(pow((double)out->b / 255.0, gamma) * 255.0);
    ++out;
  }
}


static void *atmo_output_loop (void *this_gen) {
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) this_gen;
  post_video_port_t *port = this->port;
  xine_ticket_t *ticket = this->post_plugin.running_ticket;
  output_driver_t *output_driver = this->output_driver;
  int colors_size = this->sum_channels * sizeof(rgb_color_t);
  int running = 1;
  struct timeval tvnow, tvlast, tvdiff, tvfirst;

  _x_post_inc_usage(port);

  pthread_mutex_lock(&this->lock);
  this->output_running = &running;
  pthread_mutex_unlock(&this->lock);
  pthread_cond_broadcast(&this->thread_started);

  llprintf(LOG_1, "output thread running\n");

  ticket->acquire(ticket, 0);
  reset_filters(this);

  gettimeofday(&tvfirst, NULL);

  while (running) {

    if (ticket->ticket_revoked) {
      reset_filters(this);
      llprintf(LOG_1, "output thread waiting for new ticket\n");
      ticket->renew(ticket, 0);
      llprintf(LOG_1, "output thread got new ticket\n");
      continue;
    }

    gettimeofday(&tvlast, NULL);

      /* Transfer analyzed colors into filtered colors */
    pthread_mutex_lock(&this->lock);
    switch (this->active_parm.filter) {
    case 1:
      percent_filter(this);
      break;
    case 2:
      mean_filter(this);
      break;
    default:
        /* no filtering */
      memcpy(this->filtered_colors, this->analyzed_colors, colors_size);
    }
    pthread_mutex_unlock(&this->lock);

      /* Transfer filtered colors to output colors */
    memcpy(this->output_colors, this->filtered_colors, colors_size);
    apply_gamma_correction(this);
    apply_white_calibration(this);

      /* Output colors */
    gettimeofday(&tvnow, NULL);
    timersub(&tvnow, &tvfirst, &tvdiff);
    if ((tvdiff.tv_sec * 1000 + tvdiff.tv_usec / 1000) >= this->active_parm.start_delay) {
        if (memcmp(this->output_colors, this->last_output_colors, colors_size)) {
          output_driver->output_colors(output_driver, this->output_colors, this->last_output_colors);
          memcpy(this->last_output_colors, this->output_colors, colors_size);
        }
    }

      /* Loop with output rate duration */
    const useconds_t output_rate = OUTPUT_RATE * 1000;
    gettimeofday(&tvnow, NULL);
    timersub(&tvnow, &tvlast, &tvdiff);
    if (tvdiff.tv_sec == 0 && tvdiff.tv_usec < output_rate)
      usleep(output_rate - tvdiff.tv_usec);
  }

    /* Turn off Light */
  memset(this->output_colors, 0, colors_size);
  if (memcmp(this->output_colors, this->last_output_colors, colors_size)) {
    output_driver->output_colors(output_driver, this->output_colors, this->last_output_colors);
    memset(this->last_output_colors, 0, colors_size);
  }

  ticket->release(ticket, 0);

  pthread_mutex_lock(&this->lock);
  if (running)
    this->output_running = NULL;
  pthread_mutex_unlock(&this->lock);

  _x_post_dec_usage(port);

  llprintf(LOG_1, "output thread terminated\n");

  return NULL;
}


static void config_channels(atmo_post_plugin_t *this) {
  int n = this->parm.top + this->parm.bottom + this->parm.left + this->parm.right +
          this->parm.center +
          this->parm.top_left + this->parm.top_right + this->parm.bottom_left + this->parm.bottom_right;
  this->sum_channels = n;

  if (n)
  {
    this->hue_hist = (uint64_t *) calloc(n * (h_MAX + 1), sizeof(uint64_t));
    this->w_hue_hist = (uint64_t *) calloc(n * (h_MAX + 1), sizeof(uint64_t));
    this->most_used_hue = (int *) calloc(n, sizeof(int));
    this->last_most_used_hue = (int *) calloc(n, sizeof(int));

    this->sat_hist = (uint64_t *) calloc(n * (s_MAX + 1), sizeof(uint64_t));
    this->w_sat_hist = (uint64_t *) calloc(n * (s_MAX + 1), sizeof(uint64_t));
    this->most_used_sat = (int *) calloc(n, sizeof(int));

    this->avg_cnt = (int *) calloc(n, sizeof(int));
    this->avg_bright = (uint64_t *) calloc(n, sizeof(uint64_t));

    this->analyzed_colors = (rgb_color_t *) calloc(n, sizeof(rgb_color_t));
    this->filtered_colors = (rgb_color_t *) calloc(n, sizeof(rgb_color_t));
    this->output_colors = (rgb_color_t *) calloc(n, sizeof(rgb_color_t));
    this->last_output_colors = (rgb_color_t *) calloc(n, sizeof(rgb_color_t));
    this->mean_filter_values = (rgb_color_t *) calloc(n, sizeof(rgb_color_t));
    this->mean_filter_sum_values = (rgb_color_sum_t *) calloc(n, sizeof(rgb_color_sum_t));
  }

  llprintf(LOG_1, "configure channels top %d, bottom %d, left %d, right %d, center %d, topLeft %d, topRight %d, bottomLeft %d, bottomRight %d\n",
                  this->parm.top, this->parm.bottom, this->parm.left, this->parm.right, this->parm.center,
                  this->parm.top_left, this->parm.top_right, this->parm.bottom_left, this->parm.bottom_right);
}


static void free_channels(atmo_post_plugin_t *this) {
  if (this->sum_channels)
  {
    free(this->hue_hist);
    free(this->w_hue_hist);
    free(this->most_used_hue);
    free(this->last_most_used_hue);

    free(this->sat_hist);
    free(this->w_sat_hist);
    free(this->most_used_sat);

    free(this->avg_bright);
    free(this->avg_cnt);

    free(this->analyzed_colors);
    free(this->filtered_colors);
    free(this->output_colors);
    free(this->last_output_colors);
    free(this->mean_filter_values);
    free(this->mean_filter_sum_values);
  }
}


static void start_threads(atmo_post_plugin_t *this) {
  int err;
  pthread_attr_t pth_attrs;

  pthread_attr_init(&pth_attrs);
  pthread_attr_setscope(&pth_attrs, PTHREAD_SCOPE_SYSTEM);
  pthread_attr_setdetachstate(&pth_attrs, PTHREAD_CREATE_DETACHED);

  pthread_mutex_lock(&this->lock);
  if (this->grab_running == NULL) {
    if ((err = pthread_create (&this->grab_thread, &pth_attrs, atmo_grab_loop, this)))
      xprintf(this->post_plugin.xine, XINE_VERBOSITY_LOG, "atmo: can't create grab thread (%s)\n", strerror(err));
    else
      pthread_cond_wait(&this->thread_started, &this->lock);
  }

  if (this->output_running == NULL) {
    if ((err = pthread_create (&this->output_thread, &pth_attrs, atmo_output_loop, this)))
      xprintf(this->post_plugin.xine, XINE_VERBOSITY_LOG, "atmo: can't create output thread (%s)\n", strerror(err));
    else
      pthread_cond_wait(&this->thread_started, &this->lock);
  }
  pthread_mutex_unlock(&this->lock);

  pthread_attr_destroy(&pth_attrs);
}


static void stop_threads(atmo_post_plugin_t *this, int wait) {
  int do_wait = 0;

  pthread_mutex_lock(&this->lock);
  if (this->grab_running) {
    *this->grab_running = 0;
    this->grab_running = NULL;
    do_wait = wait;
  }
  if (this->output_running) {
    *this->output_running = 0;
    this->output_running = NULL;
    do_wait = wait;
  }
  pthread_mutex_unlock(&this->lock);
  if (do_wait)
    usleep(THREAD_TERMINATION_WAIT_TIME*1000);
}


static void close_output_driver(atmo_post_plugin_t *this) {

  if (this->driver_opened) {
      /* Switch all channels off */
    int colors_size = this->sum_channels * sizeof(rgb_color_t);
    memset(this->output_colors, 0, colors_size);
    if (memcmp(this->output_colors, this->last_output_colors, colors_size))
      this->output_driver->output_colors(this->output_driver, this->output_colors, this->last_output_colors);

    if (this->output_driver->close(this->output_driver))
      xine_log(this->post_plugin.xine, XINE_LOG_PLUGIN, "atmo: output driver: %s!\n", this->output_driver->errmsg);
    this->driver_opened = 0;
    llprintf(LOG_1, "output driver closed\n");
  }
}


static void open_output_driver(atmo_post_plugin_t *this) {

  if (!this->parm.enabled || this->active_parm.driver != this->parm.driver || strcmp(this->active_parm.driver_param, this->parm.driver_param))
    close_output_driver(this);

  if (this->parm.enabled) {
    int start = 1;
    atmo_parameters_t parm = this->parm;

      /* open output driver */
    if (!this->driver_opened) {
      if ((this->output_driver = get_output_driver(&this->output_drivers, this->parm.driver)) == NULL) {
        xine_log(this->post_plugin.xine, XINE_LOG_PLUGIN, "atmo: no valid output driver selected!\n");
        start = 0;
      } else if (this->output_driver->open(this->output_driver, &this->parm)) {
        xine_log(this->post_plugin.xine, XINE_LOG_PLUGIN, "atmo: can't open output driver: %s!\n", this->output_driver->errmsg);
        start = 0;
      } else {
        this->driver_opened = 1;
        llprintf(LOG_1, "output driver opened\n");
      }
    } else {
      if (this->output_driver->configure(this->output_driver, &this->parm)) {
        xine_log(this->post_plugin.xine, XINE_LOG_PLUGIN, "atmo: can't configure output driver: %s!\n", this->output_driver->errmsg);
        start = 0;
      }
    }

    if (join_post_api_parameters(&atmo_param_descr, &parm, &this->parm)) {
      char buf[512];
      build_post_api_parameter_string(buf, sizeof(buf), &atmo_param_descr, &this->parm, &this->default_parm);
      this->post_plugin.xine->config->update_string(this->post_plugin.xine->config, "post.atmo.parameters", buf);
    }

    if (this->active_parm.top != this->parm.top ||
                    this->active_parm.bottom != this->parm.bottom ||
                    this->active_parm.left != this->parm.left ||
                    this->active_parm.right != this->parm.right ||
                    this->active_parm.center != this->parm.center ||
                    this->active_parm.top_left != this->parm.top_left ||
                    this->active_parm.top_right != this->parm.top_right ||
                    this->active_parm.bottom_left != this->parm.bottom_left ||
                    this->active_parm.bottom_right != this->parm.bottom_right) {
      free_channels(this);
      config_channels(this);
    }

    this->active_parm = this->parm;

    if (!this->sum_channels)
      start = 0;

    if (start) {
        /* send first initial color packet */
      this->output_driver->output_colors(this->output_driver, this->output_colors, NULL);

      start_threads(this);
    }
  }
}


/*
 * Open/Close video port
 */

static void atmo_video_open(xine_video_port_t *port_gen, xine_stream_t *stream) {
  post_video_port_t *port = (post_video_port_t *)port_gen;
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) port->post;

  _x_post_rewire(port->post);
  _x_post_inc_usage(port);

  pthread_mutex_lock(&this->port_lock);
  (port->original_port->open) (port->original_port, stream);
  port->stream = stream;
  this->port = port;
  open_output_driver(this);
  pthread_mutex_unlock(&this->port_lock);
}


static void atmo_video_close(xine_video_port_t *port_gen, xine_stream_t *stream) {
  post_video_port_t *port = (post_video_port_t *)port_gen;
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) port->post;

  pthread_mutex_lock(&this->port_lock);
  stop_threads(this, 0);
  this->port = NULL;
  port->original_port->close(port->original_port, stream);
  port->stream = NULL;
  pthread_mutex_unlock(&this->port_lock);

  _x_post_dec_usage(port);
}


/*
 *    Parameter functions
 */

static xine_post_api_descr_t *atmo_get_param_descr(void)
{
  return &atmo_param_descr;
}


static int atmo_set_parameters(xine_post_t *this_gen, void *parm_gen)
{
  atmo_post_plugin_t *this = (atmo_post_plugin_t *)this_gen;

  if (join_post_api_parameters(&atmo_param_descr, &this->parm, parm_gen)) {
    char buf[512];
    build_post_api_parameter_string(buf, sizeof(buf), &atmo_param_descr, &this->parm, &this->default_parm);
    this->post_plugin.xine->config->update_string(this->post_plugin.xine->config, "post.atmo.parameters", buf);
    llprintf(LOG_1, "set parameters\n");

    pthread_mutex_lock(&this->port_lock);
    if (this->port) {
      if (this->parm.enabled) {
        if (!this->active_parm.enabled)
          open_output_driver(this);
        else {
          this->active_parm.analyze_rate = this->parm.analyze_rate;
          this->active_parm.brightness = this->parm.brightness;
          this->active_parm.darkness_limit = this->parm.darkness_limit;
          this->active_parm.filter = this->parm.filter;
          this->active_parm.filter_length = this->parm.filter_length;
          this->active_parm.filter_smoothness = this->parm.filter_smoothness;
          this->active_parm.filter_threshold = this->parm.filter_threshold;
          this->active_parm.gamma = this->parm.gamma;
          this->active_parm.hue_win_size = this->parm.hue_win_size;
          this->active_parm.sat_win_size = this->parm.sat_win_size;
          this->active_parm.hue_threshold = this->parm.hue_threshold;
          this->active_parm.start_delay = this->parm.start_delay;
          this->active_parm.wc_blue = this->parm.wc_blue;
          this->active_parm.wc_green = this->parm.wc_green;
          this->active_parm.wc_red = this->parm.wc_red;
        }
      } else {
        if (this->active_parm.enabled) {
          stop_threads(this, 1);
          close_output_driver(this);
        }
      }
      this->active_parm.enabled = this->parm.enabled;
    }
    pthread_mutex_unlock(&this->port_lock);
  }

  return 1;
}


static int atmo_get_parameters(xine_post_t *this_gen, void *parm_gen)
{
  atmo_post_plugin_t *this = (atmo_post_plugin_t *)this_gen;
  atmo_parameters_t *parm = (atmo_parameters_t *)parm_gen;

  *parm  = this->parm;
  return 1;
}


static char *atmo_get_help(void) {
  return _("The xine atmolight post plugin\n"
           "Analyze video picture and generate output data for atmolight controllers\n"
           "\n"
         );
}


/*
 *    open/close plugin
 */

static void atmo_dispose(post_plugin_t *this_gen)
{
  if (_x_post_dispose(this_gen)) {
    atmo_post_plugin_t *this = (atmo_post_plugin_t *) this_gen;

    close_output_driver(this);
    free_channels(this);
    pthread_mutex_destroy(&this->lock);
    pthread_mutex_destroy(&this->port_lock);
    pthread_cond_destroy(&this->thread_started);
    free(this);
  }
}


static post_plugin_t *atmo_open_plugin(post_class_t *class_gen,
					    int inputs,
					    xine_audio_port_t **audio_target,
					    xine_video_port_t **video_target)
{
  atmo_post_class_t *class = (atmo_post_class_t *) class_gen;
  post_in_t *input;
  post_out_t *output;
  post_video_port_t *port;
  xine_post_in_t *input_param;
  static xine_post_api_t post_api =
      { atmo_set_parameters, atmo_get_parameters,
        atmo_get_param_descr, atmo_get_help };

  if (!video_target || !video_target[0])
    return NULL;

  atmo_post_plugin_t *this = (atmo_post_plugin_t *) calloc(1, sizeof(atmo_post_plugin_t));
  if (!this)
    return NULL;

  _x_post_init(&this->post_plugin, 0, 1);
  this->post_plugin.xine = class->xine;

  port = _x_post_intercept_video_port(&this->post_plugin, video_target[0], &input, &output);

  input->xine_in.name   = "video in";
  output->xine_out.name = "video out";

  this->post_plugin.dispose = atmo_dispose;

  port->new_port.open = atmo_video_open;
  port->new_port.close = atmo_video_close;
  //port->port_lock = &this->port_lock;

  this->post_plugin.xine_post.video_input[0] = &port->new_port;

  input_param       = &this->parameter_input;
  input_param->name = "parameters";
  input_param->type = XINE_POST_DATA_PARAMETERS;
  input_param->data = &post_api;
  xine_list_push_back(this->post_plugin.input, input_param);

  pthread_mutex_init(&this->lock, NULL);
  pthread_mutex_init(&this->port_lock, NULL);
  pthread_cond_init(&this->thread_started, NULL);

    /* Set default values for parameters */
  this->parm.enabled = 1;
  this->parm.overscan = 30;
  this->parm.analyze_rate = 40;
  this->parm.analyze_size = 1;
  this->parm.brightness = 100;
  this->parm.darkness_limit = 1;
  this->parm.edge_weighting = 80;
  this->parm.filter = 2;
  this->parm.filter_length = 500;
  this->parm.filter_smoothness = 50;
  this->parm.filter_threshold = 40;
  this->parm.hue_win_size = 3;
  this->parm.sat_win_size = 3;
  this->parm.hue_threshold = 93;
  this->parm.wc_red = 255;
  this->parm.wc_green = 255;
  this->parm.wc_blue = 255;
  this->parm.gamma = 0;
  this->parm.start_delay = 250;
  this->default_parm = this->parm;

    /* Read parameters from xine configuration file */
  config_values_t *config = this->post_plugin.xine->config;
  char *param = config->register_string (config, "post.atmo.parameters", "",
                                                  "Parameters of atmo post plugin",
                                                  NULL, 20, NULL, NULL);
  if (param)
    parse_post_api_parameter_string(&atmo_param_descr, &this->parm, param);

  char buf[512];
  build_post_api_parameter_string(buf, sizeof(buf), &atmo_param_descr, &this->parm, &this->default_parm);
  if (!param || strcmp(param, buf))
    config->update_string(config, "post.atmo.parameters", buf);

  return &this->post_plugin;
}


/*
 *    Plugin class
 */

#if POST_PLUGIN_IFACE_VERSION < 10
static char *atmo_get_identifier(post_class_t *class_gen)
{
  return "atmo";
}

static char *atmo_get_description(post_class_t *class_gen)
{
  return "Analyze video picture and generate output data for atmolight controllers";
}

static void atmo_class_dispose(post_class_t *class_gen)
{
  free(class_gen);
}
#endif

static void *atmo_init_plugin(xine_t *xine, void *data)
{
  atmo_post_class_t *class = (atmo_post_class_t *) calloc(1, sizeof(atmo_post_class_t));

  if(class) {
    class->xine = xine;
    class->post_class.open_plugin     = atmo_open_plugin;
#if POST_PLUGIN_IFACE_VERSION < 10
    class->post_class.get_identifier  = atmo_get_identifier;
    class->post_class.get_description = atmo_get_description;
    class->post_class.dispose         = atmo_class_dispose;
#else
    class->post_class.identifier      = "atmo";
    class->post_class.description     = N_("Analyze video picture and generate output data for atmolight controllers");
    class->post_class.dispose         = default_post_class_dispose;
#endif
  }
  return &class->post_class;
}


static post_info_t info = { XINE_POST_TYPE_VIDEO_FILTER };

const plugin_info_t xine_plugin_info[] __attribute__((visibility("default"))) =
{
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_POST, POST_PLUGIN_IFACE_VERSION, "atmo", XINE_VERSION_CODE, &info, &atmo_init_plugin },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
