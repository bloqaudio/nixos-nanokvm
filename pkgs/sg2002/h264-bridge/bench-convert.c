/* Benchmark the real conversion helper, without capture/encoder hardware.
 * Allocation and checksumming are outside the timed region. The checksum
 * must match across compiler variants before comparing process CPU time. */
#define main bridge_program_main
#include "sg2002-h264-bridge.c"
#undef main

int main(void)
{
	const unsigned int width = 1920, height = 1080, frames = 100;
	const size_t src_size = (size_t)width * height * 2;
	const size_t dst_size = (size_t)width * height * 3 / 2;
	uint8_t *src = malloc(src_size), *dst = malloc(dst_size);
	struct timespec start, end;
	uint32_t checksum = 2166136261U;
	size_t i;
	unsigned int frame;

	if (!src || !dst)
		return EXIT_FAILURE;
	for (i = 0; i < src_size; i++)
		src[i] = (uint8_t)(i * 37U + i / 101U);
	if (uyvy_to_nvxx(src, width, height, width * 2,
			 dst, width, height, width, 0))
		return EXIT_FAILURE;
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start))
		return EXIT_FAILURE;
	for (frame = 0; frame < frames; frame++) {
		src[0] = (uint8_t)frame;
		if (uyvy_to_nvxx(src, width, height, width * 2,
				 dst, width, height, width, 0))
			return EXIT_FAILURE;
	}
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end))
		return EXIT_FAILURE;
	for (i = 0; i < dst_size; i++)
		checksum = (checksum ^ dst[i]) * 16777619U;
	printf("frames=%u cpu_seconds=%.6f checksum=%08" PRIx32 "\n", frames,
	       (double)(end.tv_sec - start.tv_sec)
	       + (double)(end.tv_nsec - start.tv_nsec) / 1e9, checksum);
	free(dst);
	free(src);
	return EXIT_SUCCESS;
}
