/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>

#include <inttypes.h>

#include "gstmpeg2dec.h"

/* mpeg2dec changed a struct name after 0.3.1, here's a workaround */
/* mpeg2dec also only defined MPEG2_RELEASE after 0.3.1
   #if MPEG2_RELEASE < MPEG2_VERSION(0,3,2)
*/
#ifndef MPEG2_RELEASE
#define MPEG2_VERSION(a,b,c) ((((a)&0xff)<<16)|(((b)&0xff)<<8)|((c)&0xff))
#define MPEG2_RELEASE MPEG2_VERSION(0,3,1)
typedef picture_t mpeg2_picture_t;
typedef gint mpeg2_state_t;

#define STATE_BUFFER 0
#endif

GST_DEBUG_CATEGORY_STATIC (mpeg2dec_debug);
#define GST_CAT_DEFAULT (mpeg2dec_debug)

/* table with framerates expressed as fractions */
static gdouble fpss[] = { 24.0 / 1.001, 24.0, 25.0,
  30.0 / 1.001, 30.0, 50.0,
  60.0 / 1.001, 60.0, 0
};

/* frame periods */
static guint frame_periods[] = {
  1126125, 1125000, 1080000, 900900, 900000, 540000, 450450, 450000, 0
};

/* elementfactory information */
static GstElementDetails gst_mpeg2dec_details = {
  "mpeg1 and mpeg2 video decoder",
  "Codec/Decoder/Video",
  "Uses libmpeg2 to decode MPEG video streams",
  "Wim Taymans <wim.taymans@chello.be>",
};

/* Mpeg2dec signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0
      /* FILL ME */
};

/*
 * We can't use fractions in static pad templates, so
 * we do something manual...
 */
static GstPadTemplate *
src_templ (void)
{
  static GstPadTemplate *templ = NULL;

  if (!templ) {
    GstCaps *caps;
    GstStructure *structure;
    GValue list = { 0 }
    , fps = {
    0}
    , fmt = {
    0};
    char *fmts[] = { "YV12", "I420", "Y42B", NULL };
    guint n;

    caps = gst_caps_new_simple ("video/x-raw-yuv",
        "format", GST_TYPE_FOURCC,
        GST_MAKE_FOURCC ('I', '4', '2', '0'),
        "width", GST_TYPE_INT_RANGE, 16, 4096,
        "height", GST_TYPE_INT_RANGE, 16, 4096, NULL);

    structure = gst_caps_get_structure (caps, 0);

    g_value_init (&list, GST_TYPE_LIST);
    g_value_init (&fps, G_TYPE_DOUBLE);
    for (n = 0; fpss[n] != 0; n++) {
      g_value_set_double (&fps, fpss[n]);
      gst_value_list_append_value (&list, &fps);
    }
    gst_structure_set_value (structure, "framerate", &list);
    g_value_unset (&list);
    g_value_unset (&fps);

    g_value_init (&list, GST_TYPE_LIST);
    g_value_init (&fmt, GST_TYPE_FOURCC);
    for (n = 0; fmts[n] != NULL; n++) {
      gst_value_set_fourcc (&fmt, GST_STR_FOURCC (fmts[n]));
      gst_value_list_append_value (&list, &fmt);
    }
    gst_structure_set_value (structure, "format", &list);
    g_value_unset (&list);
    g_value_unset (&fmt);

    templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps);
  }
  return templ;
}

#ifdef enable_user_data
static GstStaticPadTemplate user_data_template_factory =
GST_STATIC_PAD_TEMPLATE ("user_data",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);
#endif

static GstStaticPadTemplate sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpeg, "
        "mpegversion = (int) [ 1, 2 ], " "systemstream = (boolean) false")
    );

static void gst_mpeg2dec_base_init (gpointer g_class);
static void gst_mpeg2dec_class_init (GstMpeg2decClass * klass);
static void gst_mpeg2dec_init (GstMpeg2dec * mpeg2dec);

static void gst_mpeg2dec_dispose (GObject * object);
static void gst_mpeg2dec_reset (GstMpeg2dec * mpeg2dec);

static void gst_mpeg2dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_mpeg2dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_mpeg2dec_set_index (GstElement * element, GstIndex * index);
static GstIndex *gst_mpeg2dec_get_index (GstElement * element);

static gboolean gst_mpeg2dec_src_event (GstPad * pad, GstEvent * event);
static const GstQueryType *gst_mpeg2dec_get_src_query_types (GstPad * pad);

static gboolean gst_mpeg2dec_src_query (GstPad * pad, GstQuery * query);

static gboolean gst_mpeg2dec_sink_convert (GstPad * pad, GstFormat src_format,
    gint64 src_value, GstFormat * dest_format, gint64 * dest_value);
static gboolean gst_mpeg2dec_src_convert (GstPad * pad, GstFormat src_format,
    gint64 src_value, GstFormat * dest_format, gint64 * dest_value);

static GstStateChangeReturn gst_mpeg2dec_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_mpeg2dec_sink_event (GstPad * pad, GstEvent * event);
static GstFlowReturn gst_mpeg2dec_chain (GstPad * pad, GstBuffer * buf);

//static gboolean gst_mpeg2dec_sink_query (GstPad * pad, GstQuery * query);
static GstCaps *gst_mpeg2dec_src_getcaps (GstPad * pad);

#if 0
static const GstFormat *gst_mpeg2dec_get_formats (GstPad * pad);
#endif

#if 0
static const GstEventMask *gst_mpeg2dec_get_event_masks (GstPad * pad);
#endif

static GstElementClass *parent_class = NULL;

static GstBuffer *crop_buffer (GstMpeg2dec * mpeg2dec, GstBuffer * input);

/*static guint gst_mpeg2dec_signals[LAST_SIGNAL] = { 0 };*/

GType
gst_mpeg2dec_get_type (void)
{
  static GType mpeg2dec_type = 0;

  if (!mpeg2dec_type) {
    static const GTypeInfo mpeg2dec_info = {
      sizeof (GstMpeg2decClass),
      gst_mpeg2dec_base_init,
      NULL,
      (GClassInitFunc) gst_mpeg2dec_class_init,
      NULL,
      NULL,
      sizeof (GstMpeg2dec),
      0,
      (GInstanceInitFunc) gst_mpeg2dec_init,
    };

    mpeg2dec_type =
        g_type_register_static (GST_TYPE_ELEMENT, "GstMpeg2dec", &mpeg2dec_info,
        0);
  }

  GST_DEBUG_CATEGORY_INIT (mpeg2dec_debug, "mpeg2dec", 0,
      "MPEG2 decoder element");

  return mpeg2dec_type;
}

static void
gst_mpeg2dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class, src_templ ());
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template_factory));
#ifdef enable_user_data
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&user_data_template_factory));
#endif
  gst_element_class_set_details (element_class, &gst_mpeg2dec_details);
}

static void
gst_mpeg2dec_class_init (GstMpeg2decClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  parent_class = g_type_class_ref (GST_TYPE_ELEMENT);

  gobject_class->set_property = gst_mpeg2dec_set_property;
  gobject_class->get_property = gst_mpeg2dec_get_property;
  gobject_class->dispose = gst_mpeg2dec_dispose;

  gstelement_class->change_state = gst_mpeg2dec_change_state;
  gstelement_class->set_index = gst_mpeg2dec_set_index;
  gstelement_class->get_index = gst_mpeg2dec_get_index;
}

static void
gst_mpeg2dec_init (GstMpeg2dec * mpeg2dec)
{
  /* create the sink and src pads */
  mpeg2dec->sinkpad =
      gst_pad_new_from_template (gst_static_pad_template_get
      (&sink_template_factory), "sink");
  gst_element_add_pad (GST_ELEMENT (mpeg2dec), mpeg2dec->sinkpad);
  gst_pad_set_chain_function (mpeg2dec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_mpeg2dec_chain));
  //gst_pad_set_query_function (mpeg2dec->sinkpad,
  //                            GST_DEBUG_FUNCPTR (gst_mpeg2dec_get_sink_query));
  gst_pad_set_event_function (mpeg2dec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_mpeg2dec_sink_event));

  mpeg2dec->srcpad = gst_pad_new_from_template (src_templ (), "src");
  gst_element_add_pad (GST_ELEMENT (mpeg2dec), mpeg2dec->srcpad);
  gst_pad_set_getcaps_function (mpeg2dec->srcpad,
      GST_DEBUG_FUNCPTR (gst_mpeg2dec_src_getcaps));
  gst_pad_set_event_function (mpeg2dec->srcpad,
      GST_DEBUG_FUNCPTR (gst_mpeg2dec_src_event));
  gst_pad_set_query_type_function (mpeg2dec->srcpad,
      GST_DEBUG_FUNCPTR (gst_mpeg2dec_get_src_query_types));
  gst_pad_set_query_function (mpeg2dec->srcpad,
      GST_DEBUG_FUNCPTR (gst_mpeg2dec_src_query));

#ifdef enable_user_data
  mpeg2dec->userdatapad =
      gst_pad_new_from_template (gst_static_pad_template_get
      (&user_data_template_factory), "user_data");
  gst_element_add_pad (GST_ELEMENT (mpeg2dec), mpeg2dec->userdatapad);
#endif

  /* initialize the mpeg2dec acceleration */
}

static void
gst_mpeg2dec_dispose (GObject * object)
{
  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (object);

  if (mpeg2dec->decoder) {
    GST_DEBUG_OBJECT (mpeg2dec, "closing decoder");
    mpeg2_close (mpeg2dec->decoder);
    mpeg2dec->decoder = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_mpeg2dec_reset (GstMpeg2dec * mpeg2dec)
{
  /* reset the initial video state */
  mpeg2dec->format = MPEG2DEC_FORMAT_NONE;
  mpeg2dec->width = -1;
  mpeg2dec->height = -1;
  mpeg2dec->segment_start = 0;
  mpeg2dec->segment_end = -1;
  mpeg2dec->discont_state = MPEG2DEC_DISC_NEW_PICTURE;
  mpeg2dec->frame_period = 0;
  mpeg2dec->need_sequence = TRUE;
  mpeg2dec->next_time = 0;
  mpeg2dec->offset = 0;
  mpeg2_reset (mpeg2dec->decoder, 1);
}

static void
gst_mpeg2dec_set_index (GstElement * element, GstIndex * index)
{
  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (element);

  mpeg2dec->index = index;

  gst_index_get_writer_id (index, GST_OBJECT (element), &mpeg2dec->index_id);
}

static GstIndex *
gst_mpeg2dec_get_index (GstElement * element)
{
  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (element);

  return mpeg2dec->index;
}

static GstBuffer *
crop_buffer (GstMpeg2dec * mpeg2dec, GstBuffer * input)
{
  unsigned char *in_data;
  unsigned char *out_data;
  unsigned int h_subsample;
  unsigned int v_subsample;
  unsigned int line;
  GstBuffer *outbuf = input;

  /*We crop only if the target region is smaller than the input one */
  if ((mpeg2dec->decoded_width > mpeg2dec->width) ||
      (mpeg2dec->decoded_height > mpeg2dec->height)) {
    /* If we don't know about the format, we just return the original
     * buffer.
     */
    if (mpeg2dec->format == MPEG2DEC_FORMAT_I422 ||
        mpeg2dec->format == MPEG2DEC_FORMAT_I420 ||
        mpeg2dec->format == MPEG2DEC_FORMAT_YV12) {
      /*FIXME:  I have tried to use gst_buffer_copy_on_write, but it
       *        still have some artifact, so I'me allocating new buffer
       *        for each frame decoded...
       */
      if (mpeg2dec->format == MPEG2DEC_FORMAT_I422) {
        outbuf =
            gst_buffer_new_and_alloc (mpeg2dec->width * mpeg2dec->height * 2);
        h_subsample = 2;
        v_subsample = 1;
      } else {
        outbuf =
            gst_buffer_new_and_alloc (mpeg2dec->width * mpeg2dec->height * 1.5);
        h_subsample = 2;
        v_subsample = 2;
      }

      GST_BUFFER_TIMESTAMP (outbuf) = GST_BUFFER_TIMESTAMP (input);
      GST_BUFFER_OFFSET (outbuf) = GST_BUFFER_OFFSET (input);
      GST_BUFFER_DURATION (outbuf) = GST_BUFFER_DURATION (input);

      /* Copy Y first */
      in_data = GST_BUFFER_DATA (input);
      out_data = GST_BUFFER_DATA (outbuf);
      for (line = 0; line < mpeg2dec->height; line++) {
        memcpy (out_data, in_data, mpeg2dec->width);
        out_data += mpeg2dec->width;
        in_data += mpeg2dec->decoded_width;
      }

      /* Now copy U & V */
      in_data =
          GST_BUFFER_DATA (input) +
          mpeg2dec->decoded_width * mpeg2dec->decoded_height;
      for (line = 0; line < mpeg2dec->height / v_subsample; line++) {
        memcpy (out_data, in_data, mpeg2dec->width / h_subsample);
        memcpy (out_data +
            mpeg2dec->width * mpeg2dec->height / (v_subsample * h_subsample),
            in_data +
            mpeg2dec->decoded_width * mpeg2dec->decoded_height / (v_subsample *
                h_subsample), mpeg2dec->width / h_subsample);
        out_data += mpeg2dec->width / h_subsample;
        in_data += mpeg2dec->decoded_width / h_subsample;
      }

      gst_buffer_unref (input);
    }
  }

  return outbuf;
}

static GstFlowReturn
gst_mpeg2dec_alloc_buffer (GstMpeg2dec * mpeg2dec, gint64 offset,
    GstBuffer ** obuf)
{
  GstBuffer *outbuf = NULL;
  gint size = mpeg2dec->decoded_width * mpeg2dec->decoded_height;
  guint8 *buf[3], *out = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  if (mpeg2dec->format == MPEG2DEC_FORMAT_I422) {
    ret =
        gst_pad_alloc_buffer (mpeg2dec->srcpad, GST_BUFFER_OFFSET_NONE,
        size * 2, GST_PAD_CAPS (mpeg2dec->srcpad), &outbuf);
    if (ret != GST_FLOW_OK)
      goto no_buffer;

    out = GST_BUFFER_DATA (outbuf);

    buf[0] = out;
    buf[1] = buf[0] + size;
    buf[2] = buf[1] + size / 2;

  } else {
    ret =
        gst_pad_alloc_buffer (mpeg2dec->srcpad, GST_BUFFER_OFFSET_NONE,
        (size * 3) / 2, GST_PAD_CAPS (mpeg2dec->srcpad), &outbuf);
    if (ret != GST_FLOW_OK)
      goto no_buffer;

    out = GST_BUFFER_DATA (outbuf);

    buf[0] = out;
    if (mpeg2dec->format == MPEG2DEC_FORMAT_I420) {
      buf[0] = out;
      buf[1] = buf[0] + size;
      buf[2] = buf[1] + size / 4;
    } else {
      buf[0] = out;
      buf[2] = buf[0] + size;
      buf[1] = buf[2] + size / 4;
    }
  }

  mpeg2_set_buf (mpeg2dec->decoder, buf, outbuf);

  /* we store the original byteoffset of this picture in the stream here
   * because we need it for indexing */
  GST_BUFFER_OFFSET (outbuf) = offset;

done:
  if (ret != GST_FLOW_OK) {
    outbuf = NULL;              /* just to asure NULL return, looking the path
                                   above it happens only when gst_pad_alloc_buffer
                                   fails to alloc outbf */
  }
  *obuf = outbuf;

  return ret;

  /* ERRORS */
no_buffer:
  {
    if (GST_FLOW_IS_FATAL (ret)) {
      GST_ELEMENT_ERROR (mpeg2dec, RESOURCE, FAILED, (NULL),
          ("Failed to allocate memory for buffer, reason %s",
              gst_flow_get_name (ret)));
    }
    goto done;
  }
}

static gboolean
gst_mpeg2dec_negotiate_format (GstMpeg2dec * mpeg2dec)
{
  GstCaps *caps;
  guint32 fourcc;
  const mpeg2_info_t *info;
  const mpeg2_sequence_t *sequence;

  info = mpeg2_info (mpeg2dec->decoder);
  sequence = info->sequence;

  if (sequence->width != sequence->chroma_width &&
      sequence->height != sequence->chroma_height) {
    fourcc = GST_STR_FOURCC ("I420");
    mpeg2dec->format = MPEG2DEC_FORMAT_I420;
  } else if (sequence->width == sequence->chroma_width ||
      sequence->height == sequence->chroma_height) {
    fourcc = GST_STR_FOURCC ("Y42B");
    mpeg2dec->format = MPEG2DEC_FORMAT_I422;
  } else {
    g_warning ("mpeg2dec: 4:4:4 format not yet supported");
    return (FALSE);
  }

  caps = gst_caps_new_simple ("video/x-raw-yuv",
      "format", GST_TYPE_FOURCC, fourcc,
      "width", G_TYPE_INT, mpeg2dec->width,
      "height", G_TYPE_INT, mpeg2dec->height,
      "pixel-aspect-ratio", GST_TYPE_FRACTION, mpeg2dec->pixel_width,
      mpeg2dec->pixel_height,
      "framerate", G_TYPE_DOUBLE, mpeg2dec->frame_rate, NULL);

  gst_pad_set_caps (mpeg2dec->srcpad, caps);
  gst_caps_unref (caps);

  return TRUE;
}

static GstFlowReturn
handle_sequence (GstMpeg2dec * mpeg2dec, const mpeg2_info_t * info)
{
  gint i;
  GstBuffer *buf;
  GstFlowReturn ret;

  mpeg2dec->width = info->sequence->picture_width;
  mpeg2dec->height = info->sequence->picture_height;
  mpeg2dec->pixel_width = info->sequence->pixel_width;
  mpeg2dec->pixel_height = info->sequence->pixel_height;
  mpeg2dec->decoded_width = info->sequence->width;
  mpeg2dec->decoded_height = info->sequence->height;
  mpeg2dec->total_frames = 0;

  /* find framerate */
  for (i = 0; i < 9; i++) {
    if (info->sequence->frame_period == frame_periods[i]) {
      mpeg2dec->frame_rate = fpss[i];
    }
  }
  mpeg2dec->frame_period = info->sequence->frame_period * GST_USECOND / 27;

  GST_DEBUG_OBJECT (mpeg2dec,
      "sequence flags: %d, frame period: %d (%g), frame rate: %g",
      info->sequence->flags, info->sequence->frame_period,
      (double) (mpeg2dec->frame_period) / GST_SECOND, mpeg2dec->frame_rate);
  GST_DEBUG_OBJECT (mpeg2dec, "profile: %02x, colour_primaries: %d",
      info->sequence->profile_level_id, info->sequence->colour_primaries);
  GST_DEBUG_OBJECT (mpeg2dec, "transfer chars: %d, matrix coef: %d",
      info->sequence->transfer_characteristics,
      info->sequence->matrix_coefficients);

  if (!gst_mpeg2dec_negotiate_format (mpeg2dec))
    goto negotiate_failed;

  mpeg2_custom_fbuf (mpeg2dec->decoder, 1);

  ret = gst_mpeg2dec_alloc_buffer (mpeg2dec, mpeg2dec->offset, &buf);
  if (ret != GST_FLOW_OK)
    goto done;

  /* libmpeg2 discards first buffer twice for some reason. */
  gst_buffer_ref (buf);

  mpeg2dec->need_sequence = FALSE;

done:
  return ret;

negotiate_failed:
  {
    GST_ELEMENT_ERROR (mpeg2dec, CORE, NEGOTIATION, (NULL), (NULL));
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto done;
  }
}

static GstFlowReturn
handle_picture (GstMpeg2dec * mpeg2dec, const mpeg2_info_t * info)
{
  gboolean key_frame = FALSE;
  GstBuffer *outbuf;
  GstFlowReturn ret;

  GST_DEBUG_OBJECT (mpeg2dec, "handle picture");

  if (info->current_picture) {
    key_frame =
        (info->current_picture->flags & PIC_MASK_CODING_TYPE) ==
        PIC_FLAG_CODING_TYPE_I;
  }
  ret = gst_mpeg2dec_alloc_buffer (mpeg2dec, mpeg2dec->offset, &outbuf);
  if (ret != GST_FLOW_OK)
    goto done;

  GST_DEBUG_OBJECT (mpeg2dec, "picture %s, outbuf %p, offset %"
      G_GINT64_FORMAT,
      key_frame ? ", kf," : "    ", outbuf, GST_BUFFER_OFFSET (outbuf)
      );

  if (mpeg2dec->discont_state == MPEG2DEC_DISC_NEW_PICTURE && key_frame)
    mpeg2dec->discont_state = MPEG2DEC_DISC_NEW_KEYFRAME;

  mpeg2_skip (mpeg2dec->decoder, 0);

done:
  return ret;
}

static GstFlowReturn
handle_slice (GstMpeg2dec * mpeg2dec, const mpeg2_info_t * info)
{
  GstBuffer *outbuf = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (mpeg2dec, "picture slice/end %p %p %p %p",
      info->display_fbuf,
      info->display_picture, info->current_picture,
      (info->display_fbuf ? info->display_fbuf->id : NULL));

  if (info->display_fbuf && info->display_fbuf->id) {
    const mpeg2_picture_t *picture;
    gboolean key_frame = FALSE;
    GstClockTime time;

    outbuf = GST_BUFFER (info->display_fbuf->id);

    picture = info->display_picture;

    key_frame =
        (picture->flags & PIC_MASK_CODING_TYPE) == PIC_FLAG_CODING_TYPE_I;

    GST_DEBUG_OBJECT (mpeg2dec, "picture keyframe %d", key_frame);

    if (!key_frame)
      GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DELTA_UNIT);
    else
      GST_BUFFER_FLAG_UNSET (outbuf, GST_BUFFER_FLAG_DELTA_UNIT);

    if (mpeg2dec->discont_state == MPEG2DEC_DISC_NEW_KEYFRAME && key_frame)
      mpeg2dec->discont_state = MPEG2DEC_DISC_NONE;

    time = GST_CLOCK_TIME_NONE;

#if MPEG2_RELEASE < MPEG2_VERSION(0,4,0)
    if (picture->flags & PIC_FLAG_PTS)
      time = MPEG_TIME_TO_GST_TIME (picture->pts);
#else
    if (picture->flags & PIC_FLAG_TAGS)
      time = MPEG_TIME_TO_GST_TIME ((GstClockTime) (picture->
              tag2) << 32 | picture->tag);
#endif

    if (time == GST_CLOCK_TIME_NONE) {
      time = mpeg2dec->next_time;
      GST_DEBUG_OBJECT (mpeg2dec, "picture didn't have pts");
    } else {
      GST_DEBUG_OBJECT (mpeg2dec,
          "picture had pts %" GST_TIME_FORMAT ", we had %"
          GST_TIME_FORMAT, GST_TIME_ARGS (time),
          GST_TIME_ARGS (mpeg2dec->next_time));
      mpeg2dec->next_time = time;
    }
    GST_BUFFER_TIMESTAMP (outbuf) = time;

    /* TODO set correct offset here based on frame number */
    if (info->display_picture_2nd) {
      GST_BUFFER_DURATION (outbuf) = (picture->nb_fields +
          info->display_picture_2nd->nb_fields) * mpeg2dec->frame_period / 2;
    } else {
      GST_BUFFER_DURATION (outbuf) =
          picture->nb_fields * mpeg2dec->frame_period / 2;
    }
    mpeg2dec->next_time += GST_BUFFER_DURATION (outbuf);

    GST_DEBUG_OBJECT (mpeg2dec,
        "picture: %s %s fields:%d off:%" G_GINT64_FORMAT " ts:%"
        GST_TIME_FORMAT,
        (picture->flags & PIC_FLAG_TOP_FIELD_FIRST ? "tff " : "    "),
        (picture->flags & PIC_FLAG_PROGRESSIVE_FRAME ? "prog" : "    "),
        picture->nb_fields, GST_BUFFER_OFFSET (outbuf),
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)));

    if (mpeg2dec->index) {
      gst_index_add_association (mpeg2dec->index, mpeg2dec->index_id,
          (key_frame ? GST_ASSOCIATION_FLAG_KEY_UNIT : 0),
          GST_FORMAT_BYTES, GST_BUFFER_OFFSET (outbuf),
          GST_FORMAT_TIME, GST_BUFFER_TIMESTAMP (outbuf), 0);
    }

    if (picture->flags & PIC_FLAG_SKIP) {
      GST_DEBUG_OBJECT (mpeg2dec, "dropping buffer because of skip flag");
    } else if (mpeg2dec->discont_state != MPEG2DEC_DISC_NONE) {
      GST_DEBUG_OBJECT (mpeg2dec, "dropping buffer, discont state %d",
          mpeg2dec->discont_state);
    } else if (mpeg2dec->next_time < mpeg2dec->segment_start) {
      GST_DEBUG_OBJECT (mpeg2dec, "dropping buffer, next_time %"
          GST_TIME_FORMAT " <  segment_start %" GST_TIME_FORMAT,
          GST_TIME_ARGS (mpeg2dec->next_time),
          GST_TIME_ARGS (mpeg2dec->segment_start));
    } else {
      GST_LOG_OBJECT (mpeg2dec, "pushing buffer, timestamp %"
          GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT,
          GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
          GST_TIME_ARGS (GST_BUFFER_DURATION (outbuf)));

      if ((mpeg2dec->decoded_height > mpeg2dec->height) ||
          (mpeg2dec->decoded_width > mpeg2dec->width)) {
        /* CHECKME: this might unref outbuf and return a new buffer.
         * Does this affect the info->discard_fbuf stuff below? */
        outbuf = crop_buffer (mpeg2dec, outbuf);
      }

      gst_buffer_ref (outbuf);
      ret = gst_pad_push (mpeg2dec->srcpad, outbuf);
    }
  }

  if (info->discard_fbuf && info->discard_fbuf->id) {
    GstBuffer *discard = GST_BUFFER (info->discard_fbuf->id);

    gst_buffer_unref (discard);
    GST_DEBUG_OBJECT (mpeg2dec, "Discarded buffer %p", discard);
  }
  return ret;
}

#if 0
static void
update_streaminfo (GstMpeg2dec * mpeg2dec)
{
  GstCaps *caps;
  GstProps *props;
  GstPropsEntry *entry;
  const mpeg2_info_t *info;

  info = mpeg2_info (mpeg2dec->decoder);

  props = gst_props_empty_new ();

  entry =
      gst_props_entry_new ("framerate",
      G_TYPE_DOUBLE (GST_SECOND / (float) mpeg2dec->frame_period));
  gst_props_add_entry (props, entry);
  entry =
      gst_props_entry_new ("bitrate",
      G_TYPE_INT (info->sequence->byte_rate * 8));
  gst_props_add_entry (props, entry);

  caps = gst_caps_new ("mpeg2dec_streaminfo",
      "application/x-gst-streaminfo", props);

  gst_caps_replace_sink (&mpeg2dec->streaminfo, caps);
  g_object_notify (G_OBJECT (mpeg2dec), "streaminfo");
}
#endif

static GstFlowReturn
gst_mpeg2dec_chain (GstPad * pad, GstBuffer * buf)
{
  GstMpeg2dec *mpeg2dec;
  guint32 size;
  guint8 *data, *end;
  GstClockTime pts;
  const mpeg2_info_t *info;
  mpeg2_state_t state;
  gboolean done = FALSE;
  GstFlowReturn ret = GST_FLOW_OK;

  mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));

  size = GST_BUFFER_SIZE (buf);
  data = GST_BUFFER_DATA (buf);
  pts = GST_BUFFER_TIMESTAMP (buf);

  GST_LOG_OBJECT (mpeg2dec, "received buffer, timestamp %"
      GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buf)));

  info = mpeg2dec->info;
  end = data + size;

  mpeg2dec->offset = GST_BUFFER_OFFSET (buf);

  while (!done) {
    GST_LOG_OBJECT (mpeg2dec, "calling parse");
    state = mpeg2_parse (mpeg2dec->decoder);
    GST_DEBUG_OBJECT (mpeg2dec, "parse state %d", state);

    switch (state) {
      case STATE_SEQUENCE:
        ret = handle_sequence (mpeg2dec, info);
        break;
      case STATE_SEQUENCE_REPEATED:
        GST_DEBUG_OBJECT (mpeg2dec, "sequence repeated");
        break;
      case STATE_GOP:
        break;
      case STATE_PICTURE:
        ret = handle_picture (mpeg2dec, info);
        break;
      case STATE_SLICE_1ST:
        GST_LOG_OBJECT (mpeg2dec, "1st slice of frame encountered");
        break;
      case STATE_PICTURE_2ND:
        GST_LOG_OBJECT (mpeg2dec,
            "Second picture header encountered. Decoding 2nd field");
        break;
#if MPEG2_RELEASE >= MPEG2_VERSION (0, 4, 0)
      case STATE_INVALID_END:
#endif
      case STATE_END:
        mpeg2dec->need_sequence = TRUE;
      case STATE_SLICE:
        ret = handle_slice (mpeg2dec, info);
        break;
      case STATE_BUFFER:
        if (data == NULL) {
          done = TRUE;
        } else {
          if (pts != GST_CLOCK_TIME_NONE) {
            gint64 mpeg_pts = GST_TIME_TO_MPEG_TIME (pts);

            GST_DEBUG_OBJECT (mpeg2dec,
                "have pts: %" G_GINT64_FORMAT " (%" GST_TIME_FORMAT ")",
                mpeg_pts, GST_TIME_ARGS (MPEG_TIME_TO_GST_TIME (mpeg_pts)));

#if MPEG2_RELEASE >= MPEG2_VERSION(0,4,0)
            mpeg2_tag_picture (mpeg2dec->decoder, mpeg_pts & 0xffffffff,
                mpeg_pts >> 32);
#else
            mpeg2_pts (mpeg2dec->decoder, mpeg_pts);
#endif
          } else {
            GST_LOG ("no pts");
          }

          GST_LOG_OBJECT (mpeg2dec, "calling mpeg2_buffer");
          mpeg2_buffer (mpeg2dec->decoder, data, end);
          GST_LOG_OBJECT (mpeg2dec, "calling mpeg2_buffer done");

          data = NULL;
        }
        break;
        /* error */
      case STATE_INVALID:
        GST_WARNING_OBJECT (mpeg2dec, "Decoding error");
        goto exit;
      default:
        GST_ERROR_OBJECT (mpeg2dec, "Unknown libmpeg2 state %d, FIXME", state);
        break;
    }

    /*
     * FIXME: should pass more information such as state the user data is from
     */
#ifdef enable_user_data
    if (info->user_data_len > 0) {
      GstBuffer *udbuf = gst_buffer_new_and_alloc (info->user_data_len);

      memcpy (GST_BUFFER_DATA (udbuf), info->user_data, info->user_data_len);

      gst_pad_push (mpeg2dec->userdatapad, udbuf);
    }
#endif

    if (ret != GST_FLOW_OK) {
      mpeg2_reset (mpeg2dec->decoder, 0);
      break;
    }
  }
  gst_buffer_unref (buf);
  return ret;

exit:
  gst_buffer_unref (buf);
  return GST_FLOW_ERROR;
}

static gboolean
gst_mpeg2dec_sink_event (GstPad * pad, GstEvent * event)
{

  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (mpeg2dec, "Got %s event on sink pad",
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NEWSEGMENT:
    {
      GST_STREAM_LOCK (pad);
      mpeg2dec->next_time = -1;;
      ret = gst_pad_event_default (pad, event);
      GST_STREAM_UNLOCK (pad);
      break;
    }
    case GST_EVENT_FLUSH_START:
      ret = gst_pad_event_default (pad, event);
      break;
    case GST_EVENT_FLUSH_STOP:
      GST_STREAM_LOCK (pad);
      mpeg2dec->discont_state = MPEG2DEC_DISC_NEW_PICTURE;
      mpeg2dec->next_time = -1;;
      mpeg2_reset (mpeg2dec->decoder, 0);
      ret = gst_pad_event_default (pad, event);
      GST_STREAM_UNLOCK (pad);
      break;
    case GST_EVENT_EOS:
      GST_STREAM_LOCK (pad);
      if (mpeg2dec->index && mpeg2dec->closed) {
        gst_index_commit (mpeg2dec->index, mpeg2dec->index_id);
      }
      ret = gst_pad_event_default (pad, event);
      GST_STREAM_UNLOCK (pad);
      break;

    default:
      ret = gst_pad_event_default (pad, event);
      break;
  }

  return ret;

}

static GstCaps *
gst_mpeg2dec_src_getcaps (GstPad * pad)
{
  GstCaps *caps;

  GST_OBJECT_LOCK (pad);
  if (!(caps = GST_PAD_CAPS (pad)))
    caps = (GstCaps *) gst_pad_get_pad_template_caps (pad);
  caps = gst_caps_ref (caps);
  GST_OBJECT_UNLOCK (pad);

  return caps;
}

static gboolean
gst_mpeg2dec_sink_convert (GstPad * pad, GstFormat src_format, gint64 src_value,
    GstFormat * dest_format, gint64 * dest_value)
{
  gboolean res = TRUE;
  GstMpeg2dec *mpeg2dec;
  const mpeg2_info_t *info;

  mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));

  if (mpeg2dec->decoder == NULL)
    return FALSE;

  if (src_format == *dest_format) {
    *dest_value = src_value;
    return TRUE;
  }

  info = mpeg2_info (mpeg2dec->decoder);

  switch (src_format) {
    case GST_FORMAT_BYTES:
      switch (*dest_format) {
        case GST_FORMAT_TIME:
          if (info->sequence && info->sequence->byte_rate) {
            *dest_value = GST_SECOND * src_value / info->sequence->byte_rate;
            break;
          }
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_TIME:
      switch (*dest_format) {
        case GST_FORMAT_BYTES:
          if (info->sequence && info->sequence->byte_rate) {
            *dest_value = src_value * info->sequence->byte_rate / GST_SECOND;
            break;
          }
        default:
          res = FALSE;
      }
      break;
    default:
      res = FALSE;
  }
  return res;
}


static gboolean
gst_mpeg2dec_src_convert (GstPad * pad, GstFormat src_format, gint64 src_value,
    GstFormat * dest_format, gint64 * dest_value)
{
  gboolean res = TRUE;
  GstMpeg2dec *mpeg2dec;
  const mpeg2_info_t *info;
  guint64 scale = 1;

  mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));

  if (mpeg2dec->decoder == NULL)
    return FALSE;

  if (src_format == *dest_format) {
    *dest_value = src_value;
    return TRUE;
  }

  info = mpeg2_info (mpeg2dec->decoder);

  switch (src_format) {
    case GST_FORMAT_BYTES:
      switch (*dest_format) {
        case GST_FORMAT_TIME:
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_TIME:
      switch (*dest_format) {
        case GST_FORMAT_BYTES:
          scale = 6 * (mpeg2dec->width * mpeg2dec->height >> 2);
        case GST_FORMAT_DEFAULT:
          if (info->sequence && mpeg2dec->frame_period) {
            *dest_value = src_value * scale / mpeg2dec->frame_period;
            break;
          }
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_DEFAULT:
      switch (*dest_format) {
        case GST_FORMAT_TIME:
          *dest_value = src_value * mpeg2dec->frame_period;
          break;
        case GST_FORMAT_BYTES:
          *dest_value =
              src_value * 6 * ((mpeg2dec->width * mpeg2dec->height) >> 2);
          break;
        default:
          res = FALSE;
      }
      break;
    default:
      res = FALSE;
  }
  return res;
}

static const GstQueryType *
gst_mpeg2dec_get_src_query_types (GstPad * pad)
{
  static const GstQueryType types[] = {
    GST_QUERY_POSITION,
    GST_QUERY_DURATION,
    0
  };

  return types;
}

static gboolean
gst_mpeg2dec_src_query (GstPad * pad, GstQuery * query)
{
  gboolean res = TRUE;
  GstMpeg2dec *mpeg2dec;

  mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat format;
      gint64 cur;

      /* save requested format */
      gst_query_parse_position (query, &format, NULL);

      /* and convert to the requested format */
      if (!gst_mpeg2dec_src_convert (pad, GST_FORMAT_TIME,
              mpeg2dec->next_time, &format, &cur))
        goto error;

      gst_query_set_position (query, format, cur);

      GST_LOG_OBJECT (mpeg2dec,
          "position query: we return %llu (format %u)", cur, format);
      break;
    }
    case GST_QUERY_DURATION:
    {
      GstFormat format;
      GstFormat rformat;
      gint64 total, total_bytes;
      GstPad *peer;

      if ((peer = gst_pad_get_peer (mpeg2dec->sinkpad)) == NULL)
        goto error;

      /* send to peer */
      if ((res = gst_pad_query (peer, query))) {
        gst_object_unref (peer);
        goto done;
      } else {
        GST_LOG_OBJECT (mpeg2dec, "query on peer pad failed, trying bytes");
      }

      /* save requested format */
      gst_query_parse_duration (query, &format, NULL);

      /* query peer for total length in bytes */
      gst_query_set_duration (query, GST_FORMAT_BYTES, -1);

      if (!(res = gst_pad_query (peer, query))) {
        GST_LOG_OBJECT (mpeg2dec, "query on peer pad failed");
        gst_object_unref (peer);
        goto error;
      }
      gst_object_unref (peer);

      /* get the returned format */
      gst_query_parse_duration (query, &rformat, &total_bytes);
      GST_LOG_OBJECT (mpeg2dec, "peer pad returned total=%lld bytes",
          total_bytes);

      if (total_bytes != -1) {
        if (!gst_mpeg2dec_sink_convert (pad, GST_FORMAT_BYTES, total_bytes,
                &format, &total))
          goto error;
      } else {
        total = -1;
      }

      gst_query_set_duration (query, format, total);

      GST_LOG_OBJECT (mpeg2dec,
          "position query: we return %llu (format %u)", total, format);
      break;
    }
    default:
      res = FALSE;
      break;
  }
done:
  return res;

error:

  GST_DEBUG ("error handling query");
  return FALSE;
}


#if 0
static const GstEventMask *
gst_mpeg2dec_get_event_masks (GstPad * pad)
{
  static const GstEventMask masks[] = {
    {GST_EVENT_SEEK, GST_SEEK_METHOD_SET | GST_SEEK_FLAG_FLUSH},
    {GST_EVENT_NAVIGATION, GST_EVENT_FLAG_NONE},
    {0,}
  };

  return masks;
}
#endif

static gboolean
index_seek (GstPad * pad, GstEvent * event)
{
  GstIndexEntry *entry;
  GstMpeg2dec *mpeg2dec;
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType cur_type, stop_type;
  gint64 cur, stop;

  mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));

  gst_event_parse_seek (event, &rate, &format, &flags,
      &cur_type, &cur, &stop_type, &stop);

  entry = gst_index_get_assoc_entry (mpeg2dec->index, mpeg2dec->index_id,
      GST_INDEX_LOOKUP_BEFORE, GST_ASSOCIATION_FLAG_KEY_UNIT, format, cur);

  if ((entry) && gst_pad_is_linked (mpeg2dec->sinkpad)) {
    const GstFormat *peer_formats, *try_formats;

    /* since we know the exact byteoffset of the frame, make sure to seek on bytes first */
    const GstFormat try_all_formats[] = {
      GST_FORMAT_BYTES,
      GST_FORMAT_TIME,
      0
    };

    try_formats = try_all_formats;

#if 0
    peer_formats = gst_pad_get_formats (GST_PAD_PEER (mpeg2dec->sinkpad));
#else
    peer_formats = try_all_formats;     /* FIXE ME */
#endif

    while (gst_formats_contains (peer_formats, *try_formats)) {
      gint64 value;

      if (gst_index_entry_assoc_map (entry, *try_formats, &value)) {
        GstEvent *seek_event;

        GST_DEBUG_OBJECT (mpeg2dec, "index %s %" G_GINT64_FORMAT
            " -> %s %" G_GINT64_FORMAT,
            gst_format_get_details (format)->nick,
            cur, gst_format_get_details (*try_formats)->nick, value);

        /* lookup succeeded, create the seek */
        seek_event =
            gst_event_new_seek (rate, *try_formats, flags, cur_type, value,
            stop_type, stop);
        /* do the seek */
        if (gst_pad_push_event (mpeg2dec->sinkpad, seek_event)) {
          /* seek worked, we're done, loop will exit */
#if 0
          mpeg2dec->segment_start = GST_EVENT_SEEK_OFFSET (event);
#endif
          return TRUE;
        }
      }
      try_formats++;
    }
  }
  return FALSE;
}


static gboolean
normal_seek (GstPad * pad, GstEvent * event)
{
  gdouble rate;
  GstFormat format, conv;
  GstSeekFlags flags;
  GstSeekType cur_type, stop_type;
  gint64 cur, stop;
  gint64 time_cur, bytes_cur;
  gint64 time_stop, bytes_stop;
  gboolean res;
  GstMpeg2dec *mpeg2dec;
  GstEvent *peer_event;

  mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));

  GST_DEBUG ("normal seek");

  gst_event_parse_seek (event, &rate, &format, &flags,
      &cur_type, &cur, &stop_type, &stop);

  conv = GST_FORMAT_TIME;
  if (!gst_mpeg2dec_src_convert (pad, format, cur, &conv, &time_cur))
    goto convert_failed;
  if (!gst_mpeg2dec_src_convert (pad, format, stop, &conv, &time_stop))
    goto convert_failed;

  GST_DEBUG ("seek to time %" GST_TIME_FORMAT "-%" GST_TIME_FORMAT,
      GST_TIME_ARGS (time_cur), GST_TIME_ARGS (time_stop));

  peer_event = gst_event_new_seek (rate, GST_FORMAT_TIME, flags,
      cur_type, time_cur, stop_type, time_stop);

  /* try seek on time then */
  if ((res = gst_pad_push_event (mpeg2dec->sinkpad, peer_event)))
    goto done;

  /* else we try to seek on bytes */
  conv = GST_FORMAT_BYTES;
  if (!gst_mpeg2dec_sink_convert (pad, GST_FORMAT_TIME, time_cur,
          &format, &bytes_cur))
    goto convert_failed;
  if (!gst_mpeg2dec_sink_convert (pad, GST_FORMAT_TIME, time_stop,
          &format, &bytes_stop))
    goto convert_failed;

  /* conversion succeeded, create the seek */
  peer_event =
      gst_event_new_seek (rate, GST_FORMAT_BYTES, flags,
      cur_type, bytes_cur, stop_type, bytes_stop);

  /* do the seek */
  res = gst_pad_push_event (mpeg2dec->sinkpad, peer_event);

done:
  return res;

  /* ERRORS */
convert_failed:
  {
    /* probably unsupported seek format */
    GST_DEBUG_OBJECT (mpeg2dec,
        "failed to convert format %u into GST_FORMAT_TIME", format);
    return FALSE;
  }
}


static gboolean
gst_mpeg2dec_src_event (GstPad * pad, GstEvent * event)
{
  gboolean res;
  GstMpeg2dec *mpeg2dec;

  mpeg2dec = GST_MPEG2DEC (GST_PAD_PARENT (pad));

  if (mpeg2dec->decoder == NULL)
    goto no_decoder;

  switch (GST_EVENT_TYPE (event)) {
      /* the all-formats seek logic */
    case GST_EVENT_SEEK:
      if (mpeg2dec->index)
        res = index_seek (pad, event);
      else
        res = normal_seek (pad, event);

      gst_event_unref (event);
      break;
    case GST_EVENT_NAVIGATION:
      /* Forward a navigation event unchanged */
    default:
      res = gst_pad_push_event (mpeg2dec->sinkpad, event);
      break;
  }
  return res;

no_decoder:
  {
    GST_DEBUG_OBJECT (mpeg2dec, "no decoder, cannot handle event");
    gst_event_unref (event);
    return FALSE;
  }
}

static GstStateChangeReturn
gst_mpeg2dec_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      mpeg2_accel (MPEG2_ACCEL_DETECT);
      if ((mpeg2dec->decoder = mpeg2_init ()) == NULL)
        goto init_failed;
      mpeg2dec->info = mpeg2_info (mpeg2dec->decoder);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_mpeg2dec_reset (mpeg2dec);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (mpeg2dec->decoder) {
        mpeg2_close (mpeg2dec->decoder);
        mpeg2dec->decoder = NULL;
        mpeg2dec->info = NULL;
      }
      break;
    default:
      break;
  }
  return ret;

  /* ERRORS */
init_failed:
  {
    GST_ELEMENT_ERROR (mpeg2dec, LIBRARY, INIT,
        (NULL), ("Failed to initialize libmpeg2 library"));
    return GST_STATE_CHANGE_FAILURE;
  }
}

static void
gst_mpeg2dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMpeg2dec *src;

  g_return_if_fail (GST_IS_MPEG2DEC (object));
  src = GST_MPEG2DEC (object);

  switch (prop_id) {
    default:
      break;
  }
}

static void
gst_mpeg2dec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMpeg2dec *mpeg2dec;

  g_return_if_fail (GST_IS_MPEG2DEC (object));
  mpeg2dec = GST_MPEG2DEC (object);

  switch (prop_id) {
    default:
      break;
  }
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "mpeg2dec", GST_RANK_SECONDARY,
          GST_TYPE_MPEG2DEC))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "mpeg2dec",
    "LibMpeg2 decoder", plugin_init, VERSION, "GPL", GST_PACKAGE, GST_ORIGIN);
