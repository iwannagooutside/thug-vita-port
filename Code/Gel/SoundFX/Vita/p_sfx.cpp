/*****************************************************************************
**  THUG-Vita â€” effets sonores                                              **
**  Code/Gel/SoundFX/Vita/p_sfx.cpp                                         **
**                                                                          **
**  Les .pcm du jeu sont de l'Xbox ADPCM mono, 44100 Hz, blockAlign 36 â€”     **
**  le meme format que la musique, a la stereo pres. Controle arithmetique  **
**  fait sur le fichier : 44100 / 64 echantillons par bloc * 36 octets =    **
**  24806, exactement l'avgBytesPerSec annonce. Le decodeur est partage :   **
**  Core/Vita/adpcm.cpp, valide au bit pres.                                **
**                                                                          **
**  CE QUI CHANGE PAR RAPPORT A LA MUSIQUE : le jeu joue plusieurs effets   **
**  simultanement (roulettes, grind, collisions, voix), alors que           **
**  sceAudioOut ne fournit qu'un flux. Il faut donc MIXER nous-memes.       **
**                                                                          **
**  Choix retenus, et pourquoi :                                            **
**                                                                          **
**  - Sons decodes ENTIEREMENT a la lecture du fichier, gardes en PCM. Un   **
**    effet fait ~12 Ko d'ADPCM, soit ~48 Ko decode ; meme quelques         **
**    centaines tiennent largement. Decoder a la volee dans le mixeur       **
**    couterait du CPU a chaque image, sur une console ou on se bat deja    **
**    pour la fluidite.                                                     **
**                                                                          **
**  - Mixage en entier 32 bits puis ecretage. Additionner 24 voix           **
**    directement en 16 bits deborde des que trois sons forts se            **
**    superposent, et le debordement s'entend comme un craquement.          **
**                                                                          **
**  - Le pas de lecture est en virgule fixe 16.16 : il porte a la fois le   **
**    reechantillonnage 44100 -> 48000 et le pitch demande par le jeu. La   **
**    POSITION, elle, est un index entier + une fraction 16 bits separes    **
**    (#26, #30) : en 16.16 dans 32 bits elle debordait a 65536             **
**    echantillons (1,49 s), soit TOUS les sons de roulement et de grind.   **
**                                                                          **
**  - Interpolation cubique (Hermite/Catmull-Rom), gains lisses par grain,  **
**    fondu court a l'arret et limiteur sur la somme : voir thread_mixeur.  **
*****************************************************************************/

#include <core/defines.h>
#include <core/Vita/adpcm.h>

#include <gel/soundfx/soundfx.h>
#include <gel/soundfx/Vita/p_sfx.h>

#include <sys/file/filesys.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <math.h>

#include "vita_log.h"

namespace Sfx
{

float	gSfxVolume = 100.0f;

namespace
{

#define SORTIE_HZ		48000	/* le port BGM tourne deja a 48000 */
#define SORTIE_CANAUX	2
// 512 et non 1024 : le port MAIN n'accepte pas plus (64..512, multiple de 64).
// Le port VOICE (1024) est deja pris par les voix des dialogues (0x80260005).
#define GRAIN			512
static const int	s_grain		= GRAIN;

// Etat d'une voix du mixeur.
struct SVoix
{
	const SSonVita	*p_son;
	// [REFUTE] (#26, #30) La position etait un 16.16 dans un unsigned 32 bits :
	// il deborde a 65536 echantillons, soit 1,49 s a 44100 Hz. Or TOUS les
	// sons de roulement et de grind sont plus longs (RollConcSmooth02 : 162048
	// echantillons, RollAsphalt ~116000, GrindMetal02 173824). La lecture
	// revenait donc au debut toutes les 1,49 s, au milieu du son -- saut de
	// 9580 a -8 sur RollConcSmooth02 : le craquement periodique du roulement.
	// Et un son PONCTUEL de plus de 65536 echantillons (FallWater,
	// FlamingFireBall01) ne finissait jamais : la position ne pouvait plus
	// atteindre sa fin. Index entier et fraction sont desormais separes.
	unsigned int	 idx;			// echantillon courant
	unsigned int	 frac;			// fraction 0..65535
	unsigned int	 pas;			// increment 16.16 par echantillon de sortie
	int				 vol_g;			// demande du moteur, 256 = 100 % (plafond 400 %)
	int				 vol_d;
	// Gains effectivement appliques a la fin du dernier grain. Le mixeur va
	// de ces gains vers la cible (vol x volume global) en rampe sur un grain :
	// le roulement change de volume a chaque image, et un saut de gain net
	// s'entend comme un clic ("zipper").
	float			 gain_g;
	float			 gain_d;
	bool			 active;
	bool			 boucle;
	// #57 : age de la voix ; une voix en boucle active plus de 20 s est
	// nommee une fois dans le journal (son de casse joue sans fin, revu le
	// 2026-10-05 sans aucun appel de son en cours : une seule voix en boucle).
	unsigned long long	 t_debut;
	bool			 signalee;
};

static SVoix	s_voix[NUM_VOICES];

// Voix arretees en cours de fondu : copie de la voix, jouee UN grain de plus
// avec un gain qui descend a zero. Couper net un son fort (roulement, grind)
// au milieu d'une onde fait un clic. La voix d'origine est libre aussitot :
// le moteur peut la reattribuer sans attendre la fin du fondu.
static SVoix	s_fondus[NUM_VOICES];

// Diagnostic #26/#30, journalise par PerFrameUpdate quand ca change.
static volatile int	s_nb_retards	= 0;	// port vide a l'arrivee du grain
static volatile int	s_nb_limites	= 0;	// grains ou le limiteur a agi
static volatile unsigned int s_us_calcul_max = 0;	// melange d'un grain, pire cas
static volatile unsigned int s_us_ecart_max  = 0;	// entre deux sorties, pire cas

// Commande de dev "mix N" (#30) : enregistre N secondes de la sortie reelle du
// mixeur, ecrites ensuite dans ux0:data/thug/mix.wav par le thread principal.
static short	*s_enreg		= NULL;
static volatile int	s_enreg_pos	= 0;	// en echantillons stereo
static int		s_enreg_max		= 0;
static volatile bool s_enreg_plein = false;
static float		s_limite		= 1.0f;	// gain courant du limiteur

static int		s_port			= -1;
static SceUID	s_thread		= -1;
static SceUID	s_mutex			= -1;
static bool		s_tourne		= false;
// DOUBLE TAMPON (#30) : sceAudioOutOutput ne copie pas le tampon, la Vita le
// lit PENDANT qu'il joue. Recalculer le grain suivant dans le meme tampon
// ecrasait le son en cours de lecture -- le craquement entendu sur console
// alors que l'enregistrement du melange (pris avant l'envoi) etait propre.
// On alterne entre deux moities, comme le pilote audio Vita de SDL.
static short	*s_melange		= NULL;	// moitie en cours de calcul
static short	*s_melange_base	= NULL;	// deux grains
static int		s_volume_global	= 256;	// 0..256

static inline void verrouille( void )
{
	if( s_mutex >= 0 )
		sceKernelLockMutex( s_mutex, 1, NULL );
}

static inline void deverrouille( void )
{
	if( s_mutex >= 0 )
		sceKernelUnlockMutex( s_mutex, 1 );
}


// Le moteur donne des volumes par canal, en pourcentage, avec un signe qui
// encode la position derriere l'auditeur sur PS2. On ne garde que l'amplitude.
static void volumes_depuis( sVolume *p_vol, int *p_g, int *p_d )
{
	float g = 100.0f, d = 100.0f;
	if( p_vol )
	{
		g = p_vol->GetChannelVolume( 0 );
		d = p_vol->GetChannelVolume( 1 );
	}
	if( g < 0.0f )	g = -g;
	if( d < 0.0f )	d = -d;

	// Plafond large ici : comme XBox/p_sfx.cpp:1216, le volume du canal est
	// d'abord multiplie par le volume global des effets, et c'est le RESULTAT
	// qui est plafonne a 100 % (gain_effectif).
	if( g > 400.0f )	g = 400.0f;
	if( d > 400.0f )	d = 400.0f;
	*p_g = (int)( g * 2.56f );
	*p_d = (int)( d * 2.56f );
	if( *p_g < 0 )		*p_g = 0;
	if( *p_d < 0 )		*p_d = 0;
}


// Gain lineaire applique : volume du canal x volume global, plafonne a 1.
// La XBox convertit le meme pourcentage en dB (20 log10(v/100), p_sfx.cpp:1230),
// ce qui revient exactement a ce gain lineaire.
static inline float gain_effectif( int vol, int vg )
{
	int g = ( vol * vg ) >> 8;
	if( g > 256 )
		g = 256;
	return (float)g * ( 1.0f / 256.0f );
}


// Echantillon i d'un son, hors bornes compris. Un son en boucle se replie sur
// son debut (la XBox boucle le tampon ENTIER, DSBPLAY_LOOPING sans region,
// Xbox/p_sfx.cpp:993) ; un son ponctuel commence par son premier echantillon
// et se tait apres sa fin.
static inline float echantillon( const SSonVita *p_s, int i, bool boucle )
{
	const int n = p_s->nb_echantillons;
	if( i < 0 )
		return boucle ? (float)p_s->p_echantillons[(( i % n ) + n ) % n]
		              : (float)p_s->p_echantillons[0];
	if( i >= n )
		return boucle ? (float)p_s->p_echantillons[i % n] : 0.0f;
	return (float)p_s->p_echantillons[i];
}


// Mixe un grain d'une voix dans accu, avec un gain qui va lineairement de
// gain_g/gain_d a cible_g/cible_d. Rend false si un son ponctuel est fini.
//
// [REFUTE] (#26) "lire l'echantillon le plus proche suffit". Mesure sur
// table (Python) contre un reechantillonneur de reference (scipy
// resample_poly 160/147) : le plus proche donne un rapport signal/erreur de
// 0,8 dB sur menu03, 5,7 dB sur GUI_click06, 20 dB sur DE_MenuSelect, 17 dB
// sur les boucles de roulement -- l'erreur est presque aussi forte que le son,
// c'est le gresillement des menus. Cubique : 18, 28, 42 et 31 dB.
static bool mixe_voix( SVoix *p_v, float *accu, float cible_g, float cible_d )
{
	const SSonVita	*p_s	= p_v->p_son;
	const short		*e		= p_s->p_echantillons;
	const unsigned int n	= (unsigned int)p_s->nb_echantillons;
	const bool		boucle	= p_v->boucle;
	const unsigned int pas	= p_v->pas;
	unsigned int	idx		= p_v->idx;
	unsigned int	frac	= p_v->frac;

	float g = p_v->gain_g;
	float d = p_v->gain_d;
	const float dg = ( cible_g - g ) * ( 1.0f / s_grain );
	const float dd = ( cible_d - d ) * ( 1.0f / s_grain );

	for( int i = 0; i < s_grain; ++i )
	{
		if( idx >= n )
		{
			if( !boucle )
			{
				p_v->idx = idx;
				return false;
			}
			// Rebouclage SANS perdre la fraction ni sauter d'echantillon de
			// sortie. L'ancien code ecrivait un zero a cet instant (le
			// "continue" sautait l'addition) : un clic a chaque tour.
			idx %= n;
		}

		float xm, x0, x1, x2;
		if(( idx >= 1 ) && ( idx + 2 < n ))
		{
			xm = e[idx - 1];
			x0 = e[idx];
			x1 = e[idx + 1];
			x2 = e[idx + 2];
		}
		else
		{
			xm = echantillon( p_s, (int)idx - 1, boucle );
			x0 = echantillon( p_s, (int)idx, boucle );
			x1 = echantillon( p_s, (int)idx + 1, boucle );
			x2 = echantillon( p_s, (int)idx + 2, boucle );
		}

		// Hermite (Catmull-Rom) a 4 points.
		const float t  = (float)frac * ( 1.0f / 65536.0f );
		const float c1 = 0.5f * ( x1 - xm );
		const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
		const float c3 = 0.5f * ( x2 - xm ) + 1.5f * ( x0 - x1 );
		const float s  = (( c3 * t + c2 ) * t + c1 ) * t + x0;

		g += dg;
		d += dd;
		accu[i * 2    ] += s * g;
		accu[i * 2 + 1] += s * d;

		frac += pas;
		idx  += frac >> 16;
		frac &= 0xFFFF;
	}

	p_v->idx	= idx;
	p_v->frac	= frac;
	p_v->gain_g	= cible_g;
	p_v->gain_d	= cible_d;
	return true;
}


// Arrete une voix en passant par un fondu. Appelee SOUS le verrou.
static void arrete_voix( SVoix *p_v )
{
	if( !p_v->active )
		return;
	p_v->active = false;
	if( !p_v->p_son || (( p_v->gain_g <= 0.0f ) && ( p_v->gain_d <= 0.0f )))
		return;
	for( int f = 0; f < NUM_VOICES; ++f )
	{
		if( !s_fondus[f].active )
		{
			s_fondus[f]			= *p_v;
			s_fondus[f].active	= true;
			return;
		}
	}
	// Plus de place : arret net, comme avant.
}


static int thread_mixeur( SceSize, void * )
{
	bool sortait = false;	// le grain precedent a ete envoye au port
	SceUInt64 t_sortie = 0;	// fin du dernier sceAudioOutOutput

	while( s_tourne )
	{
		// Un battement periodique du thread vivait ici. Il a servi une fois,
		// decisivement : il a montre que le thread CESSAIT de tourner ~8 s
		// avant le premier son joue, ce qui a designe CleanUpSoundFX et non
		// la boucle de melange. A remettre en premier si un silence revient.
		// Le melange se fait en flottant : la somme de plusieurs voix fortes
		// depasse allegrement le 16 bits, et la limitation doit se faire UNE
		// fois, a la fin, pas a chaque addition.
		static float accu[GRAIN * SORTIE_CANAUX];
		memset( accu, 0, sizeof( accu ));

		bool quelque_chose = false;

		verrouille();
		const int vg = s_volume_global;
		for( int v = 0; v < NUM_VOICES; ++v )
		{
			SVoix *p_v = &s_voix[v];
			if( !p_v->active || !p_v->p_son )
				continue;
			quelque_chose = true;
			if( !mixe_voix( p_v, accu, gain_effectif( p_v->vol_g, vg ),
			                gain_effectif( p_v->vol_d, vg )))
				p_v->active = false;
		}
		for( int f = 0; f < NUM_VOICES; ++f )
		{
			SVoix *p_f = &s_fondus[f];
			if( !p_f->active )
				continue;
			quelque_chose = true;
			mixe_voix( p_f, accu, 0.0f, 0.0f );
			p_f->active = false;	// un seul grain de fondu (~10 ms)
		}
		deverrouille();

		if( !quelque_chose )
		{
			// Aucune voix : on ne pousse pas de silence en boucle serree, on
			// rend la main. Sans cela ce thread tournerait en continu pour
			// rien, sur une console ou chaque coeur compte.
			sortait = false;
			sceKernelDelayThread( 5000 );
			continue;
		}

		// LIMITEUR sur la somme (#30). Chaque voix est au meme gain que sur
		// XBox, mais les effets sont masterises tres fort (DE_MenuSelect :
		// RMS -8 dBFS ; les BailBodyPunch ont deja des milliers d'echantillons
		// a fond dans le fichier) : deux ou trois sons simultanes depassent
		// le 16 bits, et l'ecretage net qui suivait est la saturation
		// entendue. Gain reduit d'un coup (32 echantillons) si la crete du
		// grain depasse le plafond, remonte en ~150 ms ensuite. L'ecretage
		// final ne sert plus que de filet.
		float crete = 0.0f;
		for( int i = 0; i < s_grain * SORTIE_CANAUX; ++i )
		{
			const float a = fabsf( accu[i] );
			if( a > crete )
				crete = a;
		}
		const float plafond = 32000.0f;
		const float cible   = ( crete > plafond ) ? ( plafond / crete ) : 1.0f;
		float fin;
		int   rampe;
		if( cible < s_limite )
		{
			fin   = cible;
			rampe = 32;
			++s_nb_limites;
		}
		else
		{
			fin = s_limite + ( 1.0f - s_limite ) * 0.07f;
			if( fin > cible )
				fin = cible;
			rampe = s_grain;
		}
		const float depart = s_limite;
		for( int i = 0; i < s_grain; ++i )
		{
			const float lim = ( i < rampe )
			                  ? depart + ( fin - depart ) * (float)( i + 1 ) / (float)rampe
			                  : fin;
			for( int c = 0; c < SORTIE_CANAUX; ++c )
			{
				int e = (int)( accu[i * SORTIE_CANAUX + c] * lim );
				if( e >  32767 )	e =  32767;
				if( e < -32768 )	e = -32768;
				s_melange[i * SORTIE_CANAUX + c] = (short)e;
			}
		}
		s_limite = fin;

		// Port deja vide a l'arrivee de ce grain alors qu'on jouait : le
		// thread a ete en retard, il y a eu un trou -- un craquement qui
		// n'est pas dans le melange. Compte pour le journal.
		if( sortait && ( sceAudioOutGetRestSample( s_port ) == 0 ))
			++s_nb_retards;

		// Diagnostic #30 : temps de calcul du grain (trop lent ?) et ecart
		// entre deux sorties (thread preempte ?).
		const SceUInt64 t_avant = sceKernelGetProcessTimeWide();
		if( sortait )
		{
			const unsigned int calcul = (unsigned int)( t_avant - t_sortie );
			if( calcul > s_us_calcul_max )
				s_us_calcul_max = calcul;
		}
		if( s_enreg && !s_enreg_plein )
		{
			int n = s_grain;
			if( s_enreg_pos + n > s_enreg_max )
				n = s_enreg_max - s_enreg_pos;
			memcpy( s_enreg + s_enreg_pos * SORTIE_CANAUX, s_melange,
			        n * SORTIE_CANAUX * sizeof( short ));
			s_enreg_pos += n;
			if( s_enreg_pos >= s_enreg_max )
				s_enreg_plein = true;
		}
		sceAudioOutOutput( s_port, s_melange );
		s_melange = ( s_melange == s_melange_base )
		            ? s_melange_base + GRAIN * SORTIE_CANAUX : s_melange_base;
		const SceUInt64 t_apres = sceKernelGetProcessTimeWide();
		if( sortait )
		{
			const unsigned int ecart = (unsigned int)( t_apres - t_sortie );
			if( ecart > s_us_ecart_max )
				s_us_ecart_max = ecart;
		}
		t_sortie = t_apres;
		sortait = true;
	}
	return 0;
}

} // namespace anonyme


void InitSoundFX( CSfxManager * )
{
	memset( s_voix, 0, sizeof( s_voix ));
	memset( s_fondus, 0, sizeof( s_fondus ));

	// memalign et non malloc : la sortie audio de la Vita exige un tampon
	// aligne. Un tampon mal aligne ne provoque pas d'erreur franche, juste
	// du silence -- exactement le symptome le plus couteux a diagnostiquer.
	s_melange_base = (short *)memalign( 64, 2 * GRAIN * SORTIE_CANAUX * sizeof( short ));
	s_melange = s_melange_base;
	if( !s_melange )
	{
		VLOG( "SFX", "pas d'effets : allocation du tampon impossible" );
		return;
	}

	// Port MAIN, pas BGM : la Vita n'accorde qu'UN SEUL port BGM et la musique
	// l'a deja pris. Le second appel echouait avec 0x80260005.
	s_port = sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN,
	                              SORTIE_HZ, SCE_AUDIO_OUT_MODE_STEREO );
	if( s_port < 0 )
	{
		VLOG( "SFX", "pas d'effets : port audio refuse (0x%08x)", s_port );
		return;
	}

	int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume( s_port,
	                      (SceAudioOutChannelFlag)( SCE_AUDIO_VOLUME_FLAG_L_CH |
	                                                SCE_AUDIO_VOLUME_FLAG_R_CH ),
	                      vols );

	s_mutex = sceKernelCreateMutex( "thug_sfx", 0, 0, NULL );

	s_tourne = true;
	// Priorite 64 (la plus haute des threads utilisateur, celle du thread
	// audio de SDL sur Vita) et non la priorite par defaut, partagee avec le
	// thread principal : avec un grain de 512 echantillons on n'a que 10,7 ms
	// de marge, et un thread audio qui attend son tour derriere le jeu laisse
	// le port vide -- un craquement. Il passe l'essentiel de son temps bloque
	// dans sceAudioOutOutput, et ne prend donc presque rien au jeu.
	s_thread = sceKernelCreateThread( "thug_sfx", thread_mixeur,
	                                  64, 0x10000, 0, 0, NULL );
	if( s_thread >= 0 )
	{
		// Le retour est verifie : un demarrage refuse passerait sinon pour un
		// succes, avec pour seul symptome un silence total.
		const int r = sceKernelStartThread( s_thread, 0, NULL );
		if( r < 0 )
			VLOG( "SFX", "pas d'effets : demarrage refuse (0x%08x)", r );
	}
	else
	{
		VLOG( "SFX", "pas d'effets : thread refuse (0x%08x)", s_thread );
	}

	VLOG( "SFX", "effets prets : %d voix, melange logiciel a %d Hz",
	      (int)NUM_VOICES, (int)SORTIE_HZ );
}


void CleanUpSoundFX( void )
{
	// [REFUTE] ï¿½ nettoyer, c'est arreter le thread ï¿½. Le commentaire d'origine
	// (soundfx.cpp:375) est explicite : ï¿½ so soundfx can be used in the NEXT
	// PHASE (next level, frontend, whatever) ï¿½. Cette fonction est appelee a
	// CHAQUE changement de phase, pas a l'extinction.
	//
	// Poser s_tourne = false ici tuait donc le thread de melange au premier
	// chargement de niveau, definitivement. Symptome : les sons se chargeaient
	// bien, PlaySoundPlease attribuait des voix, et il ne sortait rien -- avec
	// pour seule trace un battement du thread qui cessait ~8 s avant le
	// premier son joue.
	//
	// Nettoyer, ici, c'est faire taire les voix ET rendre les sons du niveau.
	StopAllSoundFX();

	// [SOURCE] XBox/p_sfx.cpp:843 : « on Xbox it needs to explicitly delete any
	// sounds that were not marked as permanent at load time ». Oubli ici : ~10 Mo
	// de PCM decode perdus a CHAQUE changement de niveau, tas newlib epuise apres
	// ~9 niveaux -- plantage du Story Mode au chargement de Vancouver (#47, #51).
	// Les voix sont arretees (StopAllSoundFX) ; le verrou du mixeur garantit
	// qu'aucune ne lit plus ces echantillons.
	verrouille();
	// Les fondus en cours lisent encore ces echantillons : on les coupe.
	for( int f = 0; f < NUM_VOICES; ++f )
		s_fondus[f].active = false;
	for( int i = 0; i < NumWavesInTable; ++i )
	{
		PlatformWaveInfo *p_info = &( WaveTable[PERM_WAVE_TABLE_MAX_ENTRIES + i].platformWaveInfo );
		SSonVita *p_son = (SSonVita *)p_info->p_sound_data;
		if( p_son )
		{
			free( p_son->p_echantillons );
			free( p_son );
		}
		p_info->p_sound_data = NULL;
	}
	NumWavesInTable = 0;
	deverrouille();
}


// Le thread ne s'arrete qu'a l'extinction du programme. Garde pour memoire :
// personne ne l'appelle aujourd'hui, et c'est voulu.
void ArreteThreadSfx( void )
{
	s_tourne = false;
}


bool LoadSoundPlease( const char *sfxName, uint32, PlatformWaveInfo *pInfo,
                      bool loadPerm )
{
	if( !pInfo || !sfxName )
		return false;

	pInfo->p_sound_data = NULL;

	// Chemin construit comme sur Xbox (SoundFX/Xbox/p_sfx.cpp) :
	// Â« sounds\pcm\ Â» + nom + Â« .pcm Â». Ces fichiers vivent dans les archives
	// PRE (skater_sounds.prx, parked_sounds.prx...), que la couche fichier
	// sait deja ouvrir de maniere transparente.
	char chemin[256];
	snprintf( chemin, sizeof( chemin ), "sounds\\pcm\\%s.pcm", sfxName );

	void *p_fic = File::Open( chemin, "rb" );
	if( !p_fic )
		return false;

	const int taille = File::GetFileSize( p_fic );
	if( taille < 64 )
	{
		File::Close( p_fic );
		return false;
	}

	unsigned char *p_brut = (unsigned char *)malloc( taille );
	if( !p_brut )
	{
		File::Close( p_fic );
		return false;
	}
	File::Read( p_brut, 1, taille, p_fic );
	File::Close( p_fic );

	// PARCOURS DES BLOCS RIFF, et non des offsets fixes.
	//
	// [REFUTE] ï¿½ fmt est a 12 et data a 40, comme dans les .wad ï¿½. C'est vrai
	// pour certains effets, faux pour d'autres : BailBodyPunch01_11 porte un
	// bloc ï¿½ bext ï¿½ (Broadcast Wave Extension) juste apres WAVE, et tout se
	// decale. Constate sur console -- les octets 12..15 valaient 62 65 78 74.
	if(( memcmp( p_brut, "RIFF", 4 ) != 0 ) ||
	   ( memcmp( p_brut + 8, "WAVE", 4 ) != 0 ))
	{
		VLOG( "SFX", "%s : ce n'est pas un RIFF/WAVE", sfxName );
		free( p_brut );
		return false;
	}

	int off_fmt = -1, off_data = -1, taille_data = 0;
	bool boucle = false;
	int pos = 12;
	while( pos + 8 <= taille )
	{
		unsigned int taille_bloc;
		memcpy( &taille_bloc, p_brut + pos + 4, 4 );

		if( memcmp( p_brut + pos, "fmt ", 4 ) == 0 )
		{
			off_fmt = pos + 8;
		}
		else if( memcmp( p_brut + pos, "data", 4 ) == 0 )
		{
			off_data    = pos + 8;
			taille_data = (int)taille_bloc;
		}
		// Son en boucle : bloc « smpl » avec au moins une boucle, comme
		// CWaveFile::ContainsLoop (Xbox/p_sfx.cpp:555). Il est APRES « data »
		// dans les 94 sons concernes (roulement, grind, moteurs...), d'ou le
		// parcours complet. La Xbox boucle alors tout le tampon
		// (DSBPLAY_LOOPING), sans tenir compte des points de boucle.
		else if(( memcmp( p_brut + pos, "smpl", 4 ) == 0 ) && ( taille_bloc >= 36 )
		        && ( pos + 8 + 32 <= taille ))
		{
			unsigned int nb_boucles;
			memcpy( &nb_boucles, p_brut + pos + 8 + 28, 4 );
			boucle = ( nb_boucles > 0 );
		}

		// Les blocs RIFF sont alignes sur 2 octets.
		pos += 8 + (int)taille_bloc;
		if( taille_bloc & 1 )
			++pos;
	}

	if(( off_fmt < 0 ) || ( off_data < 0 ))
	{
		VLOG( "SFX", "%s : bloc fmt ou data introuvable", sfxName );
		free( p_brut );
		return false;
	}

	const uint16 tag    = (uint16)( p_brut[off_fmt] | ( p_brut[off_fmt + 1] << 8 ));
	const int    canaux = (int)( p_brut[off_fmt + 2] | ( p_brut[off_fmt + 3] << 8 ));
	int          hz;
	memcpy( &hz, p_brut + off_fmt + 4, 4 );
	const int    block  = (int)( p_brut[off_fmt + 12] | ( p_brut[off_fmt + 13] << 8 ));

	if(( tag != 0x0069 ) || ( canaux < 1 ) || ( block < 8 ))
	{
		VLOG( "SFX", "%s : format 0x%04x non gere", sfxName, tag );
		free( p_brut );
		return false;
	}

	if( taille_data > ( taille - off_data ))
		taille_data = taille - off_data;

	const int blocs = taille_data / block;
	const int epb   = VitaAdpcm::EchantillonsParBloc( block, canaux );
	const int total = blocs * epb;
	if( total <= 0 )
	{
		free( p_brut );
		return false;
	}

	// On ne garde qu'un canal : le mixeur spatialise lui-meme en repartissant
	// sur gauche et droite. Les effets du jeu sont mono de toute facon.
	short *p_pcm = (short *)malloc( total * sizeof( short ));
	if( !p_pcm )
	{
		free( p_brut );
		return false;
	}

	// Garde explicite : DecodeBloc ecrit epb * canaux valeurs. Un blockAlign
	// inattendu deborderait ce tampon et corromprait la pile.
	if(( epb * canaux ) > 256 )
	{
		VLOG( "SFX", "%s : bloc de %d echantillons, trop grand", sfxName,
		      epb * canaux );
		free( p_brut );
		return false;
	}
	short tampon[256];
	int   ecrit = 0;
	for( int b = 0; b < blocs; ++b )
	{
		VitaAdpcm::DecodeBloc( p_brut + off_data + b * block, tampon, canaux, block );
		for( int i = 0; i < epb; ++i )
			p_pcm[ecrit++] = tampon[i * canaux];
	}
	free( p_brut );

	SSonVita *p_son = (SSonVita *)malloc( sizeof( SSonVita ));
	if( !p_son )
	{
		free( p_pcm );
		return false;
	}
	p_son->p_echantillons  = p_pcm;
	p_son->nb_echantillons = ecrit;
	p_son->hz              = hz;
	p_son->boucle          = boucle;
	{
		const char *b = sfxName;
		for( const char *q = sfxName; *q; ++q )
			if(( *q == '\\' ) || ( *q == '/' ))
				b = q + 1;
		strncpy( p_son->nom, b, sizeof( p_son->nom ) - 1 );
		p_son->nom[sizeof( p_son->nom ) - 1] = 0;
	}

	pInfo->p_sound_data = p_son;
	pInfo->looping      = boucle;
	pInfo->permanent    = loadPerm;
	return true;
}


int PlaySoundPlease( PlatformWaveInfo *pInfo, sVolume *p_vol, float pitch )
{
	if( !pInfo || !pInfo->p_sound_data || ( s_port < 0 ))
		return -1;

	verrouille();

	int v = -1;
	for( int i = 0; i < NUM_VOICES; ++i )
	{
		if( !s_voix[i].active )
		{
			v = i;
			break;
		}
	}
	if( v < 0 )
	{
		deverrouille();
		return -1;
	}

	SVoix *p_v = &s_voix[v];
	p_v->p_son    = pInfo->p_sound_data;
	p_v->idx      = 0;
	p_v->frac     = 0;
	p_v->boucle   = pInfo->looping;
	p_v->t_debut  = sceKernelGetProcessTimeWide();
	p_v->signalee = false;

	// Un seul pas porte le reechantillonnage ET le pitch : le son est a
	// 44100 Hz, la sortie a 48000, et le jeu exprime le pitch en pourcentage.
	float rapport = (float)pInfo->p_sound_data->hz / (float)SORTIE_HZ;
	if( pitch > 1.0f )
		rapport *= ( pitch / 100.0f );
	p_v->pas = (unsigned int)( rapport * 65536.0f );
	if( p_v->pas == 0 )
		p_v->pas = 1;

	volumes_depuis( p_vol, &p_v->vol_g, &p_v->vol_d );
	// Pas de rampe au demarrage : l'attaque d'un son doit rester franche.
	p_v->gain_g = gain_effectif( p_v->vol_g, s_volume_global );
	p_v->gain_d = gain_effectif( p_v->vol_d, s_volume_global );
	p_v->active = true;

	deverrouille();
	return v;
}


void StopSoundPlease( int whichVoice )
{
	if(( whichVoice < 0 ) || ( whichVoice >= NUM_VOICES ))
		return;
	verrouille();
	arrete_voix( &s_voix[whichVoice] );
	deverrouille();
}


bool VoiceIsOn( int whichVoice )
{
	if(( whichVoice < 0 ) || ( whichVoice >= NUM_VOICES ))
		return false;
	return s_voix[whichVoice].active;
}


void SetVoiceParameters( int whichVoice, sVolume *p_vol, float pitch )
{
	if(( whichVoice < 0 ) || ( whichVoice >= NUM_VOICES ))
		return;

	verrouille();
	SVoix *p_v = &s_voix[whichVoice];
	volumes_depuis( p_vol, &p_v->vol_g, &p_v->vol_d );

	// Â« pitch a zero Â» veut dire Â« ne change pas le pitch Â» (cf. p_sfx.h).
	if(( pitch > 1.0f ) && p_v->p_son )
	{
		float rapport = (float)p_v->p_son->hz / (float)SORTIE_HZ;
		rapport *= ( pitch / 100.0f );
		p_v->pas = (unsigned int)( rapport * 65536.0f );
		if( p_v->pas == 0 )
			p_v->pas = 1;
	}
	deverrouille();
}


void StopAllSoundFX( void )
{
	verrouille();
	for( int i = 0; i < NUM_VOICES; ++i )
		arrete_voix( &s_voix[i] );
	deverrouille();
}


void PauseSoundsPlease( void )
{
	StopAllSoundFX();
}


void SetVolumePlease( float volumeLevel )
{
	int v = (int)( volumeLevel * 2.56f );
	if( v < 0 )		v = 0;
	if( v > 256 )	v = 256;
	if(( v == 0 ) && ( s_volume_global != 0 ))
		VLOG( "SFX", "volume global mis a ZERO par le moteur" );
	s_volume_global = v;
}


// Sans effet : la Vita n'a pas de reverbe materielle et une reverbe logicielle
// couterait plus cher qu'elle ne rapporte ici. Ne rien faire est correct â€” le
// son sort simplement sec.
void SetReverbPlease( float, int, bool )	{}

// Bilan des voix toutes les 10 s, seulement s'il change : sert a reperer un
// son en boucle jamais arrete (#57).
void VitaEnregistreMix( int secondes )
{
	if( s_enreg || ( secondes <= 0 ))
		return;
	s_enreg_max = secondes * SORTIE_HZ;
	s_enreg = (short *)malloc( s_enreg_max * SORTIE_CANAUX * sizeof( short ));
	if( !s_enreg )
	{
		VLOG( "SND", "mix : allocation de %d s impossible", secondes );
		return;
	}
	s_enreg_pos   = 0;
	s_enreg_plein = false;
	VLOG( "SND", "mix : enregistrement de %d s de la sortie des effets", secondes );
}

static void ecrit_enregistrement( void )
{
	const int n = s_enreg_pos;
	const unsigned int octets = n * SORTIE_CANAUX * sizeof( short );
	unsigned char h[44];
	unsigned int v;
	memcpy( h, "RIFF", 4 );	v = 36 + octets;		memcpy( h + 4, &v, 4 );
	memcpy( h + 8, "WAVEfmt ", 8 );	v = 16;			memcpy( h + 16, &v, 4 );
	unsigned short w = 1;	memcpy( h + 20, &w, 2 );
	w = SORTIE_CANAUX;		memcpy( h + 22, &w, 2 );
	v = SORTIE_HZ;			memcpy( h + 24, &v, 4 );
	v = SORTIE_HZ * SORTIE_CANAUX * 2;	memcpy( h + 28, &v, 4 );
	w = SORTIE_CANAUX * 2;	memcpy( h + 32, &w, 2 );
	w = 16;					memcpy( h + 34, &w, 2 );
	memcpy( h + 36, "data", 4 );	memcpy( h + 40, &octets, 4 );
	SceUID f = sceIoOpen( "ux0:data/thug/mix.wav",
	                      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if( f >= 0 )
	{
		sceIoWrite( f, h, 44 );
		sceIoWrite( f, s_enreg, octets );
		sceIoClose( f );
	}
	VLOG( "SND", "mix : %d echantillons ecrits dans ux0:data/thug/mix.wav (%s)",
	      n, ( f >= 0 ) ? "ok" : "echec" );
	short *p = s_enreg;
	s_enreg = NULL;
	free( p );
}

void PerFrameUpdate( void )
{
	if( s_enreg && s_enreg_plein )
		ecrit_enregistrement();
	static unsigned long long s_t = 0;
	static int s_dernier = -1;
	const unsigned long long t = sceKernelGetProcessTimeWide();
	if(( t - s_t ) < 10000000ULL )
		return;
	s_t = t;
	int actives = 0, boucles = 0;
	for( int i = 0; i < NUM_VOICES; ++i )
		if( s_voix[i].active )
		{
			++actives;
			// #19, #22 : un son PONCTUEL encore actif bien apres sa fin. C'etait
			// le plouf (FallWater, 85376 echantillons) et le verre (HitGlassPane2x,
			// 81088) de la v1.0.2 : la position 16.16 debordait a 65536
			// echantillons et le son repartait du debut toutes les 1,49 s, voix
			// jamais liberee jusqu'a StopAllSoundFX (pause, objectif relance,
			// changement de niveau). Corrige en 1ff7bff ; le bilan ci-dessous ne
			// regardait que les voix en boucle et ne pouvait pas le voir.
			// Duree attendue au pas courant, marge 2 s (le pitch peut baisser).
			if( !s_voix[i].boucle && !s_voix[i].signalee && s_voix[i].p_son
			    && s_voix[i].pas )
			{
				const float duree = (float)s_voix[i].p_son->nb_echantillons * 65536.0f
				                    / ( (float)s_voix[i].pas * (float)SORTIE_HZ );
				const float age = (float)( t - s_voix[i].t_debut ) / 1000000.0f;
				if( age > duree + 2.0f )
				{
					s_voix[i].signalee = true;
					VLOG( "SND", "!! voix %d ponctuelle '%s' encore active apres %.1f s "
					      "(duree attendue %.2f s, position %u/%d)", i,
					      s_voix[i].p_son->nom, age, duree, s_voix[i].idx,
					      s_voix[i].p_son->nb_echantillons );
				}
			}
			if( s_voix[i].boucle )
			{
				++boucles;
				if( !s_voix[i].signalee && ( t - s_voix[i].t_debut > 20000000ULL ))
				{
					s_voix[i].signalee = true;
					VLOG( "SND", "voix %d en boucle depuis %d s : '%s' (volumes %d/%d)",
					      i, (int)(( t - s_voix[i].t_debut ) / 1000000ULL ),
					      s_voix[i].p_son ? s_voix[i].p_son->nom : "?",
					      s_voix[i].vol_g, s_voix[i].vol_d );
				}
			}
		}
	if(( actives * 1000 + boucles ) != s_dernier )
	{
		s_dernier = actives * 1000 + boucles;
		VLOG( "SND", "voix actives %d dont %d en boucle", actives, boucles );
	}

	// #26/#30 : retards du thread (trous dans la sortie) et interventions du
	// limiteur, cumules depuis le lancement, seulement quand ils bougent.
	static int s_retards_vus = 0, s_limites_vus = 0;
	const int retards = s_nb_retards, limites = s_nb_limites;
	if(( retards != s_retards_vus ) || ( limites != s_limites_vus ))
	{
		s_retards_vus = retards;
		s_limites_vus = limites;
		VLOG( "SND", "melange : %d retard(s) du port (trou = craquement), "
		      "limiteur %d fois (gain %.2f) ; pire calcul %u us, pire ecart %u us "
		      "(grain %d = %d us)", retards, limites, s_limite,
		      s_us_calcul_max, s_us_ecart_max, s_grain, s_grain * 1000000 / SORTIE_HZ );
		s_us_calcul_max = 0;
		s_us_ecart_max  = 0;
	}
}

void VitaListeVoix( void )
{
	const unsigned long long t = sceKernelGetProcessTimeWide();
	int n = 0;
	for( int i = 0; i < NUM_VOICES; ++i )
		if( s_voix[i].active )
		{
			++n;
			VLOG( "SND", "  voix %d : '%s'%s, volumes %d/%d, depuis %.1f s", i,
			      s_voix[i].p_son ? s_voix[i].p_son->nom : "?",
			      s_voix[i].boucle ? " EN BOUCLE" : "",
			      s_voix[i].vol_g, s_voix[i].vol_d,
			      (float)( t - s_voix[i].t_debut ) / 1000000.0f );
		}
	VLOG( "SND", "voix actives : %d", n );
}

// Le moteur s'en sert pour decider s'il peut charger d'autres sons. La Vita
// n'a pas de memoire audio dediee : on annonce une reserve confortable prise
// sur la memoire principale, plutot que zero, qui ferait renoncer le moteur.
int GetMemAvailable( void )					{ return 8 * 1024 * 1024; }

} // namespace Sfx
