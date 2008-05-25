/*  Time-stamp: <2007-11-04 09:46:51 jcs>
 *
 *  Copyright (C) 2005 Christophe Fergeau
 *
 *  URL: http://www.gtkpod.org/libgpod.html
 *  URL: http://gtkpod.sourceforge.net/
 * 
 *  The code contained in this file is free software; you can redistribute
 *  it and/or modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either version
 *  2.1 of the License, or (at your option) any later version.
 *  
 *  This file is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *  
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this code; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 *  iTunes and iPod are trademarks of Apple
 * 
 *  This product is not supported/written/published by Apple!
 *
 *  $Id$
 */

#include <config.h>
#include "itdb.h"
#include "db-image-parser.h"

#ifdef HAVE_GDKPIXBUF

#include "itdb_private.h"
#include "itdb_endianness.h"
#include "pixmaps.h"

#include <errno.h>
#include <locale.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>
#include <fcntl.h>

#include <glib/gstdio.h>

/* Maximum size for .ithmb files. Reduced from 500 MB to 256 MB after
   reports of slow iPod interface behavior */
#define ITHUMB_MAX_SIZE (256L*1000L*1000L)
/* for testing: */
/*#define ITHUMB_MAX_SIZE (1L*1000L*1000L)*/

struct _iThumbWriter {
	off_t cur_offset;
	FILE *f;
        gchar *mountpoint;
        gchar *filename;
        gint current_file_index;
	const Itdb_ArtworkFormat *img_info;
        DbType db_type;
        guint byte_order;
};
typedef struct _iThumbWriter iThumbWriter;


static guint16 *
pack_RGB_565 (GdkPixbuf *pixbuf, const Itdb_ArtworkFormat *img_info,
	      gint horizontal_padding, gint vertical_padding,
	      guint32 *thumb_size)
{
	guchar *pixels;
	guint16 *result;
	gint row_stride;
	gint channels;
	gint width;
	gint height;
	gint w;
	gint h;
	gint byte_order;

	g_object_get (G_OBJECT (pixbuf), 
		      "rowstride", &row_stride, "n-channels", &channels,
		      "height", &height, "width", &width,
		      "pixels", &pixels, NULL);
	g_return_val_if_fail ((width <= img_info->width) && (height <= img_info->height), NULL);
	/* dst_width and dst_height come from a width/height database 
	 * hardcoded in libipoddevice code, so dst_width * dst_height * 2 can't
	 * overflow, even on an iPod containing malicious data
	 */
	*thumb_size = img_info->width * img_info->height * 2;
	result = g_malloc0 (*thumb_size);

	byte_order = itdb_thumb_get_byteorder (img_info->format);

	for (h = 0; h < height; h++) {
	        gint line = (h+vertical_padding)*img_info->width;
		for (w = 0; w < width; w++) {
			gint r;
			gint g;
			gint b;

			r = pixels[h*row_stride + w*channels];
			g = pixels[h*row_stride + w*channels + 1]; 
			b = pixels[h*row_stride + w*channels + 2]; 

			r >>= (8 - RED_BITS_565);
			g >>= (8 - GREEN_BITS_565);
			b >>= (8 - BLUE_BITS_565);
			r = (r << RED_SHIFT_565) & RED_MASK_565;
			g = (g << GREEN_SHIFT_565) & GREEN_MASK_565;
			b = (b << BLUE_SHIFT_565) & BLUE_MASK_565;
			result[line + w + horizontal_padding] =
			    get_gint16 (r | g | b, byte_order);
		}
	}
	return result;
}

static guint16 *
pack_RGB_555 (GdkPixbuf *pixbuf, const Itdb_ArtworkFormat *img_info,
	      gint horizontal_padding, gint vertical_padding,
	      guint32 *thumb_size)
{
	guchar *pixels;
	guint16 *result;
	gint row_stride;
	gint channels;
	gint width;
	gint height;
	gint w;
	gint h;
	gint byte_order;

	g_object_get (G_OBJECT (pixbuf), 
		      "rowstride", &row_stride, "n-channels", &channels,
		      "height", &height, "width", &width,
		      "pixels", &pixels, NULL);
	g_return_val_if_fail ((width <= img_info->width) && (height <= img_info->height), NULL);
	/* dst_width and dst_height come from a width/height database 
	 * hardcoded in libipoddevice code, so dst_width * dst_height * 2 can't
	 * overflow, even on an iPod containing malicious data
	 */

	*thumb_size = img_info->width * img_info->height * 2;
	result = g_malloc0 (*thumb_size);

	byte_order = itdb_thumb_get_byteorder (img_info->format);

	for (h = 0; h < height; h++) {
	        gint line = (h+vertical_padding)*img_info->width;
		for (w = 0; w < width; w++) {
			gint r;
			gint g;
			gint b;
			gint a;

			r = pixels[h*row_stride + w*channels];
			g = pixels[h*row_stride + w*channels + 1]; 
			b = pixels[h*row_stride + w*channels + 2]; 

			r >>= (8 - RED_BITS_555);
			g >>= (8 - GREEN_BITS_555);
			b >>= (8 - BLUE_BITS_555);
			a = (1 << ALPHA_SHIFT_555) & ALPHA_MASK_555;
			r = (r << RED_SHIFT_555) & RED_MASK_555;
			g = (g << GREEN_SHIFT_555) & GREEN_MASK_555;
			b = (b << BLUE_SHIFT_555) & BLUE_MASK_555;
			result[line + w + horizontal_padding] =
			    get_gint16 (a | r | g | b, byte_order);
			/* I'm not sure if the highest bit really is
			   the alpha channel. For now I'm just setting
			   this bit because that's what I have seen. */
		}
	}
	return result;
}


static guint16 *
pack_RGB_888 (GdkPixbuf *pixbuf, const Itdb_ArtworkFormat *img_info,
	      gint horizontal_padding, gint vertical_padding,
	      guint32 *thumb_size)
{
	guchar *pixels;
	guint32 *result;
	gint row_stride;
	gint channels;
	gint width;
	gint height;
	gint w;
	gint h;
	gint byte_order;

	g_object_get (G_OBJECT (pixbuf), 
		      "rowstride", &row_stride, "n-channels", &channels,
		      "height", &height, "width", &width,
		      "pixels", &pixels, NULL);
	g_return_val_if_fail ((width <= img_info->width) && (height <= img_info->height), NULL);
	/* dst_width and dst_height come from a width/height database 
	 * hardcoded in libipoddevice code, so dst_width * dst_height * 2 can't
	 * overflow, even on an iPod containing malicious data
	 */
	*thumb_size = img_info->width * img_info->height * 4;
	result = g_malloc0 (*thumb_size);

	byte_order = itdb_thumb_get_byteorder (img_info->format);

	for (h = 0; h < height; h++) {
	        gint line = (h+vertical_padding)*img_info->width;
		for (w = 0; w < width; w++) {
			guint32 r;
			guint32 g;
			guint32 b;
			guint32 a;

			r = pixels[h*row_stride + w*channels];
			g = pixels[h*row_stride + w*channels + 1]; 
			b = pixels[h*row_stride + w*channels + 2]; 

			r >>= (8 - RED_BITS_888);
			g >>= (8 - GREEN_BITS_888);
			b >>= (8 - BLUE_BITS_888);
			a = (0xff << ALPHA_SHIFT_888) & ALPHA_MASK_888;
			r = (r << RED_SHIFT_888) & RED_MASK_888;
			g = (g << GREEN_SHIFT_888) & GREEN_MASK_888;
			b = (b << BLUE_SHIFT_888) & BLUE_MASK_888;
			result[line + w + horizontal_padding] =
			    get_gint32 (a | r | g | b, byte_order);
			/* I'm not sure if the highest bit really is
			   the alpha channel. For now I'm just setting
			   these bits because that's what I have seen. */
		}
	}
	return (guint16 *)result;
}


static guint16 *derange_pixels (guint16 *pixels_s, guint16 *pixels_d,
				gint width, gint height, gint row_stride)
{
    g_return_val_if_fail (width == height, pixels_s);

    if (pixels_s == NULL)
    {
	pixels_s = g_malloc0 (sizeof (guint16)*width*height);
    }

    if (width == 1)
    {
	*pixels_s = *pixels_d;
    }
    else
    {
	derange_pixels (pixels_s + 0,
			pixels_d + 0 + 0,
			width/2, height/2,
			row_stride);
	derange_pixels (pixels_s + (width/2)*(height/2),
			pixels_d + (height/2)*row_stride + 0,
			width/2, height/2,
			row_stride);
	derange_pixels (pixels_s + 2*(width/2)*(height/2),
			pixels_d + width/2,
			width/2, height/2,
			row_stride);
	derange_pixels (pixels_s + 3*(width/2)*(height/2),
			pixels_d + (height/2)*row_stride + width/2,
			width/2, height/2,
			row_stride);
    }
    
    return pixels_s;
}

static guint16 *
pack_rec_RGB_555 (GdkPixbuf *pixbuf, const Itdb_ArtworkFormat *img_info,
		  gint horizontal_padding, gint vertical_padding,
		  guint32 *thumb_size)
{
    guint16 *pixels;
    guint16 *deranged_pixels = NULL;

    pixels = pack_RGB_555 (pixbuf, img_info,
			   horizontal_padding, vertical_padding,
			   thumb_size);

    if (pixels)
    {
	deranged_pixels = derange_pixels (NULL, pixels,
					  img_info->width, img_info->height,
					  img_info->width);
	g_free (pixels);
    }

    return deranged_pixels;
}

static guchar *
pack_I420 (GdkPixbuf *orig_pixbuf, const Itdb_ArtworkFormat *img_info,
	   gint horizontal_padding, gint vertical_padding,
	   guint32 *thumb_size)
{
    GdkPixbuf *pixbuf;
    gint width, height;
    gint orig_height, orig_width;
    gint rowstride;
    gint h, z;
    guchar *pixels, *yuvdata;
    gint yuvsize, halfyuv;
    gint ustart, vstart;

    g_return_val_if_fail (img_info, NULL);

    width = img_info->width;
    height = img_info->height;

    g_object_get (G_OBJECT (orig_pixbuf), 
		  "height", &orig_height, "width", &orig_width, NULL);

    /* copy into new pixmap with padding applied */
    pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
			     gdk_pixbuf_get_has_alpha (orig_pixbuf),
			     8,
			     width, height);
    gdk_pixbuf_copy_area (orig_pixbuf, 0, 0, orig_width, orig_height,
			  pixbuf, horizontal_padding, vertical_padding);

    g_object_get (G_OBJECT (pixbuf), 
		  "rowstride", &rowstride,
		  "pixels", &pixels, NULL);

    halfyuv = width*height;

    yuvsize = 2*halfyuv;
    *thumb_size = yuvsize;

    yuvdata = g_malloc (yuvsize);

    ustart = halfyuv;
    vstart = ustart + halfyuv/4;

    /* FIXME: consider rowstride -- currently we assume rowstride==width */
    for (z=0,h=0; h < halfyuv; ++h)
    {
	gint r,g,b;
	gint u, v, y;
	gint row, col;

	r = pixels[z];
	g = pixels[z+1];
	b = pixels[z+2];

	y = (( 66*r + 129*g +  25*b + 128) >> 8) + 16;
	u = ((-38*r -  74*g + 112*b + 128) >> 8) + 128;
	v = ((112*r -  94*g -  18*b + 128) >> 8) + 128;

	row = h / width;
	col = h % width;

	yuvdata[h] = y;
	yuvdata[ustart + (row/2)*(width/2) + col/2] = u;
	yuvdata[vstart + (row/2)*(width/2) + col/2] = v;

	if (gdk_pixbuf_get_has_alpha(pixbuf))
	    z+=4;
	else
	    z+=3;
    }

    return yuvdata;
}

/* pack_UYVY() is adapted from imgconvert.c from the GPixPod project
 * (www.gpixpod.org) */
static guchar *
pack_UYVY (GdkPixbuf *orig_pixbuf, const Itdb_ArtworkFormat *img_info,
	   gint horizontal_padding, gint vertical_padding,
	   guint32 *thumb_size)
{
    GdkPixbuf *pixbuf;
    guchar *pixels, *yuvdata;
    gint width;
    gint height;
    gint orig_height, orig_width;
    gint x = 0;
    gint z = 0;
    gint z2 = 0;
    gint h = 0;
    gint r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
    gint rowstride;
    gint yuvsize, halfyuv;
    gint alphabit, rgbpx;
    gint exc;

    g_return_val_if_fail (img_info, NULL);

    width = img_info->width;
    height = img_info->height;
    *thumb_size = 2*width*height;

    g_object_get (G_OBJECT (orig_pixbuf), 
		  "height", &orig_height, "width", &orig_width, NULL);

    /* copy into new pixmap with padding applied */
    pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
			     gdk_pixbuf_get_has_alpha (orig_pixbuf),
			     8,
			     width, height);
    gdk_pixbuf_copy_area (orig_pixbuf, 0, 0, orig_width, orig_height,
			  pixbuf, horizontal_padding, vertical_padding);

    g_object_get (G_OBJECT (pixbuf), 
		  "rowstride", &rowstride,
		  "pixels", &pixels, NULL);

    yuvsize = width*2*height;

    yuvdata = g_malloc (yuvsize);
    halfyuv = yuvsize/2;
    if (gdk_pixbuf_get_has_alpha(pixbuf))
    {
	alphabit = 1;
	rgbpx = 4;
    }
    else
    {
	alphabit = 0;
	rgbpx = 3;
    }
    exc = rowstride - width*rgbpx;

    while(h < height)
    {
	gint w = 0;
	if((h % 2) == 0)
	{
	    while(w < width)
	    {
		r0 = pixels[x];
		g0 = pixels[x+1];
		b0 = pixels[x+2];
		r1 = pixels[x+3+alphabit];
		g1 = pixels[x+4+alphabit];
		b1 = pixels[x+5+alphabit];
		yuvdata[z] = ((r0*-38 + g0*-74 + b0*112 + 128) >> 8) + 128;/*U0*/
		yuvdata[z+1] = ((r0*66 + g0*129 + b0*25 + 128) >> 8) + 16;/*Y0*/
		yuvdata[z+2] = ((r0*112 + g0*-94 + b0*-18 + 128) >> 8) + 128;/*V0*/
		yuvdata[z+3] = ((r1*66 + g1*129 + b1*25 + 128) >> 8) + 16;/*Y1*/
		w += 2;
		z += 4;
		x += rgbpx*2;
	    }
	}
	else
	{
	    while(w < width)
	    {
		r2 = pixels[x];
		g2 = pixels[x+1];
		b2 = pixels[x+2];
		r3 = pixels[x+3+alphabit];
		g3 = pixels[x+4+alphabit];
		b3 = pixels[x+5+alphabit];
		yuvdata[halfyuv+z2] = ((r2*-38 + g2*-74 + b2*112 + 128) >> 8) + 128;/*U1*/
		yuvdata[halfyuv+z2+1] = ((r2*66 + g2*129 + b2*25 + 128) >> 8) + 16;/*Y2*/
		yuvdata[halfyuv+z2+2] = ((r2*112 + g2*-94 + b2*-18 + 128) >> 8) + 128;/*V1*/
		yuvdata[halfyuv+z2+3] = ((r3*66 + g3*129 + b3*25 + 128) >> 8) + 16;/*Y3*/
		w += 2;
		z2 += 4;
		x += rgbpx*2;
	    }
	}
	x += exc;
	h++;
    }
    g_object_unref (pixbuf);
    return yuvdata;
}



static char *
ipod_image_get_ithmb_filename (const char *mount_point, gint format_id, gint index, DbType db_type ) 
{
	gchar *artwork_dir = NULL, *filename, *buf;

	g_return_val_if_fail (mount_point, NULL);
	switch( db_type ) {
	case DB_TYPE_PHOTO:
		artwork_dir = itdb_get_photos_thumb_dir (mount_point);
		if (!artwork_dir)
		{
			/* attempt to create Thumbs dir */
			gchar *photos_dir = itdb_get_photos_dir (mount_point);
			gchar *dir;
			if (!photos_dir)
			{   /* give up */
				return NULL;
			}
			dir = g_build_filename (photos_dir, "Thumbs", NULL);
			mkdir (dir, 0777);
			g_free (dir);
			g_free (photos_dir);

			/* try again */
			artwork_dir = itdb_get_photos_thumb_dir (mount_point);
			if (!artwork_dir)
			{   /* give up */
				return NULL;
			}
		}
		break;
	case DB_TYPE_ITUNES:
	artwork_dir = itdb_get_artwork_dir (mount_point);
	if (!artwork_dir)
	{
		/* attempt to create Artwork dir */
		gchar *control_dir = itdb_get_control_dir (mount_point);
		gchar *dir;
		if (!control_dir)
		{   /* give up */
			return NULL;
		}
		dir = g_build_filename (control_dir, "Artwork", NULL);
		mkdir (dir, 0777);
		g_free (dir);
		g_free (control_dir);

		/* try again */
		artwork_dir = itdb_get_artwork_dir (mount_point);
		if (!artwork_dir)
		{   /* give up */
			return NULL;
		}
	}
	}

	buf = g_strdup_printf ("F%d_%d.ithmb", format_id, index);

	filename = itdb_get_path (artwork_dir, buf);

	/* itdb_get_path() only returns existing paths */
	if (!filename)
	{
	    filename = g_build_filename (artwork_dir, buf, NULL);
	}
/*	printf ("%s %s\n", buf, filename);*/

	g_free (buf);
        g_free (artwork_dir);
	return filename;
}

/* If appropriate, rotate thumb->pixbuf by the value specified in
 * thumb->rotation or thumb->pixbuf's EXIF orientation value. */
static void
ithumb_writer_handle_rotation (Itdb_Thumb *thumb) {
  /* Make sure @rotation is valid (0, 90, 180, 270) */
  thumb->rotation = thumb->rotation % 360;
  thumb->rotation /= 90;
  thumb->rotation *= 90;


  /* If the caller did not specify a rotation, and there is an orientation header
     present in the pixbuf (from EXIF), use that to choose a rotation.
     NOTE: Do this before doing any transforms on the pixbuf, or you will lose
     the EXIF metadata.
     List of orientation values: http://sylvana.net/jpegcrop/exif_orientation.html */
  if (thumb->rotation == 0) {
    /* In GdkPixbuf 2.12 or above, this returns the EXIF orientation value. */
    const char* exif_orientation = gdk_pixbuf_get_option(thumb->pixbuf, "orientation");
    if (exif_orientation != NULL) {
      switch (exif_orientation[0]) {
	case '3':
	  thumb->rotation = 180;
          break;
	case '6':
	  thumb->rotation = 270;
          break;
	case '8':
	  thumb->rotation = 90;
          break;
	/* '1' means no rotation.  The other four values are all various
	   transpositions, which are rare in real photos so we don't
	   implement them. */
      }
    }
  }

  /* Rotate if necessary */
  if (thumb->rotation != 0)
  {
      GdkPixbuf *new_pixbuf = gdk_pixbuf_rotate_simple (thumb->pixbuf, thumb->rotation);
      g_object_unref (thumb->pixbuf);
      thumb->pixbuf = new_pixbuf;
      /* Clean up */
      thumb->rotation = 0;
  }
}

/* On the iPhone, thumbnails are presented as squares in a grid.
   In order to fit the grid, they have to be cropped as well as
   scaled. */
static GdkPixbuf *
ithumb_writer_scale_and_crop (Itdb_Thumb *thumb,
                              gint width, gint height,
                              gboolean crop)
{
    gint input_width, input_height;
    double width_scale, height_scale;
    gdouble scale;
    gint offset_x, offset_y;
    gint border_width = 0;

    GdkPixbuf *input_pixbuf, *output_pixbuf;

    input_pixbuf = GDK_PIXBUF(thumb->pixbuf);
    g_object_get (G_OBJECT (input_pixbuf), 
		  "width", &input_width,
		  "height", &input_height,
		  NULL);

    width_scale = (double) width / input_width;
    height_scale = (double) height / input_height;

    if (!crop)
    {
      /* If we're not cropping, we need to be able to fit both the whole width
         and whole height, so we use the smaller of the two possible scale
         factors. */
      scale = MIN(width_scale, height_scale);
      offset_x = offset_y = 0;
    }
    else 
    {
      double scaled_width, scaled_height;
      /* If we are cropping, we use the max of the two possible scale factors,
         so that the image is large enough to fill both dimensions. */
      scale = MAX(width_scale, height_scale);

      /* In order to crop the image, we shift it either left or up by
	 a certain amount.  Note that the offset args to gdk_pixbuf_scale are
	 expressed in terms of the *output* pixbuf, not the input, so we scal the
	 offsets.  Here we figure out whether this is a vertical or horizontal
	 offset. */
      scaled_width = input_width * scale;
      scaled_height = input_height * scale;
      offset_x = round((width - scaled_width) / 2);
      offset_y = round((height - scaled_height) / 2);

      g_assert(round(scaled_width) == width ||
               round(scaled_height) == height);
    }

    output_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
			           width + border_width,
                                   height + border_width);
    gdk_pixbuf_fill(output_pixbuf, 0xffffffff);

    gdk_pixbuf_scale (input_pixbuf,
		      output_pixbuf,
		      0, 0,			 /* dest x, dest y */
		      width,                     /* dest width */
		      height,                    /* dest height */
		      offset_x, offset_y,        /* offset x, offset y */
		      scale, scale,	         /* scale x, scale y */
		      GDK_INTERP_BILINEAR);

    return output_pixbuf;
}

static void *pack_thumbnail (iThumbWriter *writer, Itdb_Thumb *thumb,
                             GdkPixbuf *pixbuf)
{
    typedef void *(*PackerFunc)(GdkPixbuf *pixbuf,
                                const Itdb_ArtworkFormat *img_info,
                                gint horizontal_padding, gint vertical_padding,
                                guint32 *thumb_size);
    struct Packer {
        ItdbThumbFormat format;
        PackerFunc packer;
    };
    guint i;
    const struct Packer packers[] = {
        { THUMB_FORMAT_RGB565_LE_90,     (PackerFunc)pack_RGB_565 },
        { THUMB_FORMAT_RGB565_BE_90,     (PackerFunc)pack_RGB_565 },
        { THUMB_FORMAT_RGB565_LE,        (PackerFunc)pack_RGB_565 },
        { THUMB_FORMAT_RGB565_BE,        (PackerFunc)pack_RGB_565 },
        { THUMB_FORMAT_RGB555_LE_90,     (PackerFunc)pack_RGB_555 },
        { THUMB_FORMAT_RGB555_BE_90,     (PackerFunc)pack_RGB_555 },
        { THUMB_FORMAT_RGB555_LE,        (PackerFunc)pack_RGB_555 },
        { THUMB_FORMAT_RGB555_BE,        (PackerFunc)pack_RGB_555 },
        { THUMB_FORMAT_RGB888_LE_90,     (PackerFunc)pack_RGB_888 },
        { THUMB_FORMAT_RGB888_BE_90,     (PackerFunc)pack_RGB_888 },
        { THUMB_FORMAT_RGB888_LE,        (PackerFunc)pack_RGB_888 },
        { THUMB_FORMAT_RGB888_BE,        (PackerFunc)pack_RGB_888 },
        { THUMB_FORMAT_REC_RGB555_LE_90, (PackerFunc)pack_rec_RGB_555 },
        { THUMB_FORMAT_REC_RGB555_BE_90, (PackerFunc)pack_rec_RGB_555 },
        { THUMB_FORMAT_REC_RGB555_LE,    (PackerFunc)pack_rec_RGB_555 },
        { THUMB_FORMAT_REC_RGB555_BE,    (PackerFunc)pack_rec_RGB_555 },
        { THUMB_FORMAT_EXPERIMENTAL_LE,  NULL },
        { THUMB_FORMAT_EXPERIMENTAL_BE,  NULL },
        { THUMB_FORMAT_UYVY_BE,          (PackerFunc)pack_UYVY },
        { THUMB_FORMAT_UYVY_LE,          (PackerFunc)pack_UYVY },
        { THUMB_FORMAT_I420_BE,          (PackerFunc)pack_I420 },
        { THUMB_FORMAT_I420_LE,          (PackerFunc)pack_I420 }
    };

    for (i = 0; i < G_N_ELEMENTS (packers); i++) {
        if (packers[i].format == writer->img_info->format) {
            break;
        }
    }

    if ((i == G_N_ELEMENTS (packers)) || (packers[i].packer == NULL)) {
        return NULL;
    }
    return packers[i].packer (pixbuf, writer->img_info,
                              thumb->horizontal_padding,
                              thumb->vertical_padding,
                              &thumb->size);
}
static gboolean write_pixels (iThumbWriter *writer, Itdb_Thumb *thumb,
                              void *pixels)
{
    if (pixels == NULL)
    {
	return FALSE;
    }

    if (fwrite (pixels, thumb->size, 1, writer->f) != 1) {
	g_print ("Error writing to file: %s\n", strerror (errno));
	return FALSE;
    }
    writer->cur_offset += thumb->size;

    if (writer->img_info->padding != 0)
    {
	gint padding = writer->img_info->padding - thumb->size;
	g_return_val_if_fail (padding >= 0, TRUE);
	if (padding != 0)
	{
            /* FIXME: check if a simple fseek() will do the same */
	    gchar *pad_bytes = g_malloc0 (padding);
	    if (fwrite (pad_bytes, padding, 1, writer->f) != 1) {
		g_free (pad_bytes);
		g_print ("Error writing to file: %s\n", strerror (errno));
		return FALSE;
	    }
	    g_free (pad_bytes);
	    writer->cur_offset += padding;
	}
    }
    return TRUE;
}

static char *get_ithmb_filename (iThumbWriter *writer, Itdb_Thumb *thumb)
{
    switch (thumb->type)
    {
    case ITDB_THUMB_PHOTO_LARGE:
    case ITDB_THUMB_PHOTO_SMALL:
    case ITDB_THUMB_PHOTO_FULL_SCREEN:
    case ITDB_THUMB_PHOTO_TV_SCREEN:
	return g_strdup_printf (":Thumbs:F%d_%d.ithmb", 
	            		writer->img_info->format_id,
                                writer->current_file_index);
	break;
    case ITDB_THUMB_COVER_LARGE:
    case ITDB_THUMB_COVER_SMALL:
    case ITDB_THUMB_COVER_XLARGE:
    case ITDB_THUMB_COVER_MEDIUM:
    case ITDB_THUMB_COVER_SMEDIUM:
    case ITDB_THUMB_COVER_XSMALL:
	return g_strdup_printf (":F%d_%d.ithmb", 
	    		        writer->img_info->format_id,
                                writer->current_file_index);
	break;
    }
    g_return_val_if_reached (NULL);
}

static void set_thumb_padding (iThumbWriter *writer, Itdb_Thumb *thumb, 
                               gint width, gint height)
{
    switch (writer->db_type)
    {
    case DB_TYPE_PHOTO:
	thumb->horizontal_padding = (writer->img_info->width - width)/2;
	thumb->vertical_padding = (writer->img_info->height - height)/2;
	break;
    case DB_TYPE_ITUNES:
	/* IPOD_COVER_LARGE will be centered automatically using
	   the info in mhni->width/height. Free space around
	   IPOD_COVER_SMALL will be used to display track
	   information -> no padding (tested on iPod
	   Nano). mhni->hor_/ver_padding is working */
	thumb->horizontal_padding = 0;
	thumb->vertical_padding = 0;
	break;
    default:
	g_return_if_reached ();
    }
}

static GdkPixbuf *pixbuf_from_image_data (guchar *image_data, gsize len)
{
    GdkPixbuf *pixbuf;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new ();
    g_return_val_if_fail (loader, FALSE);
    gdk_pixbuf_loader_write (loader, image_data, len, NULL);
    gdk_pixbuf_loader_close (loader, NULL);
    pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
    if (pixbuf)
        g_object_ref (pixbuf);
    g_object_unref (loader);

    return pixbuf;
}

static gboolean
ithumb_writer_write_thumbnail (iThumbWriter *writer, 
			       Itdb_Thumb *thumb)
{
    GdkPixbuf *pixbuf = NULL;
    void *pixels = NULL;
    gint width, height; /* must be gint -- see comment below */
    gboolean result;

    g_return_val_if_fail (writer, FALSE);
    g_return_val_if_fail (writer->img_info, FALSE);
    g_return_val_if_fail (thumb, FALSE);

    /* An thumb can start with one of:
        1. a filename
        2. raw image data read from a file
        3. a GdkPixbuf struct
       In case 1 and 2, we load the relevant data into a GdkPixbuf and proceed
       with case 3.
    */
    if (thumb->filename)
    {   /* read image from filename */
	thumb->pixbuf = gdk_pixbuf_new_from_file (thumb->filename, NULL);
	g_free (thumb->filename);
	thumb->filename = NULL;
    } 
    else if (thumb->image_data)
    {   /* image data is stored in image_data and image_data_len */
        thumb->pixbuf = pixbuf_from_image_data (thumb->image_data, 
                                                thumb->image_data_len);
	g_free (thumb->image_data);
	thumb->image_data = NULL;
	thumb->image_data_len = 0;
    }

    if (thumb->pixbuf == NULL)
    {
	/* This is quite bad... if we just return FALSE the ArtworkDB
	   gets messed up. */
	thumb->pixbuf = gdk_pixbuf_from_pixdata (&questionmark_pixdata, FALSE, NULL);

	if (!thumb->pixbuf)
	{
	    /* Somethin went wrong. let's insert a red thumbnail */
	    thumb->pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8,
				     writer->img_info->width,
				     writer->img_info->height);
	    gdk_pixbuf_fill (thumb->pixbuf, 0xff000000);
	}
	/* avoid rotation */
	thumb->rotation = 0;
    }

    g_assert(thumb->pixbuf);

    ithumb_writer_handle_rotation(thumb);

    /* If we rotate by 90 or 270 degrees interchange the width and
     * height */
    if ((thumb->rotation == 0) || (thumb->rotation == 180))
    {
	width = writer->img_info->width;
	height = writer->img_info->height;
    }
    else
    {
	width = writer->img_info->height;
	height = writer->img_info->width;
    }

    pixbuf = ithumb_writer_scale_and_crop (thumb, width, height,
                                           writer->img_info->crop);
    g_object_unref (thumb->pixbuf);
    thumb->pixbuf = NULL;

    /* !! cannot write directly to &thumb->width/height because
       g_object_get() returns a gint, but thumb->width/height are
       gint16 !! */
    g_object_get (G_OBJECT (pixbuf), 
		  "width", &width,
		  "height", &height,
		  NULL);

    set_thumb_padding (writer, thumb, width, height);

    /* The thumbnail width/height is inclusive padding */
    thumb->width = thumb->horizontal_padding + width;
    thumb->height = thumb->vertical_padding + height;
    thumb->offset = writer->cur_offset;

    pixels = pack_thumbnail (writer, thumb, pixbuf);
    g_object_unref (G_OBJECT (pixbuf));

    thumb->filename = get_ithmb_filename (writer, thumb);
    result = write_pixels (writer, thumb, pixels);
    g_free (pixels);

    return result;
}

static gboolean
ithumb_writer_update (iThumbWriter *writer)
{
    while ((writer->f == NULL) || (writer->cur_offset >= ITHUMB_MAX_SIZE))
    {
	if (writer->f)
	{
	    fclose (writer->f);
	    writer->f = NULL;
	}
	g_free (writer->filename);
	writer->filename = NULL;

	/* increment index for filename */
	++writer->current_file_index;

	writer->filename =
	    ipod_image_get_ithmb_filename (writer->mountpoint, 
					   writer->img_info->format_id,
					   writer->current_file_index, 
					   writer->db_type);
	if (writer->filename == NULL)
	{
	    return FALSE;
	}
	writer->f = fopen (writer->filename, "ab");
	if (writer->f == NULL)
	{
	    g_print ("Error opening %s: %s\n", writer->filename, strerror (errno));
	    g_free (writer->filename);
	    writer->filename = NULL;
	    return FALSE;
	}
	writer->cur_offset = ftell (writer->f);
    }

    return TRUE;
}



static void
write_thumbnail (gpointer _writer, gpointer _artwork)
{
	iThumbWriter *writer = _writer;
	Itdb_Artwork *artwork = _artwork;
 	Itdb_Thumb *thumb;

	thumb = itdb_artwork_get_thumb_by_type (artwork,
						writer->img_info->type);

	/* size == 0 indicates a thumbnail not yet written to the
	   thumbnail file */
	if (thumb && (thumb->size == 0))
	{
	    /* check if new thumbnail file has to be started */
	    if (ithumb_writer_update (writer))
		ithumb_writer_write_thumbnail (writer, thumb);
	}
}


static void
ithumb_writer_free (iThumbWriter *writer)
{
	g_return_if_fail (writer != NULL);
	if (writer->f)
	{
	    fclose (writer->f);
	    if (writer->filename && (writer->cur_offset == 0))
	    {   /* Remove empty file */
		unlink (writer->filename);
	    }
	}
	g_free (writer->filename);
	g_free (writer->mountpoint);
	g_free (writer);
}


static iThumbWriter *
ithumb_writer_new (const char *mount_point,
		   const Itdb_ArtworkFormat *info,
		   DbType db_type,
		   guint byte_order)
{
	iThumbWriter *writer;

	writer = g_new0 (iThumbWriter, 1);

	writer->img_info = info;

	writer->byte_order = byte_order;
	writer->db_type = db_type;
	writer->mountpoint = g_strdup (mount_point);
	writer->current_file_index = 0;

	if (!ithumb_writer_update (writer))
	{
	    ithumb_writer_free (writer);
	    return NULL;
	}

	return writer;
}

gint offset_sort (gconstpointer a, gconstpointer b);
gint offset_sort (gconstpointer a, gconstpointer b)
{
    return (-(((Itdb_Thumb *)a)->offset - ((Itdb_Thumb *)b)->offset));
}

static gboolean ithumb_rearrange_thumbnail_file (gpointer _key,
						 gpointer _thumbs,
						 gpointer _user_data)
{
    const gchar *filename = _key;
    GList *thumbs = _thumbs;
    gboolean *result = _user_data;
    gint fd = -1;
    guint32 size = 0;
    GList *gl;
    struct stat statbuf;
    guint32 offset;
    void *buf = NULL;

/*     printf ("%s: %d\n", filename, g_list_length (thumbs)); */

    /* check if an error occured */
    if (*result == FALSE)
	goto out;

    if (thumbs == NULL)
    {   /* no thumbnails for this file --> remove altogether */
	if (unlink (filename) == -1)
	{
	    *result = FALSE;
	    goto out;
	}
    }

    /* check if all thumbnails have the same size */
    for (gl=thumbs; gl; gl=gl->next)
    {
	Itdb_Thumb *img = gl->data;

	if (size == 0)
	    size = img->size;
	if (size != img->size)
	{
	    *result = FALSE;
	    goto out;
	}
    }
    if (size == 0)
    {
	*result = FALSE;
	goto out;
    }

    /* OK, all thumbs are the same size @size, let's see how many
     * thumbnails are in the actual file */
    if (g_stat  (filename, &statbuf) != 0)
    {
	*result = FALSE;
	goto out;
    }

    /* check if the file size is a multiple of @size */
    if (((guint32)(statbuf.st_size / size))*size != statbuf.st_size)
    {
	*result = FALSE;
	goto out;
    }

    fd = open (filename, O_RDWR, 0);
    if (fd == -1)
    {
	*result = FALSE;
	goto out;
    }

    /* size is either a value coming from a hardcoded const array from 
     * libipoddevice, or a guint32 read from an iPod file, so no overflow
     * can occur here
     */
    buf = g_malloc (size);

    /* Sort the list of thumbs in reverse order of img->offset */
    thumbs = g_list_sort (thumbs, offset_sort);

    gl = g_list_last (thumbs);

    /* check each thumbnail slot */
    for (offset=0; gl && (offset<statbuf.st_size); offset+=size)
    {
	Itdb_Thumb *thumb = gl->data;
	g_return_val_if_fail (thumb, FALSE);

	/* Try to find a thumbnail that uses this slot */
	while ((gl != NULL) && (thumb->offset < offset))
	{
	    gl = gl->prev;
	    if (gl)
		thumb = gl->data;
	    g_return_val_if_fail (thumb, FALSE);
	}

	if (!gl)
	    break;  /* offset now indicates new length of file */

	if (thumb->offset > offset)
	{
	    /* did not find a thumbnail with matching offset -> copy
	       data from last slot (== first element) */
	    GList *first_gl = g_list_first (thumbs);
	    Itdb_Thumb *first_thumb = first_gl->data;
	    guint32 first_offset;

	    g_return_val_if_fail (first_thumb, FALSE);
	    first_offset = first_thumb->offset;

	    /* actually copy the data */
	    if (lseek (fd, first_offset, SEEK_SET) != first_offset)
	    {
		*result = FALSE;
		goto out;
	    }
	    if (read (fd, buf, size) != size)
	    {
		*result = FALSE;
		goto out;
	    }
	    if (lseek (fd, offset, SEEK_SET) != offset)
	    {
		*result = FALSE;
		goto out;
	    }
	    if (write (fd, buf, size) != size)
	    {
		*result = FALSE;
		goto out;
	    }

	    /* Adjust offset of all thumbnails whose offset is
	       first_offset. Since the list is sorted, they are all at
	       the beginning of the list. */
	    while (first_thumb->offset == first_offset)
	    {
		first_thumb->offset = offset;
		/* There's a possibility that gl is the first
		   element. In that case don't attempt to move it (it
		   wouldn't work as intended because we access
		   gl->next after removing it from the list) */
		if (gl != first_gl)
		{
		    thumbs = g_list_delete_link (thumbs, first_gl);
		    /* Insert /behind/ gl */
		    thumbs = g_list_insert_before (thumbs,
						   gl->next, first_thumb);
		    first_gl = g_list_first (thumbs);
		    first_thumb = first_gl->data;
		    g_return_val_if_fail (first_thumb, FALSE);
		}
	    }
	}
    }
    /* offset corresponds to the new length of the file */
    if (offset > 0)
    {   /* Truncate */
	if (ftruncate (fd, offset) == -1)
	{
	    *result = FALSE;
	    goto out;
	}
    }
    else
    {   /* Remove file altogether */
	close (fd);
	fd = -1;
	if (unlink (filename) == -1)
	{
	    *result = FALSE;
	    goto out;
	}
    }

  out:
    if (fd != -1) close (fd);
    g_free (buf);
    g_list_free (thumbs);
    return TRUE;
}


/* The actual image data of thumbnails is not read into memory. As a
   consequence, writing the thumbnail file is not as straight-forward
   as e.g. writing the iTunesDB where all data is held in memory.

   To avoid the need to read large amounts from the iPod and back, or
   have to large files exist on the iPod (reading from the original
   thumbnail fail and writing to the new thumbnail file), the
   modifications are done in place.

   It is assumed that all thumbnails have the same data size. If not,
   FALSE is returned.

   If a thumbnail has been removed, a slot in the file is opened. This
   slot is filled by copying data from the end of the file and
   adjusting the corresponding Itdb_Image offset pointer. When all
   slots are filled, the file is truncated to the new length.
*/
static gboolean
ithmb_rearrange_existing_thumbnails (Itdb_DB *db,
				     const Itdb_ArtworkFormat *info)
{
    GList *gl;
    GHashTable *filenamehash;
    gboolean result = TRUE;
    GList *thumbs;
    gint i;
    gchar *filename;
    const gchar *mountpoint;

    g_return_val_if_fail (db, FALSE);
    g_return_val_if_fail (info, FALSE);
    g_return_val_if_fail (db_get_device(db), FALSE);

    mountpoint = db_get_mountpoint (db);

    g_return_val_if_fail (mountpoint, FALSE);

    filenamehash = g_hash_table_new_full (g_str_hash, g_str_equal, 
					  g_free, NULL);

    /* Create a hash with all filenames used for thumbnails.
       This will usually be a number of "F%d_%d.ithmb" files. A
       GList is kept with pointers to all images in a given file which
       allows to adjust the offset pointers */
	switch (db->db_type) {
	case DB_TYPE_ITUNES:
		for (gl=db_get_itunesdb(db)->tracks; gl; gl=gl->next)
		{
			Itdb_Thumb *thumb;
			Itdb_Track *track = gl->data;
			g_return_val_if_fail (track, FALSE);

			thumb = itdb_artwork_get_thumb_by_type (track->artwork,
					info->type);
			if (thumb && thumb->filename && (thumb->size != 0))
			{
				filename = itdb_thumb_get_filename (
				        db_get_device(db),
				        thumb);
				if (filename)
				{
					thumbs = g_hash_table_lookup (filenamehash, filename);
					thumbs = g_list_append (thumbs, thumb);
					g_hash_table_insert (filenamehash, filename, thumbs);
				}
			}
		}
		break;
    case DB_TYPE_PHOTO:
	for (gl=db_get_photodb(db)->photos; gl; gl=gl->next)
	{
		Itdb_Thumb *thumb;
		Itdb_Artwork *artwork = gl->data;

		thumb = itdb_artwork_get_thumb_by_type (artwork,
				info->type);
		if (thumb && thumb->filename && (thumb->size != 0))
		{
			filename = itdb_thumb_get_filename (
			        db_get_device (db),
			        thumb);
			if (filename)
			{
				thumbs = g_hash_table_lookup (filenamehash, filename);
				thumbs = g_list_append (thumbs, thumb);
				g_hash_table_insert (filenamehash, filename, thumbs);
			}
		}
	}
	break;
    default:
	g_return_val_if_reached (FALSE);
    }

    /* Check for files present on the iPod but no longer referenced by
       thumbs */

    for (i=0; i<50; ++i)
    {
	filename = ipod_image_get_ithmb_filename (mountpoint,
						  info->format_id,
						  i,
						  db->db_type);
	if (g_file_test (filename, G_FILE_TEST_EXISTS))
	{
	    if (g_hash_table_lookup (filenamehash, filename) == NULL)
	    {
		g_hash_table_insert (filenamehash,
				     g_strdup (filename), NULL);
	    }
	}
	g_free (filename);
    }

    /* I'm using the _foreach_remove variant here because the
       thumbnail GList may get changed while calling
       ithumb_rearrange_thumbnail_file but cannot be written back into
       the hash table. Using the _foreach_remove variant here will
       ensure that it will not be possible to access the invalid
       thumbnail GList. The only proper operation after the
       _foreach_remove is a call to g_hash_table_destroy().
       For the same reasons the thumb GList gets free'd in
       ithumb_rearrange_thumbnail_file() */
    g_hash_table_foreach_remove (filenamehash,
				 ithumb_rearrange_thumbnail_file, &result);
    g_hash_table_destroy (filenamehash);

    return result;
}

#endif

G_GNUC_INTERNAL int
itdb_write_ithumb_files (Itdb_DB *db) 
{
#ifdef HAVE_GDKPIXBUF
	GList *writers;
	GList *it;
	Itdb_Device *device;
	const Itdb_ArtworkFormat *format;
	const gchar *mount_point;

	g_return_val_if_fail (db, -1);
	device = db_get_device(db);
	g_return_val_if_fail (device, -1);

	mount_point = db_get_mountpoint (db);
	/* FIXME: support writing to directory rather than writing to
	   iPod */
	if (mount_point == NULL)
	    return -1;
	
	format = itdb_device_get_artwork_formats (device);
	if (format == NULL) {
		return -1;
	}
	writers = NULL;
	while (format->type != -1) {
		iThumbWriter *writer;

		if (itdb_thumb_type_is_valid_for_db (format->type, db->db_type))
		{
		    ithmb_rearrange_existing_thumbnails (db, format );
		    writer = ithumb_writer_new (mount_point, 
						format,
						db->db_type, 
						device->byte_order);
		    if (writer != NULL) {
			writers = g_list_prepend (writers, writer);
		    }
		}
		format++;
	}
	if (writers == NULL) {
		return -1;
	}
	switch (db->db_type) {
	case DB_TYPE_ITUNES:
		for (it = db_get_itunesdb(db)->tracks; it != NULL; it = it->next) {
			Itdb_Track *track;

			track = it->data;
			g_return_val_if_fail (track, -1);

			g_list_foreach (writers, write_thumbnail, track->artwork);
		}
		break;
	case DB_TYPE_PHOTO:
		for (it = db_get_photodb(db)->photos; it != NULL; it = it->next) {
			Itdb_Artwork *photo;

			photo = it->data;
			g_return_val_if_fail (photo, -1);

			g_list_foreach (writers, write_thumbnail, photo);
		}
		break;
	default:
	        g_return_val_if_reached (-1);
	}
	
	g_list_foreach (writers, (GFunc)ithumb_writer_free, NULL);
	g_list_free (writers);

	return 0;
#else
    return -1;
#endif
}
