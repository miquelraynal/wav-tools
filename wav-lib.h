// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Bootlin
 * Author: Miquel Raynal <miquel.raynal@bootlin.com>
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * WAV spec from Microsoft & IBM:
 * https://www.aelius.com/njh/wavemetatools/doc/riffmci.pdf
 */

#define WAVE_FORMAT_PCM 0x0001

struct data_container {
	char tag[4];
	uint32_t chunk_size;
} __attribute__((__packed__));

struct pcm_format {
	uint16_t bits_per_sample;
} __attribute__((__packed__));

struct wav_format {
	uint16_t format_tag;
	uint16_t channels;
	uint32_t samples_per_sec;
	uint32_t avg_bytes_per_sec;
	uint16_t block_align;
	struct pcm_format pcm_format;
	struct data_container data_container;
} __attribute__((__packed__));

struct fmt_container {
	char tag[4];
	uint32_t chunk_size;
	struct wav_format wav_format;
} __attribute__((__packed__));

struct wav_container {
	char tag[4];
	struct fmt_container fmt_container;
} __attribute__((__packed__));

struct riff_container {
	char tag[4];
	uint32_t file_len;
	struct wav_container wav_container;
} __attribute__((__packed__));

/* Shared functions and definitions */

#define MIN_FREQ 200 /* Hz */
#define MIN_DURATION 3 /* Seconds */

struct audio {
	unsigned int channels;
	unsigned int sample_rate;
	unsigned int bits_per_sample;
	unsigned int duration_s;
	unsigned int freqs_per_chan;
	unsigned int samples_per_chan;
};

void log_parameters(FILE *fd, const struct audio *wav);
int fill_desired_freqs(unsigned int **freqs, const struct audio *wav);
void free_array(void **array, unsigned int n);
void **alloc_matrix(unsigned int narrays, unsigned int nentries,
		    unsigned int elem_size);

/* The cleverness for int24_t, i32_to_i24 and i24_to_i32 is taken from PipeWire,
   see spa/plugins/audiomixer/mix-ops.h. Licensed under MIT. The project's
   reference website is: https://pipewire.org/
 */

typedef struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t v3;
	uint8_t v2;
	int8_t v1;
#else
	int8_t v1;
	uint8_t v2;
	uint8_t v3;
#endif
} __attribute__ ((packed)) int24_t;

static inline int24_t i32_to_i24(int32_t a)
{
	return (int24_t) {
		.v1 = (int8_t)(((int32_t)a) >> 16),
		.v2 = (uint8_t)(((uint32_t)a) >> 8),
		.v3 = (uint8_t)((uint32_t)a),
	};
}

static inline int32_t i24_to_i32(int24_t a)
{
	return ((int32_t)a.v1 << 16) | ((uint32_t)a.v2 << 8) | (uint32_t)a.v3;
}
