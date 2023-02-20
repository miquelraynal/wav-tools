// SPDX-License-Identifier: GPL-2.0+

#include <stdint.h>
#include <stdio.h>

#include "wav-lib.h"

void log_parameters(FILE *fd, const struct audio *wav)
{
	fprintf(fd, "* Channels: %u\n", wav->channels);
	fprintf(fd, "* Sample rate: %u Hz\n", wav->sample_rate);
	fprintf(fd, "* Bits per sample: S%u_LE\n", wav->bits_per_sample);
	fprintf(fd, "* Duration: %u seconds\n", wav->duration_s);
	if (wav->freqs_per_chan)
		fprintf(fd, "* Frequencies per channel: %u\n", wav->freqs_per_chan);
}

int fill_desired_freqs(unsigned int **freqs, const struct audio *wav)
{
	unsigned int c, i, delta_f, delta_c;

	/* Mind Nyquist-Shannon rule by limiting the analysis to half of the
	 * sampling frequency.
	 */
	delta_f = ((wav->sample_rate / 2) - MIN_FREQ) / wav->freqs_per_chan;
	delta_c = delta_f / (wav->channels + 1);

	if (!delta_f || !delta_c) {
		fprintf(stderr, "Cannot generate sine waves: not enough range\n");
		return -1;
	}

	for (c = 0; c < wav->channels; c++)
		for (i = 0; i < wav->freqs_per_chan; i++)
			freqs[c][i] = (MIN_FREQ + i * delta_f) + c * delta_c;

	return 0;
}

void free_array(void **array, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		free(array[i]);
}

void **alloc_matrix(unsigned int narrays, unsigned int nentries,
		    unsigned int elem_size)
{
	unsigned int i;
	void **matrix;

	matrix = calloc(narrays, sizeof(void *));
	if (!matrix)
		return NULL;

	for (i = 0; i < narrays; i++) {
		matrix[i] = calloc(nentries, elem_size);
		if (!matrix[i]) {
			free_array(matrix, i);
			free(matrix);
			return NULL;
		}
	}

	return matrix;
}
