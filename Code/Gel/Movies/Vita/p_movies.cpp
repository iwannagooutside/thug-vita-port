/*****************************************************************************
**  THUG-Vita -- lecture des FMV Bink (issue #3)                            **
**  Code/Gel/Movies/Vita/p_movies.cpp                                       **
**                                                                          **
**  Compile seulement avec THUG_FMV (vita/CMakeLists.txt : FFmpeg Bink      **
**  trouve dans THUG_FFMPEG_BINK_DIR). Sans lui, Flx::PlayMovie reste le    **
**  stub de kisak et les films sont sautes.                                 **
**                                                                          **
**  Calque de Xbox/p_movies.cpp (contrat) :                                 **
**   - BLOQUANT : rend la main a la fin du film ou sur appui.               **
**   - nom "movies\<nom>" (ou "teasers\<nom>") -> on garde ce qui suit le   **
**     dernier '\' et on lit ux0:data/thug/Data/movies/bik/<nom>.bik.       **
**   - fichier absent : retour immediat, sans erreur (Xbox : « Movie not    **
**     there, just quit »). C'est ce qui rend le dossier movies optionnel.  **
**   - musique et streams arretes avant ; volume = max(musique, effets) a   **
**     50 % (BinkSetVolume 16384 sur 32768, p_movies.cpp:363).              **
**   - sortie : START ou CROIX, avec anti-rebond (le bouton doit avoir ete  **
**     relache depuis l'entree, sinon le CROIX qui a lance le film le coupe).**
**                                                                          **
**  Horloge : l'audio. Le thread de sortie compte les echantillons source   **
**  consommes ; l'image k est presentee quand l'horloge atteint k / 29,97 s.**
**  Image en retard de plus d'une periode : decodee mais non presentee      **
**  (Bink 1 ne permet pas de sauter le decodage).                           **
*****************************************************************************/

#ifdef THUG_FMV

#include <core/defines.h>
#include <core/macros.h>
#include <core/singleton.h>
#include <gel/soundfx/soundfx.h>
#include <gel/music/music.h>
#include <gel/movies/movies.h>
#include <gel/movies/Vita/p_movies.h>
#include <gel/movies/Vita/vita_bink.h>
#include <sys/sioman.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include "vita_log.h"
#include "vita_dbgsrv.h"

namespace NxVita { void GammaImageDebut( void ); void GammaImageFin( void ); }

namespace Flx
{

namespace
{

/* -- Audio : anneau s16 stereo a la frequence du film + thread de sortie ---
 * Port MAIN a 48000 Hz avec reechantillonnage lineaire : le port BGM est
 * unique et tenu par la musique, le port VOICE par les voix des dialogues
 * (un second est refuse, 0x80260005), et MAIN n'accepte que 48000. */

#define SORTIE_HZ		48000
#define GRAIN			512							/* maximum du port MAIN */
/* 2 s. Mesure : chaque .bik stocke l'audio ~760 ms en avance sur l'image
 * (34560 echantillons recus avant l'image 0, avance constante ensuite) ;
 * l'anneau doit absorber cette avance plus une marge. */
#define ANNEAU			( 2 * 48000 )

static short			s_anneau[ANNEAU * 2];
static volatile int		s_ecrit;					/* en echantillons, croissant */
static volatile int		s_lu;						/* idem, cote sortie */
static volatile int		s_joues;					/* echantillons source consommes */
static volatile int		s_fin_audio;
static unsigned int		s_pas;						/* 16.16, source par sortie */
static int				s_port	 = -1;
static SceUID			s_thread = -1;
/* Double tampon : sceAudioOutOutput lit le tampon PENDANT qu'il joue (#30). */
static short			*s_sortie_base = NULL;

static void audio_cb( void *, const short *pcm, int n )
{
	/* Producteur : le decodeur. S'il prend trop d'avance (anneau plein), on
	 * attend le consommateur -- c'est ce qui regule aussi la lecture. */
	for( int i = 0; i < n; )
	{
		int libre = ANNEAU - ( s_ecrit - s_lu );
		if( libre <= 0 ) { sceKernelDelayThread( 2000 ); continue; }
		int k = n - i < libre ? n - i : libre;
		for( int j = 0; j < k; ++j )
		{
			int o = (( s_ecrit + j ) % ANNEAU ) * 2;
			s_anneau[o]     = pcm[2 * ( i + j )];
			s_anneau[o + 1] = pcm[2 * ( i + j ) + 1];
		}
		s_ecrit += k;
		i += k;
	}
}

static int thread_audio( SceSize, void * )
{
	unsigned int frac = 0;
	int tampon = 0;
	while( !s_fin_audio )
	{
		short *out = s_sortie_base + tampon * GRAIN * 2;
		tampon ^= 1;

		int i = 0;
		for( ; i < GRAIN; ++i )
		{
			if( s_ecrit - s_lu < 2 )
				break;							/* sous-alimentation : silence */
			const int o0 = ( s_lu % ANNEAU ) * 2;
			const int o1 = (( s_lu + 1 ) % ANNEAU ) * 2;
			const int t  = (int)( frac >> 1 );	/* 0..32767 */
			out[2 * i]     = (short)( s_anneau[o0]     + ((( s_anneau[o1]     - s_anneau[o0]     ) * t ) >> 15 ));
			out[2 * i + 1] = (short)( s_anneau[o0 + 1] + ((( s_anneau[o1 + 1] - s_anneau[o0 + 1] ) * t ) >> 15 ));
			frac += s_pas;
			const int avance = (int)( frac >> 16 );
			frac &= 0xFFFF;
			s_lu    += avance;
			s_joues += avance;
		}
		if( i < GRAIN )
			memset( out + 2 * i, 0, ( GRAIN - i ) * 2 * sizeof( short ));
		sceAudioOutOutput( s_port, out );		/* bloque ~10,7 ms */
	}
	return sceKernelExitDeleteThread( 0 );
}

/* -- Video : texture YUV420P3, conversion couleur par le GPU ---------------
 * vitaGL accepte VGL_YUV420P_BT601 (SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0) :
 * aucun shader, aucune conversion CPU. Bink 'i' = BT.601 plage limitee.
 * Plans attendus contigus Y, U, V sans pas de ligne : copie directe si
 * c'est deja le cas, sinon tassement ligne par ligne. */

static unsigned char	*s_tasse;					/* w*h*3/2 */

static void televerse( GLuint tex, const VitaBinkFrame &f, int w, int h )
{
	const unsigned char *src = f.plane[0];
	if( !( f.pitch[0] == w && f.pitch[1] == w / 2 && f.pitch[2] == w / 2 &&
	       f.plane[1] == f.plane[0] + w * h && f.plane[2] == f.plane[1] + w * h / 4 ))
	{
		unsigned char *d = s_tasse;
		for( int p = 0; p < 3; ++p )
		{
			int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
			for( int y = 0; y < ph; ++y, d += pw )
				memcpy( d, f.plane[p] + y * f.pitch[p], pw );
		}
		src = s_tasse;
	}
	glBindTexture( GL_TEXTURE_2D, tex );
	// glCompressedTexImage2D et NON glTexImage2D : vitaGL ne connait les
	// formats planaires que la (textures.c, gpu_alloc_planar_texture). Par
	// glTexImage2D, les plans etaient lus comme du RGBA : image repetee 4 fois.
	glCompressedTexImage2D( GL_TEXTURE_2D, 0, VGL_YUV420P_BT601, w, h, 0,
	                        w * h * 3 / 2, src );
}

/* 4:3 dans 960x544 : 725x544 centre, bandes noires. */
static void dessine( GLuint tex )
{
	const float L = 960.0f, H = 544.0f, w = H * 4.0f / 3.0f, x0 = ( L - w ) * 0.5f;
	const float pos[] = { x0, 0,  x0 + w, 0,  x0, H,  x0 + w, H };
	static const float uv[] = { 0, 0,  1, 0,  0, 1,  1, 1 };

	NxVita::GammaImageDebut();
	glViewport( 0, 0, 960, 544 );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	glMatrixMode( GL_PROJECTION ); glPushMatrix(); glLoadIdentity(); glOrtho( 0, L, H, 0, -1, 1 );
	glMatrixMode( GL_MODELVIEW );  glPushMatrix(); glLoadIdentity();
	glDisable( GL_DEPTH_TEST ); glDisable( GL_CULL_FACE );
	glDisable( GL_BLEND );      glDisable( GL_ALPHA_TEST ); glDisable( GL_LIGHTING );
	glColor4f( 1, 1, 1, 1 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, tex );
	// Tampons delies : un tableau de sommets du decor encore attache serait
	// lu a la place de notre quad (meme garde que l'ecran de chargement).
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
	glDisableClientState( GL_NORMAL_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, pos );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );

	glBindTexture( GL_TEXTURE_2D, 0 );
	glEnable( GL_DEPTH_TEST );
	glEnable( GL_CULL_FACE );
	glMatrixMode( GL_MODELVIEW );  glPopMatrix();
	glMatrixMode( GL_PROJECTION ); glPopMatrix();
	glMatrixMode( GL_MODELVIEW );

	NxVita::GammaImageFin();
	vita_dbgsrv_frame();		/* captures vctl pendant le film */
	vglSwapBuffers( GL_FALSE );
}

/* Copie conforme de Xbox/p_movies.cpp. Les octets 2-3 de GetControlData
 * suivent la disposition PS2 sur Vita (Sys/SIO/Vita/p_siodev.cpp : START =
 * 0x0800, CROIX = 0x0040), donc les memes masques que l'Xbox (Start, A). */
static bool sDebounceCheck( uint16 *p_debounceFlags, uint8 *p_data, uint16 mask )
{
	if( p_data == NULL )
		return false;
	uint16 data = ( p_data[2] << 8 ) | p_data[3];
	if(( data & mask ) == 0 )
		return ( *p_debounceFlags & mask ) != 0;
	*p_debounceFlags |= mask;
	return false;
}

static unsigned long long maintenant_us( void ) { return sceKernelGetProcessTimeWide(); }

static double horloge_us( bool avec_son, int hz, unsigned long long t0 )
{
	return avec_son ? 1e6 * s_joues / hz : (double)( maintenant_us() - t0 );
}

} // namespace


void PMovies_PlayMovie( const char *pName )
{
	// Volume : comme l'Xbox, le plus fort des deux reglages (0..100), a la
	// moitie du gain nominal (BinkSetVolume 16384 sur 32768).
	Spt::SingletonPtr< Sfx::CSfxManager > sfx_manager;
	float music_vol	= Pcm::GetVolume();
	float sfx_vol	= sfx_manager->GetMainVolume();
	float vol		= ( music_vol > sfx_vol ? music_vol : sfx_vol ) * 0.01f * 0.5f;

	// "movies\intro" -> ux0:data/thug/Data/movies/bik/intro.bik
	const char *base = strrchr( pName, '\\' );
	base = base ? base + 1 : pName;
	char chemin[256];
	snprintf( chemin, sizeof( chemin ), "ux0:data/thug/Data/movies/bik/%s.bik", base );

	Pcm::StopMusic();
	Pcm::StopStreams();

	VitaBinkInfo info;
	VitaBink *b = vita_bink_open( chemin, &info, audio_cb, NULL, vol );
	if( !b )
	{
		VLOG( "FMV", "%s absent ou illisible : film saute", chemin );
		return;
	}
	VLOG( "FMV", "%s : %dx%d, %d images, %d/%d ips, audio %d Hz x%d, volume %.2f", chemin,
	      info.width, info.height, info.frames, info.fps_num, info.fps_den,
	      info.sample_rate, info.channels, vol );

	s_tasse = (unsigned char *)malloc( info.width * info.height * 3 / 2 );
	GLuint tex = 0;
	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	s_ecrit = s_lu = s_joues = 0;
	s_fin_audio = 0;
	bool avec_son = false;
	if(( info.sample_rate > 0 ) && ( info.channels == 2 ))
	{
		if( !s_sortie_base )
			s_sortie_base = (short *)memalign( 64, 2 * GRAIN * 2 * sizeof( short ));
		s_pas  = (unsigned int)((( unsigned long long )info.sample_rate << 16 ) / SORTIE_HZ );
		s_port = s_sortie_base
		         ? sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN,
		                                SORTIE_HZ, SCE_AUDIO_OUT_MODE_STEREO )
		         : -1;
		if( s_port >= 0 )
		{
			int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
			sceAudioOutSetVolume( s_port, (SceAudioOutChannelFlag)( SCE_AUDIO_VOLUME_FLAG_L_CH |
			                                                        SCE_AUDIO_VOLUME_FLAG_R_CH ), vols );
			// Priorite au-dessus du thread principal, comme thug_sfx.
			s_thread = sceKernelCreateThread( "thug_fmv_snd", thread_audio, 64, 0x4000, 0, 0, NULL );
			if(( s_thread >= 0 ) && ( sceKernelStartThread( s_thread, 0, NULL ) >= 0 ))
				avec_son = true;
			else
				VLOG( "FMV", "thread audio refuse (0x%08x) : film muet", s_thread );
		}
		else
			VLOG( "FMV", "port audio MAIN refuse (0x%08x) : film muet", s_port );
	}

	Spt::SingletonPtr< SIO::Manager > sio_manager;
	uint16 debounce_flags[4] = { 0, 0, 0, 0 };
	const double periode_us = 1e6 * info.fps_den / info.fps_num;
	const unsigned long long t0 = maintenant_us();
	unsigned long long t_dec = 0, t_dec_max = 0;
	int n_dec = 0, n_sautees = 0;

	VitaBinkFrame f;
	for( ;; )
	{
		const unsigned long long a = maintenant_us();
		if( vita_bink_next_frame( b, &f ) != 1 )
			break;
		const unsigned long long d = maintenant_us() - a;
		t_dec += d; ++n_dec;
		if( d > t_dec_max ) t_dec_max = d;

		double h = horloge_us( avec_son, info.sample_rate, t0 );
		const double echeance_us = f.index * periode_us;

		if( h > echeance_us + periode_us )
		{
			++n_sautees;						// en retard : on ne presente pas
		}
		else
		{
			while( echeance_us > h + 2000.0 )	// en avance : on attend
			{
				sceKernelDelayThread( 2000 );
				h = horloge_us( avec_son, info.sample_rate, t0 );
			}
			televerse( tex, f, info.width, info.height );
			dessine( tex );
		}

		// Sortie au START / CROIX.
		sio_manager->ProcessDevices();
		bool quit = false;
		for( int i = 0; i < 4 && !quit; ++i )
		{
			SIO::Device *p_device = sio_manager->GetDeviceByIndex( i );
			if( !p_device )
				continue;
			unsigned char *p_data = p_device->GetControlData();
			quit = sDebounceCheck( &debounce_flags[i], p_data, 0x0800 ) ||	// Start
			       sDebounceCheck( &debounce_flags[i], p_data, 0x0040 );		// Croix
		}
		if( quit )
		{
			VLOG( "FMV", "interrompu a l'image %d", f.index );
			break;
		}

		if(( n_dec % 150 ) == 0 )
			VLOG( "FMV", "image %d : decodage %.2f ms en moyenne, %.2f au pire, %d non presentees",
			      f.index, t_dec / 1000.0 / n_dec, t_dec_max / 1000.0, n_sautees );
	}

	VLOG( "FMV", "fin : %d images decodees, %.2f ms/image en moyenne (pire %.2f), %d non presentees",
	      n_dec, n_dec ? t_dec / 1000.0 / n_dec : 0.0, t_dec_max / 1000.0, n_sautees );

	s_fin_audio = 1;
	if( s_thread >= 0 ) { sceKernelWaitThreadEnd( s_thread, NULL, NULL ); s_thread = -1; }
	if( s_port >= 0 )   { sceAudioOutReleasePort( s_port ); s_port = -1; }
	vita_bink_close( b );
	glDeleteTextures( 1, &tex );
	free( s_tasse ); s_tasse = NULL;
}

} // namespace Flx

#endif // THUG_FMV
