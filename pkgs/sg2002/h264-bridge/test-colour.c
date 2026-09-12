/* Exercise the real format helpers without video hardware. */
#define ioctl test_ioctl
#define main bridge_program_main
#include "sg2002-h264-bridge.c"
#undef main
#undef ioctl
#include <assert.h>
#include <stdarg.h>

static struct v4l2_format requested;
static struct v4l2_pix_format supplied;

int test_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	struct v4l2_format *format;

	assert(fd == 42);
	va_start(args, request);
	format = va_arg(args, struct v4l2_format *);
	va_end(args);
	requested = *format;
	assert(format->fmt.pix.priv == V4L2_PIX_FMT_PRIV_MAGIC);
	if (request == VIDIOC_G_FMT)
		format->fmt.pix = supplied;
	else
		assert(request == VIDIOC_S_FMT);
	return 0;
}

static void check_color(const struct v4l2_pix_format *actual,
			const struct v4l2_pix_format *expected)
{
	assert(actual->colorspace == expected->colorspace);
	assert(actual->xfer_func == expected->xfer_func);
	assert(actual->ycbcr_enc == expected->ycbcr_enc);
	assert(actual->quantization == expected->quantization);
}

int main(void)
{
	const struct v4l2_pix_format colors[] = {
		{ .colorspace = V4L2_COLORSPACE_SRGB,
		  .xfer_func = V4L2_XFER_FUNC_NONE,
		  .ycbcr_enc = V4L2_YCBCR_ENC_601,
		  .quantization = V4L2_QUANTIZATION_FULL_RANGE },
		{ .colorspace = V4L2_COLORSPACE_REC709,
		  .xfer_func = V4L2_XFER_FUNC_709,
		  .ycbcr_enc = V4L2_YCBCR_ENC_709,
		  .quantization = V4L2_QUANTIZATION_LIM_RANGE },
	};
	struct v4l2_pix_format actual;
	size_t i;

	for (i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
		assert(!set_video_format(42, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			V4L2_PIX_FMT_NV12, 640, 368, &colors[i], &actual));
		assert(requested.type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
		assert(actual.width == 640 && actual.height == 368);
		assert(actual.pixelformat == V4L2_PIX_FMT_NV12);
		check_color(&actual, &colors[i]);
		assert(!set_video_format(42, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			V4L2_PIX_FMT_H264, 640, 360, &colors[i], &actual));
		assert(actual.sizeimage == 1024U * 1024U);
		check_color(&actual, &colors[i]);
		supplied = actual;
		assert(!get_format(42, V4L2_BUF_TYPE_VIDEO_CAPTURE, &actual));
		check_color(&actual, &colors[i]);
	}
	assert(!set_encoder_format(42, V4L2_BUF_TYPE_VIDEO_OUTPUT,
		V4L2_PIX_FMT_NV12, 640, 368, &actual));
	assert(actual.colorspace == V4L2_COLORSPACE_DEFAULT);
	assert(actual.quantization == V4L2_QUANTIZATION_DEFAULT);
	assert(live_encoded_frames(0) == 0);
	assert(live_encoded_frames(1) == 0);
	assert(live_encoded_frames(301) == 300);
	assert(!live_frame_limit_reached(0, UINT64_MAX));
	assert(!live_frame_limit_reached(1, 1));
	assert(live_frame_limit_reached(1, 2));
	assert(!live_frame_limit_reached(300, 300));
	assert(live_frame_limit_reached(300, 301));
	assert(!live_frame_limit_reached(UINT32_MAX, UINT32_MAX));
	assert(live_frame_limit_reached(UINT32_MAX, (uint64_t)UINT32_MAX + 1));
	assert(report_live_frames(300, 300) == -1 && errno == ECANCELED);
	assert(!report_live_frames(300, 301));
	assert(!report_live_frames(0, 0));
	puts("bridge colour format helpers: full601/linear, limited709, defaults OK");
	return 0;
}
