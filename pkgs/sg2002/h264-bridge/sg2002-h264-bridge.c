/*
 * Small SG2002 V4L2 bridge:
 *
 *   /dev/video0 (UYVY capture) -> CPU UYVY->NV21 -> /dev/video1 (Coda H.264)
 *
 * The source node is deliberately negotiated rather than assumed to have a
 * particular mmap layout.  Live input is NV21: the SG2002 Coda driver performs
 * its explicit coherent VU-to-UV staging conversion for that format.  Direct
 * NV12 DMA was measured corrupt on the target, so it is not used here.
 *
 * Offline mode is useful on a host and is intentionally independent of V4L2:
 *   uyvy_nv12_bridge --raw nv12 width height input.uyvy output.raw
 *
 * Live mode is:
 *   uyvy_nv12_bridge [capture-node] [encoder-node]
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAPTURE_BUFFERS 2
#define ENCODER_BUFFERS 1
#define DEFAULT_CAPTURE "/dev/video0"
#define DEFAULT_ENCODER "/dev/video1"

struct mapped_buf {
	void *addr;
	size_t length;
};

struct mapped_queue {
	struct mapped_buf *bufs;
	unsigned int count;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do
		/* musl declares the Linux ioctl command as int; Linux truncates
		 * the command to its low 32 bits on entry, so this is safe for the
		 * V4L2 _IOC values and keeps both glibc and musl builds warning-free. */
#ifdef __GLIBC__
		ret = ioctl(fd, request, arg);
#else
		ret = ioctl(fd, (int)request, arg);
#endif
	while (ret < 0 && errno == EINTR);
	return ret;
}

static void die_errno(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
}

static int write_all(int fd, const void *data, size_t length)
{
	const uint8_t *cursor = data;

	while (length) {
		ssize_t written = write(fd, cursor, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		cursor += written;
		length -= (size_t)written;
	}
	return 0;
}

static int parse_u32(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || end == text || *end || parsed > UINT32_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

/* Convert packed UYVY to a tightly packed, progressive NV12 frame.  The live
 * path swaps the interleaved chroma bytes afterwards to submit NV21.
 *
 * src_stride is bytes per source line and dst_stride is bytes per destination
 * luma/chroma line.  Coda commonly rounds 1080 to 1088 macroblock lines; the
 * final dst_height-src_height lines repeat the final source line, so no
 * uninitialised DMA bytes can reach the encoder.
 */
static int uyvy_to_nv12(const uint8_t *src, unsigned int src_width,
			unsigned int src_height, unsigned int src_stride,
			uint8_t *dst, unsigned int dst_width, unsigned int dst_height,
			unsigned int dst_stride)
{
	unsigned int y, x;
	uint8_t *y_plane = dst;
	uint8_t *uv_plane = dst + (size_t)dst_stride * dst_height;

	if (!src || !dst || (src_width & 1) || (dst_width & 1) ||
	    (src_height & 1) || (dst_height & 1) || src_width > dst_width ||
	    src_height > dst_height || src_stride < src_width * 2 ||
	    dst_stride < dst_width)
		return -1;

	for (y = 0; y < dst_height; y++) {
		unsigned int sy = y < src_height ? y : src_height - 1;
		const uint8_t *line = src + (size_t)sy * src_stride;
		uint8_t *out = y_plane + (size_t)y * dst_stride;

		for (x = 0; x < src_width; x += 2) {
			out[x] = line[x * 2 + 1];
			out[x + 1] = line[x * 2 + 3];
		}
		/* A wider negotiated encoder frame is padded by edge extension. */
		for (; x < dst_width; x++)
			out[x] = out[src_width - 1];
	}

	for (y = 0; y < dst_height; y += 2) {
		unsigned int sy0 = y < src_height ? y : src_height - 1;
		unsigned int sy1 = y + 1 < src_height ? y + 1 : src_height - 1;
		const uint8_t *line0 = src + (size_t)sy0 * src_stride;
		const uint8_t *line1 = src + (size_t)sy1 * src_stride;
		uint8_t *out = uv_plane + (size_t)(y / 2) * dst_stride;

		for (x = 0; x < src_width; x += 2) {
			uint8_t u = (uint8_t)((line0[x * 2] + line1[x * 2] + 1) / 2);
			uint8_t v = (uint8_t)((line0[x * 2 + 2] +
						 line1[x * 2 + 2] + 1) / 2);
			out[x] = u;
			out[x + 1] = v;
		}
		for (; x < dst_width; x += 2) {
			out[x] = out[src_width - 2];
			out[x + 1] = out[src_width - 1];
		}
	}

	return 0;
}

/* Box-filter a 2x2 luma area and a 4x4 source chroma area into one half-scale
 * progressive NV12 frame.  SG2002 capture is fixed at 1080p, so this mode is
 * useful when CMA cannot sustain a full-size capture and encoder concurrently. */
static int uyvy_to_nv12_half(const uint8_t *src, unsigned int src_width,
			     unsigned int src_height, unsigned int src_stride,
			     uint8_t *dst, unsigned int dst_width,
			     unsigned int visible_height,
			     unsigned int dst_height, unsigned int dst_stride)
{
	uint8_t *y_plane = dst;
	uint8_t *uv_plane = dst + (size_t)dst_stride * dst_height;
	unsigned int y, x;

	if (!src || !dst || src_width != dst_width * 2 ||
	    src_height != visible_height * 2 || (dst_width & 1) ||
	    (visible_height & 1) || dst_height < visible_height ||
	    (dst_height & 1) || src_stride < src_width * 2 ||
	    dst_stride < dst_width)
		return -1;

	for (y = 0; y < visible_height; y++) {
		const uint8_t *line0 = src + (size_t)(y * 2) * src_stride;
		const uint8_t *line1 = line0 + src_stride;
		uint8_t *out = y_plane + (size_t)y * dst_stride;

		for (x = 0; x < dst_width; x++) {
			unsigned int sx = x * 2;
			unsigned int sum = (unsigned int)line0[sx * 2 + 1] +
				line0[sx * 2 + 3] + line1[sx * 2 + 1] +
				line1[sx * 2 + 3];

			out[x] = (uint8_t)((sum + 2) / 4);
		}
	}
	for (; y < dst_height; y++)
		memcpy(y_plane + (size_t)y * dst_stride,
		       y_plane + (size_t)(visible_height - 1) * dst_stride,
		       dst_width);

	for (y = 0; y < visible_height / 2; y++) {
		uint8_t *out = uv_plane + (size_t)y * dst_stride;
		unsigned int sy = y * 4;

		for (x = 0; x < dst_width; x += 2) {
			unsigned int sx = x * 2;
			unsigned int u = 0, v = 0, row, pair;

			for (row = 0; row < 4; row++) {
				const uint8_t *line = src +
					(size_t)(sy + row) * src_stride;

				for (pair = 0; pair < 2; pair++) {
					u += line[(sx + pair * 2) * 2];
					v += line[(sx + pair * 2) * 2 + 2];
				}
			}
			out[x] = (uint8_t)((u + 4) / 8);
			out[x + 1] = (uint8_t)((v + 4) / 8);
		}
	}
	for (; y < dst_height / 2; y++)
		memcpy(uv_plane + (size_t)y * dst_stride,
		       uv_plane + (size_t)(visible_height / 2 - 1) * dst_stride,
		       dst_width);

	return 0;
}

static void nv12_to_nv21(uint8_t *frame, unsigned int width,
			 unsigned int height, unsigned int stride)
{
	size_t luma_size = (size_t)stride * height;
	size_t chroma_size = (size_t)stride * height / 2;
	size_t i;

	(void)width;
	for (i = 0; i < chroma_size; i += 2) {
		uint8_t swap = frame[luma_size + i];
		frame[luma_size + i] = frame[luma_size + i + 1];
		frame[luma_size + i + 1] = swap;
	}
}

static int uyvy_to_nvxx_file(const char *input_path, const char *output_path,
				     unsigned int width, unsigned int height,
				     int nv21)
{
	FILE *input = NULL, *output = NULL;
	uint8_t *src = NULL, *dst = NULL;
	size_t src_size = (size_t)width * height * 2;
	size_t dst_size = (size_t)width * height * 3 / 2;
	size_t got;
	unsigned int i;
	int ret = -1;

	if ((width & 1) || (height & 1) || !width || !height)
		return fprintf(stderr, "raw dimensions must be non-zero and even\n"), -1;
	input = fopen(input_path, "rb");
	if (!input) {
		die_errno(input_path);
		goto out;
	}
	output = fopen(output_path, "wb");
	if (!output) {
		die_errno(output_path);
		goto out;
	}
	src = malloc(src_size);
	dst = malloc(dst_size);
	if (!src || !dst) {
		fprintf(stderr, "raw conversion allocation failed (%zu + %zu bytes)\n",
			src_size, dst_size);
		goto out;
	}
	got = fread(src, 1, src_size, input);
	if (got != src_size || fgetc(input) != EOF) {
		fprintf(stderr, "%s is not exactly %zu bytes\n", input_path, src_size);
		goto out;
	}
	if (uyvy_to_nv12(src, width, height, width * 2, dst, width, height,
			width))
		goto out;
	if (nv21) {
		for (i = width * height; i < dst_size; i += 2) {
			uint8_t swap = dst[i];
			dst[i] = dst[i + 1];
			dst[i + 1] = swap;
		}
	}
	if (fwrite(dst, 1, dst_size, output) != dst_size) {
		die_errno("write raw output");
		goto out;
	}
	ret = 0;
out:
	free(dst);
	free(src);
	if (output)
		fclose(output);
	if (input)
		fclose(input);
	return ret;
}

static void unmap_queue(struct mapped_queue *queue)
{
	unsigned int i;

	if (!queue->bufs)
		return;
	for (i = 0; i < queue->count; i++)
		if (queue->bufs[i].addr && queue->bufs[i].length)
			munmap(queue->bufs[i].addr, queue->bufs[i].length);
	free(queue->bufs);
	queue->bufs = NULL;
	queue->count = 0;
}

static int map_queue(int fd, enum v4l2_buf_type type, unsigned int requested,
			     struct mapped_queue *queue)
{
	struct v4l2_requestbuffers request = {
		.count = requested,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;

	if (xioctl(fd, VIDIOC_REQBUFS, &request))
		return -1;
	if (request.count < 1) {
		errno = ENOBUFS;
		return -1;
	}
	queue->bufs = calloc(request.count, sizeof(*queue->bufs));
	if (!queue->bufs)
		return -1;
	queue->count = request.count;
	for (i = 0; i < queue->count; i++) {
		struct v4l2_buffer buffer = {
			.type = type,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};
		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer))
			return -1;
		queue->bufs[i].length = buffer.length;
		queue->bufs[i].addr = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
					  MAP_SHARED, fd, buffer.m.offset);
		if (queue->bufs[i].addr == MAP_FAILED) {
			queue->bufs[i].addr = NULL;
			return -1;
		}
	}
	return 0;
}

static int queue_buffer(int fd, enum v4l2_buf_type type, unsigned int index,
			unsigned int bytesused)
{
	struct v4l2_buffer buffer = {
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
		.bytesused = bytesused,
	};
	return xioctl(fd, VIDIOC_QBUF, &buffer);
}

static int dequeue_buffer(int fd, enum v4l2_buf_type type,
				struct v4l2_buffer *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->type = type;
	buffer->memory = V4L2_MEMORY_MMAP;
	return xioctl(fd, VIDIOC_DQBUF, buffer);
}

static int stream(int fd, enum v4l2_buf_type type, int on)
{
	return xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
}

static int get_format(int fd, enum v4l2_buf_type type,
			      struct v4l2_pix_format *pix)
{
	struct v4l2_format format = { .type = type };
	if (xioctl(fd, VIDIOC_G_FMT, &format))
		return -1;
	*pix = format.fmt.pix;
	return 0;
}

static int set_encoder_format(int fd, enum v4l2_buf_type type,
				      uint32_t pixel_format, unsigned int width,
				      unsigned int height, struct v4l2_pix_format *actual)
{
	struct v4l2_format format = { .type = type };

	format.fmt.pix.width = width;
	format.fmt.pix.height = height;
	format.fmt.pix.pixelformat = pixel_format;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (pixel_format == V4L2_PIX_FMT_H264)
		format.fmt.pix.sizeimage = 1024U * 1024U;
	if (xioctl(fd, VIDIOC_S_FMT, &format))
		return -1;
	*actual = format.fmt.pix;
	return 0;
}

static int set_output_crop(int fd, unsigned int width, unsigned int height)
{
	struct v4l2_selection selection = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.target = V4L2_SEL_TGT_CROP,
		.r = {
			.width = width,
			.height = height,
		},
	};
	return xioctl(fd, VIDIOC_S_SELECTION, &selection);
}

static int live_bridge(const char *capture_path, const char *encoder_path,
		       const char *output_path, int half_scale)
{
	int capture_fd = -1, encoder_fd = -1, output_fd = -1;
	struct mapped_queue capture_queue = { 0 }, encoder_out = { 0 }, encoder_cap = { 0 };
	unsigned char *output_queued = NULL;
	struct v4l2_pix_format capture_fmt, encoder_out_fmt, encoder_cap_fmt;
	unsigned int i, free_output = 0, held_capture = UINT32_MAX;
	unsigned int visible_width, visible_height, coded_height;
	uint64_t frames = 0, encoded_frames = 0, encoded_bytes = 0;
	int capture_on = 0, encoder_out_on = 0, encoder_cap_on = 0, ret = -1;

	capture_fd = open(capture_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (capture_fd < 0) {
		die_errno(capture_path);
		goto out;
	}
	encoder_fd = open(encoder_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (encoder_fd < 0) {
		die_errno(encoder_path);
		goto out;
	}
	if (output_path) {
		if (!strcmp(output_path, "-"))
			output_fd = STDOUT_FILENO;
		else
			output_fd = open(output_path,
					 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
					 0644);
		if (output_fd < 0) {
			die_errno(output_path);
			goto out;
		}
	}
	if (get_format(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &capture_fmt))
		goto out_errno;
	if (capture_fmt.pixelformat != V4L2_PIX_FMT_UYVY ||
	    capture_fmt.width < 2 || capture_fmt.height < 2 ||
	    capture_fmt.bytesperline < capture_fmt.width * 2) {
		fprintf(stderr, "capture must provide packed UYVY with a valid stride\n");
		goto out;
	}
	if (half_scale && ((capture_fmt.width & 3) || (capture_fmt.height & 3))) {
		fprintf(stderr, "half scale requires capture dimensions divisible by four\n");
		goto out;
	}
	visible_width = half_scale ? capture_fmt.width / 2 : capture_fmt.width;
	visible_height = half_scale ? capture_fmt.height / 2 : capture_fmt.height;
	coded_height = (visible_height + 15U) & ~15U;
	/* Coda reads a macroblock surface even when the visible frame is 1080
	 * lines.  Negotiate that padded surface first, crop it to the source
	 * height, and only then configure CAPTURE.  Doing this in another order can
	 * leave the firmware with a 1920x1080 stride/height mismatch. */
	if (set_encoder_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
				       V4L2_PIX_FMT_NV21, visible_width, coded_height,
				       &encoder_out_fmt))
		goto out_errno;
	if (set_output_crop(encoder_fd, visible_width, visible_height))
		goto out_errno;
	if (get_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &encoder_out_fmt))
		goto out_errno;
	if (set_encoder_format(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
				       V4L2_PIX_FMT_H264, visible_width,
				       visible_height, &encoder_cap_fmt))
		goto out_errno;
	if ((encoder_out_fmt.width & 1) || (encoder_out_fmt.height & 1) ||
	    encoder_out_fmt.bytesperline < encoder_out_fmt.width ||
	    encoder_out_fmt.sizeimage < (size_t)encoder_out_fmt.bytesperline *
					 encoder_out_fmt.height * 3 / 2) {
		fprintf(stderr, "encoder did not return a usable NV21 output format\n");
		goto out;
	}
	if (encoder_cap_fmt.pixelformat != V4L2_PIX_FMT_H264)
		goto out;
	if (map_queue(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			      ENCODER_BUFFERS, &encoder_out))
		goto out_errno;
	output_queued = calloc(encoder_out.count, sizeof(*output_queued));
	if (!output_queued)
		goto out_errno;
	if (map_queue(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			      ENCODER_BUFFERS, &encoder_cap))
		goto out_errno;
	for (i = 0; i < encoder_cap.count; i++)
		if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out_errno;
	/* Prime Coda before allocating the two large CSI buffers.  This gives its
	 * reconstruction surfaces first claim on compacted CMA.  The black frame
	 * is also a valid reference picture for the first live frame. */
	{
		size_t luma_size = (size_t)encoder_out_fmt.bytesperline *
			encoder_out_fmt.height;
		size_t frame_size = luma_size * 3 / 2;

		if (frame_size > UINT32_MAX ||
		    frame_size > encoder_out.bufs[0].length) {
			fprintf(stderr, "encoder priming buffer is too small\n");
			goto out;
		}
		memset(encoder_out.bufs[0].addr, 16, luma_size);
		memset((uint8_t *)encoder_out.bufs[0].addr + luma_size, 128,
		       frame_size - luma_size);
		if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0,
				 (unsigned int)frame_size))
			goto out_errno;
		output_queued[0] = 1;
		free_output = encoder_out.count - 1;
	}
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	encoder_cap_on = 1;
	if (stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1))
		goto out_errno;
	encoder_out_on = 1;
	if (map_queue(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			      CAPTURE_BUFFERS, &capture_queue))
		goto out_errno;
	for (i = 0; i < capture_queue.count; i++)
		if (queue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out_errno;
	if (stream(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1))
		goto out_errno;
	capture_on = 1;
	fprintf(stderr, "bridge %s (%ux%u stride %u) -> %s NV21 (%ux%u stride %u)\n",
		capture_path, capture_fmt.width, capture_fmt.height,
		capture_fmt.bytesperline, encoder_path, encoder_out_fmt.width,
		encoder_out_fmt.height, encoder_out_fmt.bytesperline);

	while (!stop_requested) {
		struct v4l2_buffer buffer;
		int progress = 0;

		/* Drain encoded CAPTURE buffers and append the Annex-B elementary
		 * stream when the caller supplied an output path. */
		for (;;) {
			if (dequeue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					   &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= encoder_cap.count ||
			    buffer.bytesused > encoder_cap.bufs[buffer.index].length) {
				fprintf(stderr, "encoder returned an invalid capture buffer\n");
				goto out;
			}
			if (output_fd >= 0 && buffer.bytesused &&
			    write_all(output_fd, encoder_cap.bufs[buffer.index].addr,
				      buffer.bytesused))
				goto out_errno;
			encoded_frames++;
			encoded_bytes += buffer.bytesused;
			if (queue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 buffer.index, 0))
				goto out_errno;
			progress = 1;
		}
		/* Return completed OUTPUT buffers to the free pool. */
		for (;;) {
			if (dequeue_buffer(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					   &buffer)) {
				if (errno == EAGAIN)
					break;
				goto out_errno;
			}
			if (buffer.index >= encoder_out.count || !output_queued[buffer.index]) {
				fprintf(stderr, "encoder returned an unexpected output buffer\n");
				goto out;
			}
			output_queued[buffer.index] = 0;
			free_output++;
			progress = 1;
		}
		if (held_capture == UINT32_MAX) {
			if (!dequeue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					    &buffer))
				held_capture = buffer.index;
			else if (errno != EAGAIN)
				goto out_errno;
		}
		if (held_capture != UINT32_MAX && free_output) {
			struct v4l2_buffer out_buffer;
			unsigned int dst_size = encoder_out_fmt.bytesperline *
					encoder_out_fmt.height * 3 / 2;
			unsigned int source_index = held_capture;
			unsigned int output_index;
			for (output_index = 0; output_index < encoder_out.count;
			     output_index++)
				if (!output_queued[output_index])
					break;
			if (output_index == encoder_out.count ||
			    dst_size > encoder_out.bufs[output_index].length) {
				fprintf(stderr, "no usable free encoder output buffer\n");
				goto out;
			}
			if ((half_scale ?
			     uyvy_to_nv12_half(capture_queue.bufs[source_index].addr,
				capture_fmt.width, capture_fmt.height,
				capture_fmt.bytesperline,
				encoder_out.bufs[output_index].addr,
				encoder_out_fmt.width, visible_height,
				encoder_out_fmt.height,
				encoder_out_fmt.bytesperline) :
			     uyvy_to_nv12(capture_queue.bufs[source_index].addr,
				capture_fmt.width, capture_fmt.height,
				capture_fmt.bytesperline,
				encoder_out.bufs[output_index].addr,
				encoder_out_fmt.width, encoder_out_fmt.height,
				encoder_out_fmt.bytesperline))) {
				fprintf(stderr, "UYVY->NV21 conversion rejected negotiated geometry\n");
				goto out;
			}
			nv12_to_nv21(encoder_out.bufs[output_index].addr,
				      encoder_out_fmt.width, encoder_out_fmt.height,
				      encoder_out_fmt.bytesperline);
			memset(&out_buffer, 0, sizeof(out_buffer));
			out_buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			out_buffer.memory = V4L2_MEMORY_MMAP;
			out_buffer.index = output_index;
			out_buffer.bytesused = dst_size;
			if (xioctl(encoder_fd, VIDIOC_QBUF, &out_buffer))
				goto out_errno;
			output_queued[output_index] = 1;
			free_output--;
			if (queue_buffer(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 source_index, 0))
				goto out_errno;
			held_capture = UINT32_MAX;
			frames++;
			progress = 1;
		}
		if (!progress) {
			struct pollfd fds[2] = {
				{ .fd = capture_fd, .events = POLLIN },
				{ .fd = encoder_fd, .events = POLLIN | POLLOUT },
			};
			if (poll(fds, 2, 1000) < 0 && errno != EINTR)
				goto out_errno;
		}
	}
	fprintf(stderr, "stopped after %" PRIu64 " submitted frames, %" PRIu64
		" encoded frames, %" PRIu64 " encoded bytes\n",
		frames, encoded_frames, encoded_bytes);
	ret = 0;
out_errno:
	if (ret && errno)
		die_errno("live bridge");
out:
	if (capture_on)
		stream(capture_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	if (encoder_out_on)
		stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0);
	if (encoder_cap_on)
		stream(encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	unmap_queue(&encoder_cap);
	unmap_queue(&encoder_out);
	unmap_queue(&capture_queue);
	free(output_queued);
	if (encoder_fd >= 0)
		close(encoder_fd);
	if (capture_fd >= 0)
		close(capture_fd);
	if (output_fd >= 0 && output_fd != STDOUT_FILENO)
		close(output_fd);
	return ret;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [capture-node] [encoder-node] [h264-output|-] [full|half]\n"
		"       %s --raw nv12|nv21 width height input.uyvy output.raw\n",
		program, program);
}

int main(int argc, char **argv)
{
	const char *capture_path = DEFAULT_CAPTURE;
	const char *encoder_path = DEFAULT_ENCODER;
	const char *output_path = NULL;
	struct sigaction action = { .sa_handler = on_signal };
	unsigned int width, height;
	int nv21;
	int half_scale = 0;

	if (argc >= 2 && !strcmp(argv[1], "--raw")) {
		if (argc != 7 || (strcmp(argv[2], "nv12") && strcmp(argv[2], "nv21")) ||
		    parse_u32(argv[3], &width) || parse_u32(argv[4], &height)) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		nv21 = !strcmp(argv[2], "nv21");
		return uyvy_to_nvxx_file(argv[5], argv[6], width, height, nv21) ?
			EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (argc > 5) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		capture_path = argv[1];
	if (argc >= 3)
		encoder_path = argv[2];
	if (argc >= 4)
		output_path = argv[3];
	if (argc == 5) {
		if (strcmp(argv[4], "full") && strcmp(argv[4], "half")) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		half_scale = !strcmp(argv[4], "half");
	}
	if (sigemptyset(&action.sa_mask) || sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL))
		return EXIT_FAILURE;
	return live_bridge(capture_path, encoder_path, output_path, half_scale) ?
		EXIT_FAILURE : EXIT_SUCCESS;
}
