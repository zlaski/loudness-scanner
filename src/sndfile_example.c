/* See LICENSE file for copyright and license details. */
#define _POSIX_C_SOURCE 200112L
#include <float.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <glib-2.0/glib.h>
#include <sndfile.h>
#ifdef G_OS_WIN32
  #include <windows.h>
#endif

#include "./ebur128.h"

long nproc() {
  long ret = 1;
#if defined(G_OS_UNIX) && defined(_SC_NPROCESSORS_ONLN)
  ret = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(G_OS_WIN32)
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  ret = sysinfo.dwNumberOfProcessors;
#endif
  return ret;
}

#define CHECK_ERROR(condition, message, errorcode, goto_point)                 \
  if ((condition)) {                                                           \
    fprintf(stderr, message);                                                  \
    errcode = (errorcode);                                                     \
    goto goto_point;                                                           \
  }

struct gain_data {
  char* const* track_names;
  int calculate_lra, tag_rg;
  ebur128_state** library_states;
  double* segment_loudness;
  double* segment_peaks;
};

void calculate_gain_of_file(void* user, void* user_data) {
  struct gain_data* gd = (struct gain_data*) user_data;
  size_t i = (size_t) GPOINTER_TO_INT(user) - 1;
  char* const* av = gd->track_names;

  SF_INFO file_info;
  SNDFILE* file;
  float* buffer;
  ebur128_state* st = NULL;
  int channels, samplerate;
  size_t nr_frames_read, nr_frames_read_all;

  int errcode, result;

  gd->segment_loudness[i] = DBL_MAX;
  memset(&file_info, '\0', sizeof(file_info));
  file = sf_open(av[i], SFM_READ, &file_info);
  CHECK_ERROR(!file, "Could not open file!\n", 1, endloop)
  channels = file_info.channels;
  samplerate = file_info.samplerate;
  nr_frames_read_all = 0;

  st = ebur128_init(channels,
                    samplerate,
                    EBUR128_MODE_I |
                    (gd->calculate_lra ? EBUR128_MODE_LRA : 0));
  CHECK_ERROR(!st, "Could not initialize EBU R128!\n", 1, close_file)
  gd->library_states[i] = st;

  result = sf_command(file, SFC_GET_CHANNEL_MAP_INFO,
                            (void*) st->channel_map,
                            channels * (int) sizeof(int));
  /* If sndfile found a channel map, set it with
   * ebur128_set_channel_map */
  if (result == SF_TRUE) {
    int j;
    for (j = 0; j < (int) st->channels; ++j) {
      switch (st->channel_map[j]) {
        case SF_CHANNEL_MAP_INVALID:
          ebur128_set_channel(st, j, EBUR128_UNUSED);         break;
        case SF_CHANNEL_MAP_MONO:
          ebur128_set_channel(st, j, EBUR128_CENTER);         break;
        case SF_CHANNEL_MAP_LEFT:
          ebur128_set_channel(st, j, EBUR128_LEFT);           break;
        case SF_CHANNEL_MAP_RIGHT:
          ebur128_set_channel(st, j, EBUR128_RIGHT);          break;
        case SF_CHANNEL_MAP_CENTER:
          ebur128_set_channel(st, j, EBUR128_CENTER);         break;
        case SF_CHANNEL_MAP_REAR_LEFT:
          ebur128_set_channel(st, j, EBUR128_LEFT_SURROUND);  break;
        case SF_CHANNEL_MAP_REAR_RIGHT:
          ebur128_set_channel(st, j, EBUR128_RIGHT_SURROUND); break;
        default:
          ebur128_set_channel(st, j, EBUR128_UNUSED);         break;
      }
    }
  /* Special case seq-3341-6-5channels-16bit.wav.
   * Set channel map with function ebur128_set_channel. */
  } else if (channels == 5) {
    ebur128_set_channel(st, 0, EBUR128_LEFT);
    ebur128_set_channel(st, 1, EBUR128_RIGHT);
    ebur128_set_channel(st, 2, EBUR128_CENTER);
    ebur128_set_channel(st, 3, EBUR128_LEFT_SURROUND);
    ebur128_set_channel(st, 4, EBUR128_RIGHT_SURROUND);
  }

  buffer = (float*) malloc(st->samplerate * st->channels * sizeof(float));
  CHECK_ERROR(!buffer, "Could not allocate memory!\n", 1, close_file)
  gd->segment_peaks[i] = 0.0;
  for (;;) {
    nr_frames_read = (size_t) sf_readf_float(file, buffer,
                                           (sf_count_t) st->samplerate);
    if (!nr_frames_read) break;
    if (gd->tag_rg) {
      size_t j;
      for (j = 0; j < (size_t) nr_frames_read * st->channels; ++j) {
        if (buffer[j] > gd->segment_peaks[i])
          gd->segment_peaks[i] = buffer[j];
        else if (-buffer[j] > gd->segment_peaks[i])
          gd->segment_peaks[i] = -buffer[j];
      }
    }
    nr_frames_read_all += nr_frames_read;
    result = ebur128_add_frames_float(st, buffer, (size_t) nr_frames_read);
    CHECK_ERROR(result, "Internal EBU R128 error!\n", 1, free_buffer)
  }
  if (file && (size_t) file_info.frames != nr_frames_read_all) {
    fprintf(stderr, "Warning: Could not read full file"
                            " or determine right length!\n");
  }

  gd->segment_loudness[i] = ebur128_loudness_global(st);
  fprintf(stderr, "segment %d: %.2f LUFS\n", (int) i + 1,
                  gd->segment_loudness[i]);

free_buffer:
  free(buffer);
  buffer = NULL;

close_file:
  if (sf_close(file)) {
    fprintf(stderr, "Could not close input file!\n");
  }
  file = NULL;

endloop: ;
}


int main(int ac, char* const av[]) {
  int result;
  int i, c;
  int errcode = 0;

  GThreadPool* pool;
  struct gain_data gd;
  gd.calculate_lra = 0;
  gd.tag_rg = 0;

  g_thread_init(NULL);

  CHECK_ERROR(ac < 2, "usage: r128-test [-r] [-t] FILENAME(S) ...\n\n"
                      " -r: calculate loudness range in LRA\n"
                      " -t: output ReplayGain tagging info\n", 1, exit)
  while ((c = getopt(ac, av, "tr")) != -1) {
    switch (c) {
      case 't':
        gd.tag_rg = 1;
        break;
      case 'r':
        gd.calculate_lra = 1;
        break;
      default:
        return 1;
        break;
    }
  }

  gd.track_names = &av[optind];
  gd.segment_loudness = calloc((size_t) (ac - optind), sizeof(double));
  gd.segment_peaks = calloc((size_t) (ac - optind), sizeof(double));
  gd.library_states = calloc((size_t) (ac - optind), sizeof(ebur128_state*));

  pool = g_thread_pool_new(calculate_gain_of_file, &gd, (int) nproc(),
                           FALSE, NULL);

  for (i = optind; i < ac; ++i) {
    g_thread_pool_push(pool, GINT_TO_POINTER(i - optind + 1), NULL);
  }
  g_thread_pool_free(pool, FALSE, TRUE);

  result = 1;
  for (i = 0; i < ac - optind; ++i) {
    if (!gd.library_states[i]) {
      result = 0;
    }
  }

  if (result) {
    double gated_loudness;
    gated_loudness = ebur128_loudness_global_multiple(gd.library_states,
                                                      (size_t) (ac - optind));
    fprintf(stderr, "global loudness: %.2f LUFS\n", gated_loudness);

    /*
    if (calculate_lra) {
      fprintf(stderr, "LRA: %.2f\n", ebur128_loudness_range(st));
    }
    */

    if (gd.tag_rg) {
      double global_peak = 0.0;
      for (i = 0; i < ac - optind; ++i) {
        if (gd.segment_peaks[i] > global_peak) {
          global_peak = gd.segment_peaks[i];
        }
      }
      for (i = optind; i < ac; ++i) {
        printf("%.8f %.8f %.8f %.8f\n", -18.0 - gd.segment_loudness[i - optind],
                                        gd.segment_peaks[i - optind],
                                        -18.0 - gated_loudness,
                                        global_peak);
      }
    }
  }

  for (i = 0; i < ac - optind; ++i) {
    if (gd.library_states[i]) {
      ebur128_destroy(&gd.library_states[i]);
    }
  }
  free(gd.library_states);
  free(gd.segment_loudness);
  free(gd.segment_peaks);

exit:
  return errcode;
}
