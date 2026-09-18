#define _GNU_SOURCE

/*
 * Small lab-only ALSA probe for the LicheeRV Nano onboard ADC/DAC.
 *
 * It deliberately uses the hw PCM devices rather than a desktop mixer or
 * service.  Capture is written as interleaved S16_LE samples, and playback
 * writes a short block of zeroes so that the kernel/DMA path is exercised
 * without making a loud test tone.
 */
#include <alsa/asoundlib.h>
#include <alloca.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die_alsa(const char *what, int err)
{

	fprintf(stderr, "%s: %s\n", what, snd_strerror(err));
	exit(EXIT_FAILURE);
}

static snd_config_t *lab_config(void)
{
	static snd_config_t *root;
	static const char text[] =
		"pcm.camera_capture { type hw card 0 device 1 subdevice 0 }\n"
		"pcm.board_playback { type hw card 0 device 0 subdevice 0 }\n";
	int err;

	if (root)
		return root;
	if ((err = snd_config_load_string(&root, text, sizeof(text) - 1)) < 0)
		die_alsa("snd_config_load_string", err);
	return root;
}

static snd_pcm_t *open_pcm(const char *device, snd_pcm_stream_t direction,
				   unsigned int channels, unsigned int rate,
				   snd_pcm_uframes_t requested_buffer)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw = NULL;
	unsigned int actual_rate = rate;
	snd_pcm_uframes_t period = 1024;
	snd_pcm_uframes_t buffer = 0;
	int err;

	/* The bridge accepts a literal hw: device, whereas the default lab path
	 * uses aliases in a minimal config.  Permit both so a diagnostic can make
	 * the ALSA open path identical to the bridge. */
	if (!strncmp(device, "hw:", 3))
		err = snd_pcm_open(&pcm, device, direction, 0);
	else
		err = snd_pcm_open_lconf(&pcm, device, direction, 0, lab_config());
	if (err < 0)
		die_alsa("snd_pcm_open", err);

	snd_pcm_hw_params_alloca(&hw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0)
		die_alsa("snd_pcm_hw_params_any", err);
	if ((err = snd_pcm_hw_params_set_access(pcm, hw,
						SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		die_alsa("snd_pcm_hw_params_set_access", err);
	if ((err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0)
		die_alsa("snd_pcm_hw_params_set_format", err);
	if ((err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0)
		die_alsa("snd_pcm_hw_params_set_channels", err);
	if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &actual_rate, NULL)) < 0)
		die_alsa("snd_pcm_hw_params_set_rate_near", err);
	if (actual_rate != rate) {
		fprintf(stderr, "%s: requested rate %u, got %u\n",
				device, rate, actual_rate);
		exit(EXIT_FAILURE);
	}
	if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0)
		die_alsa("snd_pcm_hw_params_set_period_size_near", err);
	if (requested_buffer &&
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw,
						     &requested_buffer)) < 0)
		die_alsa("snd_pcm_hw_params_set_buffer_size_near", err);
	if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
		die_alsa("snd_pcm_hw_params", err);
	if ((err = snd_pcm_hw_params_get_buffer_size(hw, &buffer)) < 0)
		die_alsa("snd_pcm_hw_params_get_buffer_size", err);

	fprintf(stdout, "%s %s channels=%u rate=%u period=%lu buffer=%lu\n", device,
			direction == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
			channels, actual_rate, (unsigned long)period, (unsigned long)buffer);
	return pcm;
}

static snd_pcm_sframes_t recover_pcm(snd_pcm_t *pcm, snd_pcm_sframes_t err)
{
	if (err == -EPIPE || err == -ESTRPIPE) {
		int recovered = snd_pcm_recover(pcm, (int)err, 1);
		if (recovered < 0)
			die_alsa("snd_pcm_recover", recovered);
		return 0;
	}
	return err;
}

static void capture_pcm(const char *device, const char *path,
				unsigned int seconds, snd_pcm_uframes_t buffer,
				int explicit_start)
{
	const unsigned int channels = 2;
	const unsigned int rate = 48000;
	const snd_pcm_uframes_t chunk = 1024;
	snd_pcm_t *pcm = open_pcm(device, SND_PCM_STREAM_CAPTURE, channels, rate,
					    buffer);
	int16_t *samples = calloc(chunk * channels, sizeof(*samples));
	FILE *out = fopen(path, "wb");
	unsigned long long remaining = (unsigned long long)rate * seconds;
	unsigned long long captured = 0;
	long long sample_min = INT16_MAX;
	long long sample_max = INT16_MIN;
	unsigned long long nonzero = 0;
	long double sumsq = 0;

	if (!samples || !out) {
		perror("capture allocation/output");
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_prepare(pcm) < 0)
		die_alsa("snd_pcm_prepare capture", -1);
	if (explicit_start && snd_pcm_start(pcm) < 0)
		die_alsa("snd_pcm_start capture", -1);

	while (remaining) {
		snd_pcm_uframes_t want = remaining < chunk ? remaining : chunk;
		snd_pcm_sframes_t got = snd_pcm_readi(pcm, samples, want);
		if (got < 0) {
			got = recover_pcm(pcm, got);
			if (got == 0)
				continue;
			die_alsa("snd_pcm_readi", (int)got);
		}
		if (got == 0)
			continue;
		if (fwrite(samples, channels * sizeof(*samples), (size_t)got, out) !=
			(size_t)got) {
			perror("capture write");
			exit(EXIT_FAILURE);
		}
		for (snd_pcm_sframes_t frame = 0; frame < got; frame++) {
			for (unsigned int channel = 0; channel < channels; channel++) {
				long long sample = samples[frame * channels + channel];
				if (sample < sample_min)
					sample_min = sample;
				if (sample > sample_max)
					sample_max = sample;
				if (sample)
					nonzero++;
				sumsq += (long double)sample * sample;
			}
		}
		captured += (unsigned long long)got;
		remaining -= (unsigned long long)got;
	}

	if (fclose(out) != 0)
		perror("capture close");
	fprintf(stdout,
			"capture_frames=%llu bytes=%llu sample_min=%lld sample_max=%lld "
			"nonzero_samples=%llu rms=%.2Lf\n",
		captured, captured * channels * sizeof(*samples), sample_min,
			sample_max, nonzero, (captured && channels) ?
				sqrtl(sumsq / (captured * channels)) : 0.0L);
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	free(samples);
}

static void playback_pcm(const char *device, snd_pcm_uframes_t buffer)
{
	const unsigned int channels = 2;
	const unsigned int rate = 48000;
	const snd_pcm_uframes_t chunk = 1024;
	const unsigned int seconds = 1;
	snd_pcm_t *pcm = open_pcm(device, SND_PCM_STREAM_PLAYBACK, channels, rate,
					    buffer);
	int16_t *silence = calloc(chunk * channels, sizeof(*silence));
	unsigned long long remaining = (unsigned long long)rate * seconds;
	unsigned long long played = 0;

	if (!silence)
		perror("playback allocation"), exit(EXIT_FAILURE);
	if (snd_pcm_prepare(pcm) < 0)
		die_alsa("snd_pcm_prepare playback", -1);

	while (remaining) {
		snd_pcm_uframes_t want = remaining < chunk ? remaining : chunk;
		snd_pcm_sframes_t done = snd_pcm_writei(pcm, silence, want);
		if (done < 0) {
			done = recover_pcm(pcm, done);
			if (done == 0)
				continue;
			die_alsa("snd_pcm_writei", (int)done);
		}
		if (done == 0)
			continue;
		played += (unsigned long long)done;
		remaining -= (unsigned long long)done;
	}
	if (snd_pcm_drain(pcm) < 0)
		fprintf(stderr, "snd_pcm_drain playback failed\n");
	fprintf(stdout, "playback_frames=%llu bytes=%llu\n", played,
			played * channels * sizeof(*silence));
	snd_pcm_close(pcm);
	free(silence);
}

int main(int argc, char **argv)
{
	char *end;
	unsigned long parsed_buffer = 0;
	int explicit_start = 0;

	if (argc != 4 && argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s <capture-device> <playback-device> <output> [buffer-frames [start]]\n",
				argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 5) {
		errno = 0;
		parsed_buffer = strtoul(argv[4], &end, 10);
		if (errno || !argv[4][0] || *end || !parsed_buffer ||
		    parsed_buffer > UINT_MAX) {
			fprintf(stderr, "invalid buffer-frames: %s\n", argv[4]);
			return EXIT_FAILURE;
		}
	}
	if (argc == 6) {
		if (strcmp(argv[5], "start")) {
			fprintf(stderr, "invalid start mode: %s\n", argv[5]);
			return EXIT_FAILURE;
		}
		explicit_start = 1;
	}
	capture_pcm(argv[1], argv[3], 3, (snd_pcm_uframes_t)parsed_buffer,
			explicit_start);
	playback_pcm(argv[2], (snd_pcm_uframes_t)parsed_buffer);
	return EXIT_SUCCESS;
}
