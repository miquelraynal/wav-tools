// SPDX-License-Identifier: GPL-2.0+
/*
 * *.wav file creator with sinewaves at different frequencies
 *
 * Copyright (C) 2023 Bootlin
 * Author: Miquel Raynal <miquel.raynal@bootlin.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#include "wav-lib.h"

#define DEFAULT_NCHANS 2
#define DEFAULT_RATE 48000
#define DEFAULT_BPS 32
#define DEFAULT_DURATION 10
#define DEFAULT_NFREQS 4

static void fill_audio_buf(uint8_t *buf, double **waves, const struct audio *wav)
{
	int16_t *buf_i16 = (int16_t *)buf;
	int32_t *buf_i32 = (int32_t *)buf;
	unsigned int s, c;

	switch (wav->bits_per_sample) {
	case 16:
		for (s = 0; s < wav->samples_per_chan; s++)
			for (c = 0; c < wav->channels; c++)
				buf_i16[(s * wav->channels) + c] = waves[c][s] * INT16_MAX;
		break;
	case 32:
		for (s = 0; s < wav->samples_per_chan; s++)
			for (c = 0; c < wav->channels; c++)
				buf_i32[(s * wav->channels) + c] = waves[c][s] * INT32_MAX;
		break;
	default:
		/* Checked in parse_args() */
		break;
	};
}

static void fill_audio_wave(double *wave, unsigned int *freqs, const struct audio *wav)
{
	unsigned int s, f;

	/* w(t) = sin(2 PI f t) */
	for (s = 0; s < wav->samples_per_chan; s++) {
		for (f = 0; f < wav->freqs_per_chan; f++)
			wave[s] += sin(2.0 * M_PI * freqs[f] * s / wav->sample_rate);

		/* Normalize power */
		wave[s] /= wav->freqs_per_chan;
	}
}

static void log_freqs(FILE *fd, unsigned int **freqs, const struct audio *wav)
{
	unsigned int c, i;

	for (c = 0; c < wav->channels; c++) {
		fprintf(fd, "Frequencies on channel %d:\n", c);
		for (i = 0; i < wav->freqs_per_chan; i++)
			fprintf(fd, "* %u/ %u Hz\n", i, freqs[c][i]);
	}
	fprintf(fd, "\n");
}

static void print_help(FILE *fd, char *tool_name)
{
	fprintf(fd, "\n"
		"Generates a WAV audio file on the standard output, with a number of known frequencies added on each channel.\n"
		"Listening to this file is discouraged, as pure sinewaves are as mathematically beautiful as unpleasant to the human ears.\n\n"
		"%s [-c <nchans>] [-r <rate>] [-b <bps>] [-d <duration>] [-f <nfreqs>] > play.wav\n"
		"	-c: Number of channels (default: %u)\n"
		"	-r: Sampling rate in Hz (default: %u, min: %u)\n"
		"	-b: Bits per sample (default: %u, supp: 16, 32)\n"
		"	-d: Duration in seconds (default: %u, min: %u)\n"
		"	-f: Number of frequencies per channel (default: %u)\n\n",
		tool_name, DEFAULT_NCHANS, DEFAULT_RATE, 2 * MIN_FREQ, DEFAULT_BPS,
		DEFAULT_DURATION, MIN_DURATION, DEFAULT_NFREQS);
}

static int parse_args(int argc, char *argv[], struct audio *wav)
{
	char *tool_name = argv[0];
	int option, val;

	while ((option = getopt(argc, argv, ":c:r:b:d:f:h")) != -1) {
		switch(option){
		case 'c':
			val = strtol(optarg, NULL, 0);
			wav->channels = val;
			break;
		case 'r':
			val = strtol(optarg, NULL, 0);
			wav->sample_rate = val;
			break;
		case 'b':
			val = strtol(optarg, NULL, 0);
			wav->bits_per_sample = val;
			break;
		case 'd':
			val = strtol(optarg, NULL, 0);
			wav->duration_s = val;
			break;
		case 'f':
			val = strtol(optarg, NULL, 0);
			wav->freqs_per_chan = val;
			break;
		case 'h':
			print_help(stderr, tool_name);
			return -1;
		case ':':
			fprintf(stderr, "Missing value with option %c\n", option);
			print_help(stderr, tool_name);
			return -1;
		case '?':
			fprintf(stderr, "Unknown option: %c\n", optopt);
			print_help(stderr, tool_name);
			return -1;
		}

		if (val <= 0) {
			fprintf(stderr, "Wrong user input: negative or null value\n");
			print_help(stderr, tool_name);
			return -1;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Unknown extra arguments: %s\n", argv[optind]);
		print_help(stderr, tool_name);
		return -1;
	}

	if (wav->sample_rate < 2 * MIN_FREQ) {
		fprintf(stderr, "Invalid frequency\n");
		print_help(stderr, tool_name);
		return -1;
	}

	if (wav->bits_per_sample != 16 && wav->bits_per_sample != 32) {
		fprintf(stderr, "Unsupported number of bits per sample\n");
		print_help(stderr, tool_name);
		return -1;
	}

	if (wav->duration_s < MIN_DURATION) {
		fprintf(stderr, "Audio file would be too short\n");
		print_help(stderr, tool_name);
		return -1;
	}

	wav->samples_per_chan = wav->sample_rate * wav->duration_s;

	return 0;
}

static struct riff_container riff = {
	.tag = {'R', 'I', 'F', 'F'},
	.file_len = UINT32_MAX,
	.wav_container = {
		.tag = {'W', 'A', 'V', 'E'},
		.fmt_container = {
			.tag = {'f', 'm', 't', ' '},
			.chunk_size = sizeof(struct wav_format) -
			sizeof(struct data_container),
			.wav_format = {
				.format_tag = WAVE_FORMAT_PCM,
				.data_container = {
					.tag = {'d', 'a', 't', 'a'},
					.chunk_size = UINT32_MAX,
				},
			},
		},
	},
};

int main(int argc, char *argv[])
{
	const struct audio wav = {
		.channels = DEFAULT_NCHANS,
		.sample_rate = DEFAULT_RATE,
		.bits_per_sample = DEFAULT_BPS,
		.duration_s = DEFAULT_DURATION,
		.freqs_per_chan = DEFAULT_NFREQS,
	};
	struct wav_format *hdr = &riff.wav_container.fmt_container.wav_format;
	unsigned int **freqs;
	unsigned int data_sz;
	unsigned int c;
	double **waves;
	uint8_t *buf;
	int ret = -1;

	/* Parse args */
	if (parse_args(argc, argv, (struct audio *)&wav))
		return -1;

	fprintf(stderr, "Generating audio file with following parameters:\n");
	log_parameters(stderr, &wav);
	fprintf(stderr, "\n");

	/* Update the WAV format header */
	hdr->channels = wav.channels;
	hdr->samples_per_sec = wav.sample_rate;
	hdr->avg_bytes_per_sec = wav.channels * wav.sample_rate * wav.bits_per_sample / 8;
	hdr->block_align = wav.channels * wav.bits_per_sample / 8;
	hdr->pcm_format.bits_per_sample = wav.bits_per_sample;

	/* List expected frequencies per channel */
	freqs = (unsigned int **)alloc_matrix(wav.channels, wav.freqs_per_chan,
					      sizeof(**freqs));
	if (!freqs)
		return -1;

	if (fill_desired_freqs(freqs, &wav))
		return -1;

	log_freqs(stderr, freqs, &wav);

	/* Generate audio waves for each channels */
	waves = (double **)alloc_matrix(wav.channels, wav.samples_per_chan,
					sizeof(**waves));
	if (!waves)
		goto free_freqs;

	for (c = 0; c < wav.channels; c++)
		fill_audio_wave(waves[c], freqs[c], &wav);

	/* Generate audio buffer */
	data_sz = wav.channels * wav.samples_per_chan * wav.bits_per_sample / 8;
	buf = calloc(data_sz, 1);
	if (!buf)
		goto free_waves;

	fill_audio_buf(buf, waves, &wav);

	/* Generate the final *.wav output */
	riff.wav_container.fmt_container.wav_format.data_container.chunk_size = data_sz;
	riff.file_len = sizeof(riff) + data_sz;

	fwrite(&riff, sizeof(riff), 1, stdout);
	fwrite(buf, data_sz, 1, stdout);

	ret = 0;
	free(buf);
free_waves:
	free_array((void **)waves, c);
	free(waves);
free_freqs:
	free_array((void **)freqs, c);
	free(freqs);

	return ret;
}
