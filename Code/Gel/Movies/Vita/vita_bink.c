/*****************************************************************************
**  THUG-Vita -- lecteur Bink 1 (FMV)                                       **
**  Code/Gel/Movies/Vita/vita_bink.c                                        **
**                                                                          **
**  Demux + decodage par un FFmpeg minimal lie en statique. Mesures sur Mac **
**  (M5, C pur --disable-asm) : 0,73 ms/image video, 0,6 ms par seconde     **
**  d'audio ; 6,2 M instructions AArch64 par image. Sur Vita, estimation    **
**  18-30 ms/image a 444 MHz : A MESURER sur la console avant integration.  **
**                                                                          **
**  Lecture fichier : AVIOContext perso sur sceIo, tampon 256 Ko. Debit     **
**  des videos : ~800 Ko/s (6,4 Mbit/s), une lecture tous les ~10 images.   **
**                                                                          **
**  Branche par vita/CMakeLists.txt (THUG_FMV). Derive de FFmpeg (LGPL 2.1+).**
*****************************************************************************/

#include "vita_bink.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

#define TAMPON_IO		( 256 * 1024 )
#define PCM_MAX			8192		/* echantillons par canal par paquet audio, large */

struct VitaBink
{
	AVFormatContext		*fc;
	AVIOContext			*io;
	AVCodecContext		*vc, *ac;
	AVPacket			*pkt;
	AVFrame				*vfr, *afr;
	int					 vs, as;
	int					 n;
	VitaBinkAudioCb		 cb;
	void				*user;
	int					 vol;		/* 0..65536 */
	short				*pcm;
#ifdef __vita__
	SceUID				 fd;
#else
	FILE				*fp;
#endif
};

/* -- E/S ----------------------------------------------------------------- */

static int io_lire( void *o, uint8_t *buf, int n )
{
	VitaBink *b = (VitaBink *)o;
#ifdef __vita__
	int r = sceIoRead( b->fd, buf, n );
	return r > 0 ? r : AVERROR_EOF;
#else
	size_t r = fread( buf, 1, (size_t)n, b->fp );
	return r ? (int)r : AVERROR_EOF;
#endif
}

static int64_t io_seek( void *o, int64_t off, int whence )
{
	VitaBink *b = (VitaBink *)o;
#ifdef __vita__
	if( whence == AVSEEK_SIZE )
	{
		SceOff cur = sceIoLseek( b->fd, 0, SCE_SEEK_CUR );
		SceOff end = sceIoLseek( b->fd, 0, SCE_SEEK_END );
		sceIoLseek( b->fd, cur, SCE_SEEK_SET );
		return end;
	}
	return sceIoLseek( b->fd, off, whence & 3 );
#else
	if( whence == AVSEEK_SIZE )
	{
		long cur = ftell( b->fp );
		fseek( b->fp, 0, SEEK_END );
		long end = ftell( b->fp );
		fseek( b->fp, cur, SEEK_SET );
		return end;
	}
	return fseek( b->fp, (long)off, whence & 3 ) ? -1 : ftell( b->fp );
#endif
}

/* -- Audio : FLTP (sortie de binkaudio_dct) -> s16 entrelace -------------- */

static void pousse_audio( VitaBink *b, const AVFrame *f )
{
	if( !b->cb )
		return;
	const int ch = f->ch_layout.nb_channels;
	const int n  = f->nb_samples < PCM_MAX ? f->nb_samples : PCM_MAX;
	const float *g = (const float *)f->extended_data[0];
	const float *d = (const float *)f->extended_data[ch > 1 ? 1 : 0];
	for( int i = 0; i < n; ++i )
	{
		int l = (int)( g[i] * 32767.0f );
		int r = (int)( d[i] * 32767.0f );
		l = ( l * b->vol ) >> 16;
		r = ( r * b->vol ) >> 16;
		if( l > 32767 ) l = 32767; else if( l < -32768 ) l = -32768;
		if( r > 32767 ) r = 32767; else if( r < -32768 ) r = -32768;
		b->pcm[2 * i]     = (short)l;
		b->pcm[2 * i + 1] = (short)r;
	}
	b->cb( b->user, b->pcm, n );
}

/* -- API ----------------------------------------------------------------- */

static AVCodecContext *ouvre_codec( AVStream *s )
{
	const AVCodec *c = avcodec_find_decoder( s->codecpar->codec_id );
	if( !c )
		return NULL;
	AVCodecContext *cc = avcodec_alloc_context3( c );
	if( !cc )
		return NULL;
	avcodec_parameters_to_context( cc, s->codecpar );
	cc->thread_count = 1;			/* Bink 1 : chaque image depend de la precedente */
	if( avcodec_open2( cc, c, NULL ) < 0 )
	{
		avcodec_free_context( &cc );
		return NULL;
	}
	return cc;
}

VitaBink *vita_bink_open( const char *path, VitaBinkInfo *info,
						  VitaBinkAudioCb audio_cb, void *user, float volume )
{
	VitaBink *b = (VitaBink *)calloc( 1, sizeof( VitaBink ));
	if( !b )
		return NULL;
	b->vs = b->as = -1;
#ifdef __vita__
	b->fd = sceIoOpen( path, SCE_O_RDONLY, 0 );
	if( b->fd < 0 ) { free( b ); return NULL; }
#else
	b->fp = fopen( path, "rb" );
	if( !b->fp ) { free( b ); return NULL; }
#endif
	b->cb   = audio_cb;
	b->user = user;
	b->vol  = (int)( volume * 65536.0f );
	b->pcm  = (short *)malloc( PCM_MAX * 2 * sizeof( short ));

	unsigned char *tampon = (unsigned char *)av_malloc( TAMPON_IO );
	b->io = avio_alloc_context( tampon, TAMPON_IO, 0, b, io_lire, NULL, io_seek );
	b->fc = avformat_alloc_context();
	if( !b->pcm || !tampon || !b->io || !b->fc )
		goto echec;
	b->fc->pb = b->io;
	if( avformat_open_input( &b->fc, NULL, av_find_input_format( "bink" ), NULL ) < 0 )
		goto echec;

	for( unsigned i = 0; i < b->fc->nb_streams; ++i )
	{
		enum AVMediaType t = b->fc->streams[i]->codecpar->codec_type;
		if( t == AVMEDIA_TYPE_VIDEO && b->vs < 0 ) b->vs = (int)i;
		if( t == AVMEDIA_TYPE_AUDIO && b->as < 0 ) b->as = (int)i;
	}
	if( b->vs < 0 || !( b->vc = ouvre_codec( b->fc->streams[b->vs] )))
		goto echec;
	if( b->as >= 0 )
		b->ac = ouvre_codec( b->fc->streams[b->as] );	/* son absent : on joue muet */

	b->pkt = av_packet_alloc();
	b->vfr = av_frame_alloc();
	b->afr = av_frame_alloc();
	if( !b->pkt || !b->vfr || !b->afr )
		goto echec;

	if( info )
	{
		AVStream *s = b->fc->streams[b->vs];
		memset( info, 0, sizeof( *info ));
		info->width   = b->vc->width;
		info->height  = b->vc->height;
		info->fps_num = s->avg_frame_rate.num ? s->avg_frame_rate.num : s->r_frame_rate.num;
		info->fps_den = s->avg_frame_rate.den ? s->avg_frame_rate.den : s->r_frame_rate.den;
		info->frames  = (int)s->duration;	/* le demuxeur bink y range le nombre d'images */
		if( b->ac )
		{
			info->sample_rate = b->ac->sample_rate;
			info->channels    = b->ac->ch_layout.nb_channels;
		}
	}
	return b;

echec:
	vita_bink_close( b );
	return NULL;
}

int vita_bink_next_frame( VitaBink *b, VitaBinkFrame *out )
{
	for( ;; )
	{
		/* Une image deja prete dans le decodeur ? */
		if( avcodec_receive_frame( b->vc, b->vfr ) == 0 )
		{
			for( int p = 0; p < 3; ++p )
			{
				out->plane[p] = b->vfr->data[p];
				out->pitch[p] = b->vfr->linesize[p];
			}
			out->index = b->n++;
			return 1;
		}

		int r = av_read_frame( b->fc, b->pkt );
		if( r < 0 )
			return r == AVERROR_EOF ? 0 : r;

		if( b->pkt->stream_index == b->vs )
		{
			avcodec_send_packet( b->vc, b->pkt );
		}
		else if( b->ac && b->cb && b->pkt->stream_index == b->as )	/* sans callback : audio ignore */
		{
			if( avcodec_send_packet( b->ac, b->pkt ) == 0 )
				while( avcodec_receive_frame( b->ac, b->afr ) == 0 )
					pousse_audio( b, b->afr );
		}
		av_packet_unref( b->pkt );
	}
}

void vita_bink_close( VitaBink *b )
{
	if( !b )
		return;
	av_frame_free( &b->vfr );
	av_frame_free( &b->afr );
	av_packet_free( &b->pkt );
	avcodec_free_context( &b->vc );
	avcodec_free_context( &b->ac );
	if( b->fc )
		avformat_close_input( &b->fc );	/* pb perso : non ferme ici */
	if( b->io )
	{
		av_freep( &b->io->buffer );
		avio_context_free( &b->io );
	}
#ifdef __vita__
	if( b->fd >= 0 ) sceIoClose( b->fd );
#else
	if( b->fp ) fclose( b->fp );
#endif
	free( b->pcm );
	free( b );
}
