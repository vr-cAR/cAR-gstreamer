#include "gst/gst.h"

#include <k4a/k4a.h>
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TIMEOUT_IN_MS 1000

typedef struct _CustomData {
    k4a_device_t device;
    GstElement *pipeline;
    GstElement *app_source, *jpeg_dec, *queue, *omx_enc, *omx_dec, *video_convert, *video_scale, *video_out;
    GstElement *fakesink, *app_sink, *rtp_sink, *udp_sink, *nv_sink;
    GstElement *rtp_pay;
    GstElement *h264_parse, *h265_parse;
    guint sourceid;
    GMainLoop *main_loop;
} CustomData;

static gboolean push_data(CustomData *data) {
    g_print("Start of push_data\n");
    GstClock *clock = gst_system_clock_obtain();
    g_print("First start %lu\n", gst_clock_get_time(clock) / 1000000);
    // gst_clock_unref(clock);
    GstBuffer *buffer;
    GstFlowReturn ret;
    int i;
    GstMapInfo map;
    gint16 *raw;
    gfloat freq;

    k4a_device_t device = data->device;
    k4a_capture_t capture = NULL;

    // Get capture
    k4a_image_t image;
    size_t img_size;
    switch (k4a_device_get_capture(device, &capture, TIMEOUT_IN_MS))
    {
        case K4A_WAIT_RESULT_SUCCEEDED:
            break;
        case K4A_WAIT_RESULT_TIMEOUT:
            g_print("Timed out waiting for a capture\n");
            break;
        case K4A_WAIT_RESULT_FAILED:
            g_print("Failed to read a capture\n");
            break;
            // goto Exit;
        default:
            g_print("Shouldn't be here\n");
        

    }
    // Get frame from capture
    image = k4a_capture_get_color_image(capture);
    if (!image) {
        g_print("Failed to get color image\n");
        return TRUE;
    }
    img_size = k4a_image_get_size(image);
    // g_print("Sending frame data!\n");
    // g_print("Image size is %lu\n", img_size);

    buffer = gst_buffer_new_and_alloc (img_size);

    /* Set its timestamp and duration */
    // GST_BUFFER_TIMESTAMP (buffer) = gst_util_uint64_scale (data->num_samples, GST_SECOND, SAMPLE_RATE);
    // GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale (num_samples, GST_SECOND, SAMPLE_RATE);

    gst_buffer_map (buffer, &map, GST_MAP_WRITE);
    uint8_t* img_buf = k4a_image_get_buffer(image);
    memcpy(map.data, img_buf, img_size);

    gst_buffer_unmap(buffer, &map);
    g_signal_emit_by_name(data->app_source, "push-buffer", buffer, &ret);

    /* Free the buffer now that we are done with it */
    gst_buffer_unref (buffer);

    // Free k4a image
    k4a_image_release(image);
    k4a_capture_release(capture);

    if (ret != GST_FLOW_OK) {
        /* We got some error, stop sending data */
        return FALSE;
    }
    return TRUE;
}

static gboolean get_latency(CustomData *data) {
    GstQuery *latency_query = gst_query_new_latency();
    if (latency_query == NULL) {
        g_print("Cannot create latency query\n");
    }
    gboolean res = gst_element_query(data->udp_sink, latency_query);
    if (res) {
        gboolean live;
        GstClockTime min_lat, max_lat;
        gst_query_parse_latency(latency_query, &live, &min_lat, &max_lat);
        g_print("Is live: %d\n", live);
        g_print("Min latency: %lu\n", min_lat);
        g_print("Max latency: %lu\n", max_lat);
        gst_query_unref(latency_query);
    }
    else {
        g_print("Can't make latency query...\n");
    }

    return TRUE;
}

static GstFlowReturn new_sample (GstElement *sink, CustomData *data) {
  GstSample *sample;
//   GstStructure *info;
  GstBuffer *buffer;

  /* Retrieve the buffer */
  g_signal_emit_by_name (sink, "pull-sample", &sample);

  GstClock *clock = gst_system_clock_obtain();
  g_print("********************************Second sample arrived %lu\n", gst_clock_get_time(clock) / 1000000);
//   gst_clock_unref(clock);
  if (sample) {
    /* The only thing we do in this example is print a * to indicate a received buffer */
    g_print ("*");
    // info = gst_sample_get_info(sample);
    buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        g_printerr("THERE IS NO BUFFER AHHHHHHHHHHHHH\n");
    }
    g_print("The buffer is of size %lu\n", gst_buffer_get_size(buffer));
    gst_sample_unref (sample);
    return GST_FLOW_OK;
  }

  return GST_FLOW_ERROR;
}


static void start_feed(GstElement *source, guint size, CustomData *data) {
    if (data->sourceid == 0) {
        g_print("Start feeding\n");
        data->sourceid = g_idle_add((GSourceFunc) push_data, data);
    }
}

static void stop_feed(GstElement *source, CustomData *data) {
    if (data->sourceid != 0) {
        g_print("Stop feeding\n");
        g_source_remove(data->sourceid);
        data->sourceid = 0;
    }
}

static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
    GError *err;
    gchar *debug_info;

    gst_message_parse_error(msg, &err, &debug_info);
    g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
    g_printerr("Debugging information: %s\n", debug_info ? debug_info: "none");
    g_clear_error(&err);
    g_free(debug_info);

    g_main_loop_quit(data->main_loop);
}

int main(int argc, char* argv[]){
    int returnCode = 1;
    k4a_device_t device = NULL;
    // const int32_t TIMEOUT_IN_MS = 1000;
    int captureFrameCount;
    k4a_capture_t capture = NULL;

    // if (argc < 2)
    // {
    //     printf("%s FRAMECOUNT\n", argv[0]);
    //     printf("Capture FRAMECOUNT color and depth frames from the device using the separate get frame APIs\n");
    //     returnCode = 2;
    //     goto Exit;
    // }

    // captureFrameCount = atoi(argv[1]);
    // printf("Capturing %d frames\n", captureFrameCount);

    uint32_t device_count = k4a_device_get_installed_count();

    if (device_count == 0)
    {
        printf("No K4A devices found\n");
        return 0;
    }

    if (K4A_RESULT_SUCCEEDED != k4a_device_open(K4A_DEVICE_DEFAULT, &device))
    {
        printf("Failed to open device\n");
        // goto Exit;
    }

    k4a_device_configuration_t config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
    config.color_format = K4A_IMAGE_FORMAT_COLOR_YUY2; // K4A_IMAGE_FORMAT_COLOR_MJPG;
    config.color_resolution = K4A_COLOR_RESOLUTION_720P; // K4A_COLOR_RESOLUTION_2160P;
    config.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;
    config.camera_fps = K4A_FRAMES_PER_SECOND_30;

    if (K4A_RESULT_SUCCEEDED != k4a_device_start_cameras(device, &config))
    {
        printf("Failed to start device\n");
        // goto Exit;
    }
    //**************************************************************************************************************
    // Setup GStreamer to use k4adevice
    gst_init(&argc, &argv);
    CustomData data;
    GstBus *bus;
    
    memset (&data, 0, sizeof(data));

    //Kinect thing
    data.device = device;
    data.pipeline = gst_pipeline_new("test-pipeline");
    data.app_source = gst_element_factory_make("appsrc", "video_source");
    data.jpeg_dec = gst_element_factory_make("jpegdec", "jpegdec");
    data.queue = gst_element_factory_make("queue", "queue");
    data.omx_enc = gst_element_factory_make("omxh264enc", "encoder");
    data.omx_dec = gst_element_factory_make("omxh264dec", "decoder");
    data.video_convert = gst_element_factory_make("videoconvert", "video_convert");
    data.video_scale = gst_element_factory_make("videoscale", "video_scale");
    data.video_out = gst_element_factory_make("autovideosink", "sink");
    data.fakesink = gst_element_factory_make("fakesink", "fakesink");
    data.app_sink = gst_element_factory_make("appsink", "appsink");
    data.rtp_sink = gst_element_factory_make("rtpsink", "rtpsink");
    data.nv_sink = gst_element_factory_make("nveglglessink", "nveglglessink");


    data.h264_parse = gst_element_factory_make("h264parse", "h264_parse");
    data.h265_parse = gst_element_factory_make("h265parse", "h265parse");
    data.rtp_pay = gst_element_factory_make("rtph264pay", "rtph264pay");
    data.udp_sink = gst_element_factory_make("udpsink", "udpsink");

    GstElement *nvjpeg = gst_element_factory_make("nvjpegdec", "nvjpegdec");
    GstElement *jpeg_parse = gst_element_factory_make("jpegformat", "jpegformat");
    GstElement *mjpeg_dec = gst_element_factory_make("avdec_mjpeg", "avdec_mjpeg");
    GstElement *video_raw = gst_element_factory_make("video/x-raw", "video/x-raw");
    if (!video_raw) {
        g_print("NO video/x-raw AVAILABLE\n");
    }

    // Caps negotiation for the app_source
    GstCaps *vcaps = gst_caps_new_simple("video/x-raw",
    "format", G_TYPE_STRING, "YUY2",
    "width", G_TYPE_INT, 1280,
    "height", G_TYPE_INT, 720,
    "framerate", GST_TYPE_FRACTION, 30, 1,
    "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
    NULL);
    g_object_set(data.app_source, "caps", vcaps, "is-live", TRUE, NULL);

    // GstElement *video = gst_element_factory_make("")

    g_object_set (data.app_sink, "emit-signals", TRUE, NULL);
    g_signal_connect (data.app_sink, "new-sample", G_CALLBACK (new_sample), &data);

    // Flags for appsrc
    // g_object_set(data.app_source, "is-live", TRUE, NULL);

    // Flags for encoder
    // g_object_set(data.omx_enc, "latency-mode", 1, NULL);
    // g_object_set(data.omx_enc, "insert-vui", TRUE, NULL);

    //Flags for rtp
    // g_object_set(data.rtp_sink, "uri", "rtp://0.0.0.0:5004", NULL);

    g_object_set(data.h265_parse, "config-interval", -1, NULL);

    g_object_set(data.rtp_pay, "config-interval", -1, NULL);

    // Currently using lovey.cs.utexas.edu
    // g_object_set(data.udp_sink, "host", "128.83.139.40", "port", 5004, NULL);
    g_object_set(data.udp_sink, "host", "10.159.65.5", "port", 5678, NULL);

    if (!data.pipeline || !data.app_source || !data.jpeg_dec || !data.queue || !data.omx_enc || !data.video_convert ||
        !data.video_scale || !data.video_out || !data.fakesink || !data.app_sink || !data.h265_parse || !data.udp_sink){
        g_printerr("Can't make some elements\n");
        return -1;
    }

    if (!data.rtp_sink) {
        g_printerr("Can't make rtp_sink\n");
    }
    g_print("Finished making custom data\n");

    g_signal_connect(data.app_source, "need-data", G_CALLBACK(start_feed), &data);
    g_signal_connect(data.app_source, "enough-data", G_CALLBACK(stop_feed), &data);
    // gst_bin_add_many (GST_BIN (data.pipeline), data.app_source, data.jpeg_dec, data.omx_enc, data.video_out, NULL);
    gst_bin_add_many(GST_BIN(data.pipeline), data.app_source, data.omx_enc, data.h264_parse, data.omx_dec, data.video_convert, data.video_out, NULL); //data.omx_enc, data.h264_parse, data.omx_dec, data.video_scale, data.video_out, NULL);
    if (gst_element_link_many(data.app_source, data.omx_enc, data.h264_parse, data.omx_dec, data.video_convert, data.video_out, NULL) != TRUE){ // data.omx_enc, data.h264_parse, data.omx_dec, data.video_scale, data.video_out, NULL) != TRUE) {
        g_printerr ("Elements could not be linked.\n");
        gst_object_unref (data.pipeline);
        return -1;
    }

    g_print("Finished linking components\n");
    
    bus = gst_element_get_bus(data.pipeline);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(G_OBJECT(bus), "message::error", (GCallback)error_cb, &data);
    gst_object_unref(bus);


    // g_idle_add((GSourceFunc) get_latency, &data);

    gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    data.main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(data.main_loop);


    sleep(10);

    g_print("Exiting\n");
    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);


    return 0;

    // while (captureFrameCount-- > 0)
    // {
    //     k4a_image_t image;
    //     // Get a depth frame
    //     switch (k4a_device_get_capture(device, &capture, TIMEOUT_IN_MS))
    //     {
    //     case K4A_WAIT_RESULT_SUCCEEDED:
    //         break;
    //     case K4A_WAIT_RESULT_TIMEOUT:
    //         printf("Timed out waiting for a capture\n");
    //         continue;
    //         break;
    //     case K4A_WAIT_RESULT_FAILED:
    //         printf("Failed to read a capture\n");
    //         // goto Exit;
    //     }

    //     printf("Capture");

    //     // Probe for a color image
    //     image = k4a_capture_get_color_image(capture);
    //     if (image)
    //     {
    //         printf(" | Color res:%4dx%4d stride:%5d ",
    //                k4a_image_get_height_pixels(image),
    //                k4a_image_get_width_pixels(image),
    //                k4a_image_get_stride_bytes(image));
    //         k4a_image_release(image);
    //     }
    //     else
    //     {
    //         printf(" | Color None                       ");
    //     }

    //     // probe for a IR16 image
    //     image = k4a_capture_get_ir_image(capture);
    //     if (image != NULL)
    //     {
    //         printf(" | Ir16 res:%4dx%4d stride:%5d ",
    //                k4a_image_get_height_pixels(image),
    //                k4a_image_get_width_pixels(image),
    //                k4a_image_get_stride_bytes(image));
    //         k4a_image_release(image);
    //     }
    //     else
    //     {
    //         printf(" | Ir16 None                       ");
    //     }

    //     // Probe for a depth16 image
    //     image = k4a_capture_get_depth_image(capture);
    //     if (image != NULL)
    //     {
    //         printf(" | Depth16 res:%4dx%4d stride:%5d\n",
    //                k4a_image_get_height_pixels(image),
    //                k4a_image_get_width_pixels(image),
    //                k4a_image_get_stride_bytes(image));
    //         k4a_image_release(image);
    //     }
    //     else
    //     {
    //         printf(" | Depth16 None\n");
    //     }

    //     // release capture
    //     k4a_capture_release(capture);
    //     fflush(stdout);
    // }

    // returnCode = 0;

    // return returnCode;

    
}
