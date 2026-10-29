#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

typedef enum {
    MODE_FILL,  // Zoom & crop to fill screen
    MODE_SCALE  // Fit entire video inside screen (letterbox)
} RenderMode;

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [OPTIONS] <path_to_video>\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --fill     Zoom video to fill the whole screen (default)\n");
    fprintf(stderr, "  --scale    Scale video to fit inside screen (letterbox/pillarbox)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *video_path = NULL;
    RenderMode mode = MODE_FILL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0) {
            mode = MODE_SCALE;
        } else if (strcmp(argv[i], "--fill") == 0) {
            mode = MODE_FILL;
        } else if (argv[i][0] != '-') {
            video_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!video_path) {
        fprintf(stderr, "Error: No video file path provided.\n");
        print_usage(argv[0]);
        return 1;
    }

    // 1. FFmpeg Initialization
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, video_path, NULL, NULL) < 0) {
        fprintf(stderr, "Error: Could not open video file: %s\n", video_path);
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Error: Could not find stream information.\n");
        return 1;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        fprintf(stderr, "Error: Could not find video stream.\n");
        return 1;
    }

    AVCodecParameters *codec_par = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        fprintf(stderr, "Error: Unsupported codec.\n");
        return 1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codec_par);
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Error: Could not open codec.\n");
        return 1;
    }

    // 2. X11 Setup
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Error: Cannot open X display.\n");
        return 1;
    }

    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(display, screen);

    Window window = XCreateWindow(
        display, RootWindow(display, screen),
                                  0, 0, screen_width, screen_height, 0,
                                  CopyFromParent, InputOutput, CopyFromParent,
                                  CWOverrideRedirect | CWBackPixel, &attrs
    );

    Atom wm_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom wm_type_desktop = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(display, window, wm_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wm_type_desktop, 1);

    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom wm_state_below = XInternAtom(display, "_NET_WM_STATE_BELOW", False);
    XChangeProperty(display, window, wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wm_state_below, 1);

    XMapWindow(display, window);
    XLowerWindow(display, window);
    XFlush(display);

    GC gc = XCreateGC(display, window, 0, NULL);

    // 3. Aspect Ratio & Dimension Calculations
    double video_aspect = (double)codec_ctx->width / (double)codec_ctx->height;
    double screen_aspect = (double)screen_width / (double)screen_height;

    int render_w = screen_width;
    int render_h = screen_height;

    if (mode == MODE_FILL) {
        // Zoom to fill entire screen
        if (screen_aspect > video_aspect) {
            render_h = (int)(screen_width / video_aspect);
        } else {
            render_w = (int)(screen_height * video_aspect);
        }
    } else if (mode == MODE_SCALE) {
        // Fit inside screen (Letterbox/Pillarbox)
        if (screen_aspect > video_aspect) {
            render_w = (int)(screen_height * video_aspect);
        } else {
            render_h = (int)(screen_width / video_aspect);
        }
    }

    int offset_x = (screen_width - render_w) / 2;
    int offset_y = (screen_height - render_h) / 2;

    struct SwsContext *sws_ctx = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        render_w, render_h, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    AVFrame *frame = av_frame_alloc();
    AVFrame *frame_rgb = av_frame_alloc();

    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_BGRA, render_w, render_h, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(num_bytes * sizeof(uint8_t));
    av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, buffer,
                         AV_PIX_FMT_BGRA, render_w, render_h, 1);

    AVPacket packet;

    // 4. Playback Loop
    while (1) {
        if (av_read_frame(fmt_ctx, &packet) >= 0) {
            if (packet.stream_index == video_stream_idx) {
                if (avcodec_send_packet(codec_ctx, &packet) == 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                        sws_scale(sws_ctx, (uint8_t const * const *)frame->data,
                                  frame->linesize, 0, codec_ctx->height,
                                  frame_rgb->data, frame_rgb->linesize);

                        XImage *ximage = XCreateImage(
                            display, DefaultVisual(display, screen),
                                                      DefaultDepth(display, screen), ZPixmap, 0,
                                                      (char *)frame_rgb->data[0], render_w, render_h,
                                                      32, 0
                        );

                        XPutImage(display, window, gc, ximage, 0, 0, offset_x, offset_y, render_w, render_h);
                        XFlush(display);

                        ximage->data = NULL;
                        XFree(ximage);

                        usleep(33000); // ~30 FPS
                    }
                }
            }
            av_packet_unref(&packet);
        } else {
            // Loop video
            av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
            avcodec_flush_buffers(codec_ctx);
        }
    }

    // Cleanup
    av_free(buffer);
    av_frame_free(&frame_rgb);
    av_frame_free(&frame);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}
