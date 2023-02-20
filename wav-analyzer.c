// SPDX-License-Identifier: GPL-2.0+
/*
 * *.wav file frequency analyzer
 *
 * Copyright (C) 2023 Bootlin
 * Author: Miquel Raynal <miquel.raynal@bootlin.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <gsl/gsl_fft_real.h>

#include "wav-lib.h"

#define MAX_FREQS_PER_CHAN 64
#define POWER_NOISE_LEVEL 5.0 /* Arbitrary Unit */
#define FREQ_ACCURACY 1 /* Hz */

/* Find the next power of 2, useful for performing FFT calculations */
static uint32_t next_pow_2(unsigned int val)
{
	int i;

	if (val & (1 << 31))
		return 1 << 31;

	for (i = 30; i >= 0; i--)
		if (val & (1 << i))
			break;

	return 1 << (i + 1);
}

static bool freqs_are_equal(unsigned int f1, unsigned int f2, unsigned int accuracy)
{
	if (f1 <= (f2 + accuracy) &&
	    f1 >= (f2 - accuracy))
		return true;

	return false;
}

static bool freq_is_listed(unsigned int *freqs, unsigned int nfreqs,
			   unsigned int frequency)
{
	unsigned int i;

	for (i = 0; i < MAX_FREQS_PER_CHAN; i++) {
		if (i == nfreqs)
			break;

		if (freqs_are_equal(frequency, freqs[i], FREQ_ACCURACY))
			return true;
	}

	return false;
}

static void add_freq_to_list(unsigned int *freqs, unsigned int *nfreqs,
			     unsigned int frequency)
{
	if (*nfreqs + 1 >= MAX_FREQS_PER_CHAN) {
		fprintf(stderr, "Maximum number of detected frequencies reached\n");
		return;
	}

	if (!freq_is_listed(freqs, *nfreqs, frequency)) {
		freqs[*nfreqs] = frequency;
		*nfreqs = *nfreqs + 1;
	}
}

/* Mitigate windowing consequences when performing spectral analysis, see:
 * https://en.wikipedia.org/wiki/Window_function#Hann_and_Hamming_windows
 * Implementation taken from igt-gpu-tools, see COPYING.
 */
static double hann_window(double val, unsigned int idx, unsigned int len)
{
	return val * 0.5 * (1 - cos(2.0 * M_PI * (double) idx / (double) len));
}

/* Extract the major frequencies by:
 * - Windowing the data set
 * - Performing a discrete FFT
 * - Generating a power distribution across the frequencies
 * - Deriving a threshold as being half of the maximum power
 * - Finding a maximum each time the power distribution crosses the threshold
 * - Listing these maxima as being the relevant frequencies for our analysis
 * Implementation inspired from igt-gpu-tools, see COPYING.
 */
static void extract_frequencies(unsigned int *freqs, unsigned int *nfreqs,
				double *wave, unsigned int size,
				double *max_thresh, const struct audio *wav)
{
	size_t power_len = size / 2 + 1;
	double data[size], power[power_len], local_max = 0, maximum, threshold;
	unsigned int local_max_idx = 0, i;
	unsigned int frequency;
	bool above = false;

	/* Don't smash the wave, GSL functions work in-place */
	memcpy(data, wave, size * sizeof(double));

	/* Hann-window the signal to limit harmonics on discontinuous segments */
	for (i = 0; i < size; i++)
		data[i] = hann_window(data[i], i, size);

	/* Perform Discrete FFT in-place */
	if (gsl_fft_real_radix2_transform(data, 1, size))
		return;

	/* Extract the computed power out of the real and imaginary parts:
	 * http://linux.math.tifr.res.in/manuals/html/gsl-ref-html/gsl-ref_15.html#SEC240
	 */
	power[0] = data[0];
	for (i = 1; i < power_len - 1; i++)
		power[i] = hypot(data[i], data[size - i]);
	power[power_len - 1] = data[size / 2];

	/* Find maximum power, derive a threshold above which we will consider a
	 * peak and save the maximum threshold used on the channel to let the
	 * user know about the amount of possible noise.
	 */
	maximum = 0;
	for (i = (MIN_FREQ * size / wav->sample_rate); i < power_len - 1; i++) {
		if (power[i] > maximum)
			maximum = power[i];
	}

	threshold = maximum / 2;
	if (threshold < POWER_NOISE_LEVEL)
		return;

	if (threshold > *max_thresh)
		*max_thresh = threshold;

	/* Read peaks in the range [FREQ_MIN; Fs/2[ */
	for (i = (MIN_FREQ * size / wav->sample_rate); i < power_len - 1; i++) {
		if (power[i] > threshold) {
			/* We are looking for a max */
			above = true;
			if (power[i] > local_max) {
				local_max = power[i];
				local_max_idx = i;
			}
		} else {
			if (above) {
				/* We found a frequency */
				frequency = wav->sample_rate * local_max_idx / size;
				add_freq_to_list(freqs, nfreqs, frequency);
			}
			above = false;
			local_max = 0.0;
		}
	}
}

static int32_t get_sample(int32_t *buf, unsigned int chan,
			  unsigned int channels, unsigned int sample)
{
	return buf[(channels * sample) + chan];
}

static void extract_channel(double *wave, int32_t *buf, unsigned int chan,
			    const struct audio *wav)
{
	unsigned int s;
	double factor;

	switch (wav->bits_per_sample) {
	case 16:
		factor = INT16_MAX;
		break;
	case 24:
		factor = 0x7FFFFF;
		break;
	case 32:
		factor = INT32_MAX;
		break;
	default:
		factor = 0;
	};

	for (s = 0; s < wav->samples_per_chan; s++)
		wave[s] = (double)get_sample(buf, chan, wav->channels, s) / factor;
}

static int extract_audio_parameters(struct wav_format *wav_format, struct audio *wav)
{
	int data_sz;

	wav->channels = wav_format->channels;
	wav->sample_rate = wav_format->samples_per_sec;
	data_sz = wav_format->data_container.chunk_size;
	if (!wav->channels || !wav->sample_rate || !data_sz || data_sz % wav->channels) {
		fprintf(stderr, "Corrupted header (%u channels, %u Hz, %d B)\n",
			wav->channels, wav->sample_rate, data_sz);
		return -1;
	}

	wav->bits_per_sample = wav_format->pcm_format.bits_per_sample;
	switch (wav->bits_per_sample) {
	case 32:
		break;
	case 24:
	case 16:
		fprintf(stderr, "FYI: Untested behavior\n");
		break;
	default:
		fprintf(stderr, "Unsupported: %u bits per sample\n",
			wav->bits_per_sample);
		return -1;
	}

	wav->samples_per_chan = data_sz / wav->channels / (wav->bits_per_sample / 8);
	wav->duration_s = wav->samples_per_chan / wav->sample_rate;
	if (wav->duration_s < MIN_DURATION) {
		fprintf(stderr, "Audio file too short (%u seconds)\n", wav->duration_s);
		return -1;
	}

	return data_sz;
}

static void print_help(FILE *fd, char *tool_name)
{
	fprintf(fd, "\n"
		"Analyzes a WAV audio file on the standard input and exposes its major frequencies.\n"
		"The tool extracts the audio parameters from the *.wav header.\n"
		"Up to %u frequencies can be discovered per channel.\n"
		"It is possible to check for frequencies generated with the same heuristics.\n\n"
		"%s [-f <nfreqs>] < record.wav\n"
		"	-f: Number of expected frequencies per channel\n\n",
		MAX_FREQS_PER_CHAN, tool_name);
}

static int parse_args(int argc, char *argv[], struct audio *wav)
{
	char *tool_name = argv[0];
	int option, val;

	while ((option = getopt(argc, argv, ":c:r:b:d:f:h")) != -1) {
		switch(option){
		case 'f':
			val = strtol(optarg, NULL, 0);
			wav->freqs_per_chan = val;
			break;
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

	return 0;
}

int main(int argc, char *argv[])
{
	struct riff_container riff;
	struct wav_format *wav_format = &riff.wav_container.fmt_container.wav_format;
	const struct audio wav = {
		.freqs_per_chan = 0,
	};
	unsigned int **cfreqs, **efreqs, *ncfreqs;
	unsigned int offset, slide, windows_sz, i, c;
	size_t sz, data_sz;
	int32_t *buf;
	double *wave, *thresholds;
	int ret = -1;

	/* Parse args */
	if (parse_args(argc, argv, (struct audio *)&wav))
		return -1;

	/* Read the *.wav file from the standard input */
	freopen(NULL, "rb", stdin);
	sz = fread(&riff, 1, sizeof(riff), stdin);
	if (sz != sizeof(riff)) {
		fprintf(stderr, "Malformed WAV file\n");
		return -1;
	}

	/* Extract parameters from the *.wav header and check their validity */
	data_sz = extract_audio_parameters(wav_format, (struct audio *)&wav);
	if (data_sz <= 0)
		return 1;

	fprintf(stderr, "Analyzing audio file with following parameters:\n");
	log_parameters(stderr, &wav);
	fprintf(stderr, "\n");

	/* Read the *.wav sound data */
	buf = malloc(data_sz);
	if (!buf)
		return -1;

	sz = fread(buf, 1, data_sz, stdin);
	if (sz != data_sz) {
		fprintf(stderr, "Partial audio content, aborting\n");
		goto free_buf;
	}

	/* Allocate the array to store the frequencies extracted from the file */
	ncfreqs = calloc(wav.channels, sizeof(unsigned int));
	if (!ncfreqs)
		goto free_buf;

	cfreqs = (unsigned int **)alloc_matrix(wav.channels, MAX_FREQS_PER_CHAN,
					       sizeof(unsigned int));
	if (!cfreqs)
		goto free_ncfreqs;

	thresholds = calloc(wav.channels, sizeof(double));
	if (!thresholds)
		goto free_cfreqs;

	/* Process each channel, one at a time, with a sliding FFT:
	 * - Make the window at least 1s wide.
	 * - Start after 0.5s, stop 0.5s from the end to avoid possible glitches.
	 * - Slide the window by 0.5s to ensure a sufficient overlap.
	 * - The slide/window size are rounded up to the next higher power of 2
	 *   in order to match the library requirements.
	 */
	wave = malloc(wav.samples_per_chan * sizeof(double));
	if (!wave)
		goto free_thresholds;

	offset = wav.sample_rate / 2;
	slide = next_pow_2(wav.sample_rate / 2);
	windows_sz = 2 * slide;
	for (c = 0; c < wav.channels; c++) {
		/* Extract samples from a single channel and convert them into floats */
		extract_channel(wave, buf, c, &wav);

		/* Perform a sliding window discrete FFT */
		for (i = offset; i + windows_sz < wav.samples_per_chan - offset; i += slide)
			extract_frequencies(cfreqs[c], &ncfreqs[c], &wave[i],
					    windows_sz, &thresholds[c], &wav);
	}

	/* The user did not require frequency comparisons, just print the analysis */
	if (!wav.freqs_per_chan) {
		for (c = 0; c < wav.channels; c++) {
			printf("Frequencies found on channel %d (max threshold: %.1f):\n",
			       c, thresholds[c]);
			if (!ncfreqs[c])
				printf("None.\n");
			for (i = 0; i < ncfreqs[c]; i++)
				printf("* %u Hz\n", cfreqs[c][i]);
		}

		ret = 0;
		goto free_wave;
	}

	/* List expected frequencies per channel */
	efreqs = (unsigned int **)alloc_matrix(wav.channels, wav.freqs_per_chan,
					       sizeof(unsigned int));
	if (!efreqs)
		goto free_wave;

	if (fill_desired_freqs(efreqs, &wav))
		goto free_efreqs;

	/* Compare computed and expected frequencies */
	for (c = 0; c < wav.channels; c++) {
		unsigned int found = 0, i;
		bool is_listed;

		printf("Frequencies expected on channel %d (%smax threshold: %.1f):\n",
		       c, !ncfreqs[c] ? "empty, " : "", thresholds[c]);
		for (i = 0; i < wav.freqs_per_chan; i++) {
			is_listed = freq_is_listed(cfreqs[c], ncfreqs[c], efreqs[c][i]);
			printf("* %u/ %u Hz: ", i, efreqs[c][i]);
			if (!is_listed) {
				printf("KO\n");
			} else {
				int diff = cfreqs[c][i] - efreqs[c][i];
				printf("ok");
				if (diff)
					printf(" (%d Hz)", diff);
				printf("\n");
				found++;
			}
		}

		if (found < ncfreqs[c]) {
			printf("Frequencies *not* expected on channel %d:\n", c);
			for (i = 0; i < ncfreqs[c]; i++) {
				is_listed = freq_is_listed(efreqs[c], wav.freqs_per_chan,
							   cfreqs[c][i]);
				if (!is_listed)
					printf("*    %u Hz: spurious\n", cfreqs[c][i]);
			}
		}
	}
	printf("\n");

	ret = 0;
free_efreqs:
	free_array((void **)efreqs, wav.channels);
	free(efreqs);
free_wave:
	free(wave);
free_thresholds:
	free(thresholds);
free_cfreqs:
	free_array((void **)cfreqs, wav.channels);
	free(cfreqs);
free_ncfreqs:
	free(ncfreqs);
free_buf:
	free(buf);

	return ret;
}
