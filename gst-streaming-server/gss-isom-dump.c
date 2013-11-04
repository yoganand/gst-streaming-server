/* GStreamer Streaming Server
 * Copyright (C) 2013 Rdio Inc <ingestions@rd.io>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include <gst/gst.h>

#include "gss-isom.h"
#include "gss-isom-boxes.h"

#include <string.h>
#include <stdlib.h>


#ifdef unused
static void
gss_isom_stsc_dump (GssBoxStsc * stsc)
{
  int i;

  g_print ("stsc:\n");
  g_print ("  entry_count: %d\n", stsc->entry_count);
  g_print ("    index: first_chunk samples_per_chunk desc_index\n");
  for (i = 0; i < stsc->entry_count; i++) {
    g_print ("    %d: %d %d %d\n", i,
        stsc->entries[i].first_chunk,
        stsc->entries[i].samples_per_chunk,
        stsc->entries[i].sample_description_index);
  }
}
#endif

void
gss_isom_track_dump (GssIsomTrack * track)
{
  /* trak */
  g_print ("  atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('t', 'r', 'a', 'k')));

  g_print ("    ignore: tkhd\n");
  //gss_isom_tkhd_dump (&track->tkhd);
  g_print ("    ignore: tref\n");
  //gss_isom_tref_dump (&track->tref);

  /* trak/mdia */
  g_print ("    atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('m', 'd', 'i', 'a')));
  g_print ("      ignore: mdhd\n");
  //gss_isom_mdhd_dump (&track->mdhd);
  g_print ("      ignore: hdlr\n");
  //gss_isom_hdlr_dump (&track->hdlr);

  /* trak/mdia/minf */
  g_print ("      atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('m', 'i', 'n', 'f')));
  g_print ("        ignore: vmhd\n");
  //gss_isom_vmhd_dump (&track->vmhd);
  g_print ("        ignore: smhd\n");
  //gss_isom_smhd_dump (&track->smhd);
  g_print ("        ignore: hmhd\n");
  //gss_isom_hmhd_dump (&track->hmhd);

  /* trak/mdia/minf/dinf */
  g_print ("        atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('d', 'i', 'n', 'f')));
  g_print ("          ignore: dref\n");
  //gss_isom_dref_dump (&track->dref);   /* */

  /* trak/mdia/minf/stbl */
  g_print ("        atom: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('s', 't', 'b', 'l')));
  g_print ("    ignore: stsd\n");
  //gss_isom_stsd_dump (track);
  g_print ("    ignore: stts\n");
  //gss_isom_stts_dump (&track->stts);
  g_print ("    ignore: stsc\n");
  //gss_isom_stsc_dump (&track->stsc);
  g_print ("    ignore: stsz\n");
  //gss_isom_stsz_dump (&track->stsz);
  g_print ("    ignore: stco\n");
  //gss_isom_stco_dump (&track->stco);

#if 0
  if (0) {
    gss_isom_ctts_dump (&track->ctts);
    gss_isom_stss_dump (&track->stss);
    gss_isom_stsh_dump (&track->stsh);
    gss_isom_stdp_dump (&track->stdp, track);
  }
#endif

#if 0
  /* trak/mdia/minf/stsd */
  offset_stsd = BOX_INIT (bw, GST_MAKE_FOURCC ('s', 't', 's', 'd'));
  gss_isom_mp4v_dump (&track->mp4v);
  gss_isom_mp4a_dump (&track->mp4a);
  gss_isom_esds_dump (&track->esds);
#endif
}

static void
gss_isom_mvhd_dump (GssBoxMvhd * mvhd)
{
  g_print ("  atom: mvhd\n");
  g_print ("    version: 0x%08x\n", mvhd->version);
  g_print ("    flags: 0x%08x\n", mvhd->flags);
  g_print ("    creation_time: 0x%08lx\n", mvhd->creation_time);
  g_print ("    modification_time: 0x%08lx\n", mvhd->modification_time);
  g_print ("    timescale: %d\n", mvhd->timescale);
  g_print ("    duration: %ld\n", mvhd->duration);

  g_print ("    unknown: 0x%08x\n", 0x00010000);
  g_print ("    unknown: 0x%08x\n", 0x0100);
  g_print ("    unknown: 0x%08x\n", 0x0000);

  /* matrix */
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00010000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00010000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);
  g_print ("    matrix[0]: 0x%08x\n", 0x00000000);

  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x40000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0x00000000);

  g_print ("    track_id: %d\n", mvhd->next_track_id);

  /* FIXME for ccff */
  g_print ("    unknown: 0x%08x\n", 0x00000000);
  g_print ("    unknown: 0x%08x\n", 0xffffffff);

}

static void
gss_isom_moov_dump (GssIsomMovie * movie)
{
  int i;

  g_print ("atom: moov\n");
  gss_isom_mvhd_dump (&movie->mvhd);
#if 0
  gss_isom_ainf_dump (&movie->ainf);
  gss_isom_iods_dump (&movie->iods);
#endif
  for (i = 0; i < movie->n_tracks; i++) {
    gss_isom_track_dump (movie->tracks[i]);
  }
}

static void
gss_isom_mfhd_dump (GssBoxMfhd * mfhd)
{
  g_print ("  mfhd:\n");
}

static void
gss_isom_tfhd_dump (GssBoxTfhd * tfhd)
{
  g_print ("  tfhd:\n");
}

static void
gss_isom_trun_dump (GssBoxTrun * trun)
{
  int i;

  g_print ("trun:\n");
  g_print ("  version: %d\n", trun->version);
  g_print ("  flags: %06x\n", trun->flags);
  g_print ("  sample_count: %d\n", trun->sample_count);
  if (trun->flags & TR_DATA_OFFSET) {
    g_print ("  data_offset: unknown\n");
  }
  if (trun->flags & TR_FIRST_SAMPLE_FLAGS) {
    g_print ("  first_sample_flags: %08x\n", trun->first_sample_flags);
  }
  g_print ("  samples:\n");
  for (i = 0; i < trun->sample_count; i++) {
    if (trun->flags & TR_SAMPLE_DURATION) {
      g_print ("    duration: %d\n", trun->samples[i].duration);
    }
    if (trun->flags & TR_SAMPLE_SIZE) {
      g_print ("    size: %d\n", trun->samples[i].size);
    }
    if (trun->flags & TR_SAMPLE_FLAGS) {
      g_print ("    flags: %08x\n", trun->samples[i].flags);
    }
    if (trun->flags & TR_SAMPLE_COMPOSITION_TIME_OFFSETS) {
      g_print ("    cto: %d\n", trun->samples[i].composition_time_offset);
    }
  }

}

static void
gss_isom_sdtp_dump (GssBoxSdtp * sdtp)
{
  g_print ("  sdtp:\n");
}

static void
gss_isom_sample_encryption_dump (GssBoxUUIDSampleEncryption * se)
{
  g_print ("  UUID sample encryption:\n");
}

static void
gss_isom_avcn_dump (GssBoxAvcn * avcn)
{
  g_print ("  avcn:\n");
}

static void
gss_isom_tfdt_dump (GssBoxTfdt * tfdt)
{
  g_print ("  tfdt:\n");
}

static void
gss_isom_trik_dump (GssBoxTrik * trik)
{
  g_print ("  trik:\n");
}

static void
gss_isom_saiz_dump (GssBoxSaiz * saiz)
{
  g_print ("  saiz:\n");
}

static void
gss_isom_saio_dump (GssBoxSaio * saio)
{
  g_print ("  saio:\n");
}

void
gss_isom_fragment_dump (GssIsomFragment * fragment)
{

  gss_isom_mfhd_dump (&fragment->mfhd);
  gss_isom_tfhd_dump (&fragment->tfhd);
  gss_isom_trun_dump (&fragment->trun);
  gss_isom_sdtp_dump (&fragment->sdtp);
  gss_isom_sample_encryption_dump (&fragment->sample_encryption);
  gss_isom_avcn_dump (&fragment->avcn);
  gss_isom_tfdt_dump (&fragment->tfdt);
  gss_isom_trik_dump (&fragment->trik);
  gss_isom_saiz_dump (&fragment->saiz);
  gss_isom_saio_dump (&fragment->saio);

}

void
gss_isom_parser_dump (GssIsomParser * parser)
{
  gss_isom_movie_dump (parser->movie);
}

void
gss_isom_movie_dump (GssIsomMovie * movie)
{
  g_print ("atom: ftyp\n");
  g_print ("  file type: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('c', 'c', 'f', 'f')));
  g_print ("  unknown: %d\n", 0x00000001);
  g_print ("  compat: %" GST_FOURCC_FORMAT "\n",
      GST_FOURCC_ARGS (GST_MAKE_FOURCC ('i', 's', 'o', '6')));

  gss_isom_moov_dump (movie);

}
