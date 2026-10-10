/*****************************************************************************
**  THUG-Vita — backend musique / PCM                                       **
**  Code/Gel/Music/Vita/p_music.cpp                                         **
**                                                                          **
**  LA MUSIQUE JOUE. Ce fichier n'est plus une rangée de stubs.             **
**                                                                          **
**  Tout le format a été établi mécaniquement AVANT d'écrire une ligne de   **
**  C++, avec vita/tools/audio_check.py (même méthode que cas_check.py) :   **
**                                                                          **
**  [VÉRIFIÉ] data/streams/pcm/music_pcm.dat est un index :                  **
**            uint32 nombre, puis nombre x { checksum, offset, taille }.     **
**            Preuve : 4 + 167*12 == 2008 == taille exacte du fichier,       **
**            aucun chevauchement, tout tient dans le .wad.                  **
**                                                                          **
**  [VÉRIFIÉ] Les entrées sont triées par checksum croissant, pour une       **
**            recherche binaire. Confirmé par le backend d'origine :         **
**            Gel/Music/Xbox/p_music.cpp:575.                                **
**                                                                          **
**  [VÉRIFIÉ] Le .wad concatène des RIFF/WAVE dont le formatTag vaut         **
**            0x0069 = XBOX ADPCM — et NON du PCM, malgré le nom du          **
**            dossier. 48000 Hz, stéréo, blockAlign 72, 4 bits.              **
**            Contrôle : 48000/64 échantillons par bloc * 72 octets = 54000, **
**            exactement l'avgBytesPerSec de l'en-tête.                      **
**                                                                          **
**  [VÉRIFIÉ] data/streams/wma/ est VIDE sur cette ISO (0 octet). Aucun      **
**            décodeur WMA n'est donc nécessaire — c'est ce qui rend ce      **
**            chantier faisable.                                             **
**                                                                          **
**  Xbox ADPCM est une variante d'IMA ADPCM. Par bloc et par canal :         **
**  4 octets d'en-tête (prédicteur int16, index de pas uint8, un octet       **
**  réservé) puis les données par groupes de 4 octets alternant les canaux.  **
**                                                                          **
**  VOIX (issue #20) : les flux de pcm.wad -- dialogues de mission,       **
**  donneurs d'objectifs -- sont joues par le bloc "Flux de voix" plus     **
**  bas, avec le meme decodeur (mono, blockAlign 36, 11025..48000 Hz).     **
**  Les cinematiques n'en dependent pas : leur son passe par la piste     **
**  musique (PreLoadMusicStream).                                         **
*****************************************************************************/

#include <core/defines.h>
#include <gel/music/Vita/p_music.h>
#include <core/crc.h>
#include <core/Vita/adpcm.h>
#include <core/macros.h>
#include <gel/soundfx/soundfx.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vita_log.h"

SVitaStreamInfo	gCurrentStreamInfo[ NUM_STREAMS ];

namespace
{

// --- format, tel que mesuré ------------------------------------------------

#define ADPCM_BLOC			72		/* blockAlign, stéréo */
#define ADPCM_ECH_PAR_BLOC	64		/* (72/2 - 4) * 2 */
#define AUDIO_HZ			48000
#define AUDIO_CANAUX		2

// Granularité de sortie. sceAudioOutOutput bloque jusqu'à ce que le tampon
// précédent soit consommé : c'est LUI qui cadence le thread, on n'a aucune
// horloge à tenir nous-mêmes.
#define GRAIN				1024	/* échantillons par appel, multiple de 64 */

// Lecture disque par gros blocs. Une lecture de 576 octets par appel de sortie
// ferait ~90 accès disque par seconde ; sur Vita le stockage est le vrai
// goulot, et c'est exactement le genre d'I/O qui coûte des images.
// 910 blocs = 65520 octets = ~1,2 s d'audio par lecture.
#define BLOCS_PAR_LECTURE	910
#define TAILLE_LECTURE		( BLOCS_PAR_LECTURE * ADPCM_BLOC )

static const char *CHEMIN_MUSIC_DAT = "ux0:data/thug/Data/streams/pcm/music_pcm.dat";
static const char *CHEMIN_MUSIC_WAD = "ux0:data/thug/Data/streams/pcm/music_pcm.wad";

// --- index ------------------------------------------------------------------

struct SEntree
{
	uint32	checksum;
	uint32	offset;
	uint32	taille;
};

static SEntree	*s_index			= NULL;
static int		 s_index_nb			= 0;

// --- état de lecture --------------------------------------------------------

static SceUID	 s_wad				= -1;
static int		 s_port				= -1;
static SceUID	 s_thread			= -1;
static bool		 s_thread_tourne	= false;

// Protège les champs partagés avec le thread. Le moteur appelle Play/Stop
// depuis le thread principal pendant que le thread audio lit : sans verrou on
// libère un tampon en cours de décodage.
static SceUID	 s_mutex			= -1;

static volatile bool	s_joue			= false;
static volatile bool	s_pause			= false;
static volatile bool	s_fini			= true;
static volatile uint32	s_prechargee	= 0;
static volatile bool	s_a_precharger	= false;

// Position dans le morceau courant.
static uint32	s_pos_debut			= 0;	/* offset des données dans le .wad */
static uint32	s_pos_taille		= 0;	/* octets de données ADPCM */
static uint32	s_pos_lue			= 0;	/* octets déjà consommés */

static float	s_volume			= 50.0f;	// pourcentage 0..100 (DEFAULT_MUSIC_VOLUME)

// Tampons. Alloués une fois : allouer dans le thread audio à chaque tour
// fragmenterait le tas pendant le jeu.
static unsigned char	*s_lecture	= NULL;
// DOUBLE TAMPON (#30) : sceAudioOutOutput ne copie pas le tampon, la Vita le
// lit PENDANT qu'il joue. Recalculer le grain suivant dans le meme tampon
// ecrasait le son en cours de lecture -- le craquement entendu sur console
// alors que l'enregistrement du melange (pris avant l'envoi) etait propre.
// On alterne entre deux moities, comme le pilote audio Vita de SDL.
static short			*s_sortie	= NULL;	// moitie en cours de decodage
static short			*s_sortie_base	= NULL;	// deux grains

// Le decodeur vit dans Core/Vita/adpcm.cpp : les effets sonores utilisent
// exactement le meme format (mono, blockAlign 36, au lieu de stereo 72), et
// une seule implementation validee vaut mieux que deux copies.

// --- index ------------------------------------------------------------------

static int cmp_entree( const void *a, const void *b )
{
	const uint32 ca = ((const SEntree *)a)->checksum;
	const uint32 cb = ((const SEntree *)b)->checksum;
	return ( ca < cb ) ? -1 : (( ca > cb ) ? 1 : 0 );
}

static bool charge_index( void )
{
	SceUID f = sceIoOpen( CHEMIN_MUSIC_DAT, SCE_O_RDONLY, 0 );
	if( f < 0 )
	{
		VLOG( "PCM", "index musique introuvable : %s", CHEMIN_MUSIC_DAT );
		return false;
	}

	uint32 nb = 0;
	if( sceIoRead( f, &nb, 4 ) != 4 )
	{
		sceIoClose( f );
		return false;
	}

	// Garde-fou : un nombre absurde signifie un fichier tronqué ou d'un autre
	// format. Mieux vaut ne pas jouer de musique que d'allouer 4 Go.
	if(( nb == 0 ) || ( nb > 100000 ))
	{
		VLOG( "PCM", "index musique : nombre d'entrees invraisemblable (%u)", nb );
		sceIoClose( f );
		return false;
	}

	s_index = (SEntree *)malloc( nb * sizeof( SEntree ));
	if( !s_index )
	{
		sceIoClose( f );
		return false;
	}

	const int voulu = (int)( nb * sizeof( SEntree ));
	const int lu    = sceIoRead( f, s_index, voulu );
	sceIoClose( f );

	if( lu != voulu )
	{
		VLOG( "PCM", "index musique tronque : %d octets sur %d", lu, voulu );
		free( s_index );
		s_index = NULL;
		return false;
	}

	s_index_nb = (int)nb;

	// Le backend d'origine compte sur un index déjà trié. On le retrie quand
	// même : c'est instantané sur quelques centaines d'entrées, et une
	// recherche binaire sur des données non triées échouerait silencieusement.
	qsort( s_index, s_index_nb, sizeof( SEntree ), cmp_entree );

	VLOG( "PCM", "index musique : %d pistes", s_index_nb );
	return true;
}

static const SEntree *trouve( uint32 checksum )
{
	int lo = 0, hi = s_index_nb - 1;
	while( lo <= hi )
	{
		const int mid = ( lo + hi ) / 2;
		if( s_index[mid].checksum == checksum )	return &s_index[mid];
		if( s_index[mid].checksum <  checksum )	lo = mid + 1;
		else									hi = mid - 1;
	}
	return NULL;
}

// --- thread de lecture ------------------------------------------------------

// Lit l'en-tête RIFF du morceau et positionne la lecture sur ses données.
// Rend false si l'en-tête n'est pas celui attendu — auquel cas on ne joue
// rien plutôt que d'interpréter des octets au hasard comme du son.
static bool prepare_morceau( const SEntree *p_e )
{
	unsigned char entete[64];

	if( sceIoLseek( s_wad, p_e->offset, SCE_SEEK_SET ) < 0 )
		return false;
	if( sceIoRead( s_wad, entete, sizeof( entete )) != (int)sizeof( entete ))
		return false;

	// « fmt » à l'offset 12, « data » à 40 : disposition RIFF fixe de ces
	// fichiers, vérifiée sur l'ISO.
	if( memcmp( entete + 12, "fmt ", 4 ) != 0 )
	{
		VLOG( "PCM", "morceau %08x : pas de bloc fmt", p_e->checksum );
		return false;
	}
	if( memcmp( entete + 40, "data", 4 ) != 0 )
	{
		VLOG( "PCM", "morceau %08x : pas de bloc data", p_e->checksum );
		return false;
	}

	const uint16 tag = (uint16)( entete[20] | ( entete[21] << 8 ));
	if( tag != 0x0069 )
	{
		VLOG( "PCM", "morceau %08x : format 0x%04x non gere", p_e->checksum, tag );
		return false;
	}

	uint32 taille_data;
	memcpy( &taille_data, entete + 44, 4 );

	// L'en-tête pourrait annoncer plus que ce que l'entrée contient.
	if( taille_data > ( p_e->taille - 48 ))
		taille_data = p_e->taille - 48;

	s_pos_debut  = p_e->offset + 48;
	s_pos_taille = taille_data;
	s_pos_lue    = 0;
	return true;
}

static int thread_audio( SceSize, void * )
{
	while( s_thread_tourne )
	{
		bool joue;
		sceKernelLockMutex( s_mutex, 1, NULL );
		joue = s_joue && !s_pause;
		sceKernelUnlockMutex( s_mutex, 1 );

		if( !joue )
		{
			// Rien à faire : on rend la main. Sans cette pause, ce thread
			// mangerait un cœur entier pendant les menus silencieux.
			sceKernelDelayThread( 10000 );
			continue;
		}

		sceKernelLockMutex( s_mutex, 1, NULL );

		uint32 reste = ( s_pos_taille > s_pos_lue ) ? ( s_pos_taille - s_pos_lue ) : 0;
		if( reste == 0 )
		{
			s_joue = false;
			s_fini = true;
			sceKernelUnlockMutex( s_mutex, 1 );
			continue;
		}

		uint32 a_lire = TAILLE_LECTURE;
		if( a_lire > reste )
			a_lire = ( reste / ADPCM_BLOC ) * ADPCM_BLOC;	/* blocs entiers */
		if( a_lire == 0 )
		{
			s_joue = false;
			s_fini = true;
			sceKernelUnlockMutex( s_mutex, 1 );
			continue;
		}

		sceIoLseek( s_wad, s_pos_debut + s_pos_lue, SCE_SEEK_SET );
		const int lu = sceIoRead( s_wad, s_lecture, a_lire );
		if( lu <= 0 )
		{
			s_joue = false;
			s_fini = true;
			sceKernelUnlockMutex( s_mutex, 1 );
			continue;
		}
		s_pos_lue += lu;

		sceKernelUnlockMutex( s_mutex, 1 );

		// Décodage puis sortie, PAR GRAIN. sceAudioOutOutput bloque : le
		// verrou est relâché avant, sinon le thread principal resterait
		// bloqué sur Stop pendant toute la durée d'un tampon.
		const int blocs = lu / ADPCM_BLOC;
		int bloc = 0;
		while(( bloc < blocs ) && s_thread_tourne )
		{
			const int blocs_grain = GRAIN / ADPCM_ECH_PAR_BLOC;
			int n = blocs_grain;
			if( bloc + n > blocs )
				n = blocs - bloc;

			for( int i = 0; i < n; ++i )
			{
				VitaAdpcm::DecodeBloc(
					s_lecture + ( bloc + i ) * ADPCM_BLOC,
					s_sortie + i * ADPCM_ECH_PAR_BLOC * AUDIO_CANAUX,
					AUDIO_CANAUX, ADPCM_BLOC );
			}

			// Un grain incomplet en fin de morceau : on complète par du
			// silence plutôt que d'envoyer les restes du tampon précédent.
			const int ech_remplis = n * ADPCM_ECH_PAR_BLOC;
			if( ech_remplis < GRAIN )
			{
				memset( s_sortie + ech_remplis * AUDIO_CANAUX, 0,
				        ( GRAIN - ech_remplis ) * AUDIO_CANAUX * sizeof( short ));
			}

			sceAudioOutOutput( s_port, s_sortie );
			s_sortie = ( s_sortie == s_sortie_base )
			           ? s_sortie_base + GRAIN * AUDIO_CANAUX : s_sortie_base;
			bloc += n;

			if( s_pause || !s_joue )
				break;
		}
	}
	return 0;
}

static void applique_volume( void )
{
	if( s_port < 0 )
		return;
	int v = (int)( s_volume * 0.01f * SCE_AUDIO_VOLUME_0DB );	// pourcentage, comme Xbox (p_adpcmfilestream.cpp:767)
	if( v < 0 )						v = 0;
	if( v > SCE_AUDIO_VOLUME_0DB )	v = SCE_AUDIO_VOLUME_0DB;
	int vols[2] = { v, v };
	sceAudioOutSetVolume( s_port,
	                      (SceAudioOutChannelFlag)( SCE_AUDIO_VOLUME_FLAG_L_CH |
	                                                SCE_AUDIO_VOLUME_FLAG_R_CH ),
	                      vols );
}

// Le moteur fabrique ses noms de piste a la mode PS2 :
// Sk/Scripting/cfuncs.cpp:3724 prefixe � MUSIC\\VAG\\SONGS\\ � -- VAG etant le
// format audio de la PlayStation 2. Or l'index du .wad Xbox est construit sur
// le nom NU.
//
// [VERIFIE] sur table : le CRC de � ACEYALONE � tombe dans l'index, celui de
// � MUSIC\\VAG\\SONGS\\ACEYALONE � non ; 5 noms testes sur 5. Le CRC lui-meme a
// ete valide contre quatre constantes ecrites en dur dans le moteur (loop,
// is_frontend, MusicVolume, MusicStreamVolume).
static uint32 checksum_de_piste( const char *p_nom )
{
	// On ne garde que ce qui suit le dernier separateur, quel qu'il soit.
	const char *p_court = p_nom;
	for( const char *p = p_nom; *p; ++p )
	{
		if(( *p == '\\' ) || ( *p == '/' ))
			p_court = p + 1;
	}
	return Crc::GenerateCRCFromString( p_court );
}

// Le moteur appelle StopMusic et consorts meme quand l'audio n'a pas pu
// s'initialiser (fichiers absents sur la carte). Verrouiller un mutex jamais
// cree n'est pas fatal, mais c'est une erreur silencieuse par appel : on
// enveloppe une fois pour toutes.
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

static bool demarre( uint32 checksum )
{
	if(( s_wad < 0 ) || !s_index )
		return false;

	const SEntree *p_e = trouve( checksum );
	if( !p_e )
	{
		VLOG( "PCM", "morceau %08x absent de l'index", checksum );
		return false;
	}

	verrouille();
	const bool ok = prepare_morceau( p_e );
	if( ok )
	{
		s_joue  = true;
		s_pause = false;
		s_fini  = false;
		VLOG( "PCM", "lecture %08x : %u octets", checksum, s_pos_taille );
	}
	deverrouille();
	return ok;
}

} // namespace anonyme

// ---------------------------------------------------------------------------
// Flux de voix : data/streams/pcm/pcm.wad (issue #20)
// ---------------------------------------------------------------------------
//
// Les dialogues de mission (donneurs d'objectifs, Eric, les pros...) passent
// par ce chemin, PAS par la musique :
//
//   CGoalPed::PlayGoalStream (Sk/Modules/Skate/GoalPed.cpp:400) commence par
//   Pcm::StreamExists -> PCMAudio_FindNameFromChecksum (music.cpp:2277). Ce
//   stub rendait 0 : le moteur concluait que la voix n'existait pas et ne
//   tentait meme pas de la jouer. Derriere, PreLoadStream / goal_play_stream
//   (goal_utilities.qb : boucle sur PreLoadStreamDone puis
//   StartPreloadedStream volume=190) tombaient sur des stubs muets.
//
//   Les cinematiques, elles, ont leur voix parce qu'elles passent par
//   Pcm::PreLoadMusicStream (Sk/Objects/cutscenedetails.cpp:4157) : la piste
//   "musique" de music_pcm.wad, implementee plus haut.
//
// Contrat suivi : Gel/Music/Xbox/p_music.cpp (PlayStream:1266,
// PreLoadStream:740, GetStreamStatus:882, FindNameFromChecksum:1019,
// SetStreamVolume:1035). Pitch ignore, comme sur Xbox (SetStreamPitch:1105).
//
// [VERIFIE] sur table, avant d'ecrire ce code : pcm.dat = 4 + 2385*12 octets,
// meme structure que music_pcm.dat, mais NON trie (le tri est obligatoire,
// Xbox le fait a l'init, p_music.cpp:575). Les 2385 entrees sont des RIFF
// Xbox ADPCM MONO, blockAlign 36, fmt puis data (donnees a l'octet 48) ;
// frequences : 44100 Hz (2352), 48000 (30), 22050 (2), 11025 (1). On
// reechantillonne donc vers 48000 en virgule fixe 16.16.
//
// Architecture : un thread et UN port audio (VOICE, repli MAIN) melangent les
// NUM_STREAMS canaux. Les lectures disque se font dans ce thread, HORS verrou,
// par sceIoPread (pas de position partagee a proteger) ; un numero de
// generation par canal jette une lecture devenue caduque si le thread
// principal a arrete/relance le canal pendant ce temps.

namespace
{

#define VOIX_HZ				48000
#define VOIX_GRAIN_MAX		1024
// Lecture par paquets de 455 blocs de 36 octets = 16380 octets, ~0,37 s a
// 44100 Hz. On recharge quand il reste moins de VOIX_SEUIL octets : un grain
// de 1024 echantillons a 48000 Hz consomme au plus 16 blocs = 576 octets.
#define VOIX_LECTURE		16380
#define VOIX_SEUIL			4096
#define VOIX_TAMPON			( VOIX_LECTURE + VOIX_SEUIL )

static const char *CHEMIN_VOIX_DAT = "ux0:data/thug/Data/streams/pcm/pcm.dat";
static const char *CHEMIN_VOIX_WAD = "ux0:data/thug/Data/streams/pcm/pcm.wad";

struct SCanalVoix
{
	// Etat vu par le thread principal (music.cpp) : actif <=> pas FREE.
	volatile bool	actif;
	volatile bool	ok_jouer;		// faux tant qu'un PreLoad n'est pas lance
	volatile bool	pause;
	volatile bool	pret;			// premier paquet lu (ou fin atteinte)
	uint32			generation;
	uint32			checksum;

	// Position dans pcm.wad.
	uint32			debut;			// offset des donnees ADPCM
	uint32			taille;			// octets de donnees
	uint32			lu;				// octets deja lus du disque
	int				bloc;			// blockAlign (36)
	int				ech_bloc;		// echantillons par bloc (64)

	// Tampon brut : ecrit par le seul thread de melange.
	unsigned char	*p_brut;
	int				brut_octets;
	int				brut_pos;

	// Bloc decode et reechantillonnage.
	short			pcm[64];
	int				pcm_n;
	int				pcm_pos;
	unsigned int	pas;			// 16.16, frequence source / 48000
	unsigned int	frac;
	int				e0, e1;			// interpolation lineaire entre e0 et e1
	bool			fin_donnees;

	int				vol_g;			// 0..256
	int				vol_d;
};

static SEntree		*s_voix_index		= NULL;
static int			 s_voix_index_nb	= 0;
static SceUID		 s_voix_wad			= -1;
static int			 s_voix_port		= -1;
static int			 s_voix_grain		= 512;
static SceUID		 s_voix_thread		= -1;
static SceUID		 s_voix_mutex		= -1;
static volatile bool s_voix_tourne		= false;
static short		*s_voix_sortie		= NULL;	// double tampon, voir s_sortie
static short		*s_voix_sortie_base	= NULL;
static SCanalVoix	 s_canal[NUM_STREAMS];

static inline void verrouille_voix( void )
{
	if( s_voix_mutex >= 0 )
		sceKernelLockMutex( s_voix_mutex, 1, NULL );
}

static inline void deverrouille_voix( void )
{
	if( s_voix_mutex >= 0 )
		sceKernelUnlockMutex( s_voix_mutex, 1 );
}

static const SEntree *trouve_voix( uint32 checksum )
{
	int lo = 0, hi = s_voix_index_nb - 1;
	while( lo <= hi )
	{
		const int mid = ( lo + hi ) / 2;
		if( s_voix_index[mid].checksum == checksum )	return &s_voix_index[mid];
		if( s_voix_index[mid].checksum <  checksum )	lo = mid + 1;
		else											hi = mid - 1;
	}
	return NULL;
}

static bool charge_index_voix( void )
{
	SceUID f = sceIoOpen( CHEMIN_VOIX_DAT, SCE_O_RDONLY, 0 );
	if( f < 0 )
	{
		VLOG( "PCM", "index des voix introuvable : %s", CHEMIN_VOIX_DAT );
		return false;
	}

	uint32 nb = 0;
	if(( sceIoRead( f, &nb, 4 ) != 4 ) || ( nb == 0 ) || ( nb > 100000 ))
	{
		VLOG( "PCM", "index des voix : en-tete invalide (%u)", nb );
		sceIoClose( f );
		return false;
	}

	s_voix_index = (SEntree *)malloc( nb * sizeof( SEntree ));
	if( !s_voix_index )
	{
		sceIoClose( f );
		return false;
	}

	const int voulu = (int)( nb * sizeof( SEntree ));
	const int lu    = sceIoRead( f, s_voix_index, voulu );
	sceIoClose( f );
	if( lu != voulu )
	{
		VLOG( "PCM", "index des voix tronque : %d octets sur %d", lu, voulu );
		free( s_voix_index );
		s_voix_index = NULL;
		return false;
	}

	// pcm.dat n'est PAS trie sur l'ISO (verifie) : sans ce tri, la recherche
	// binaire manquerait la plupart des voix, sans le moindre message.
	s_voix_index_nb = (int)nb;
	qsort( s_voix_index, s_voix_index_nb, sizeof( SEntree ), cmp_entree );
	return true;
}

// Lit l'en-tete RIFF de l'entree. On parcourt les blocs au lieu de supposer
// "data" a l'octet 48 : c'est le cas des 2385 entrees de l'ISO USA, mais un
// bloc supplementaire (bext...) a deja piege les effets sonores.
static bool lit_entete_voix( const SEntree *p_e, uint32 *p_debut, uint32 *p_taille,
                             uint32 *p_hz, int *p_bloc )
{
	unsigned char h[128];
	if( sceIoPread( s_voix_wad, h, sizeof( h ), p_e->offset ) != (int)sizeof( h ))
		return false;
	if(( memcmp( h, "RIFF", 4 ) != 0 ) || ( memcmp( h + 8, "WAVE", 4 ) != 0 ))
		return false;

	bool fmt_ok = false;
	uint32 p = 12;
	while( p + 8 <= sizeof( h ))
	{
		uint32 lg;
		memcpy( &lg, h + p + 4, 4 );
		if( memcmp( h + p, "fmt ", 4 ) == 0 )
		{
			if( p + 8 + 16 > sizeof( h ))
				return false;
			const uint16 tag    = (uint16)( h[p + 8]  | ( h[p + 9]  << 8 ));
			const uint16 canaux = (uint16)( h[p + 10] | ( h[p + 11] << 8 ));
			uint32 hz;
			memcpy( &hz, h + p + 12, 4 );
			const uint16 bloc   = (uint16)( h[p + 20] | ( h[p + 21] << 8 ));
			if(( tag != 0x0069 ) || ( canaux != 1 ) || ( bloc != 36 ) ||
			   ( hz < 8000 ) || ( hz > VOIX_HZ ))
			{
				VLOG( "PCM", "voix %08x : format non gere (tag 0x%04x, %u canaux, %u Hz, bloc %u)",
				      p_e->checksum, tag, canaux, hz, bloc );
				return false;
			}
			*p_hz   = hz;
			*p_bloc = bloc;
			fmt_ok  = true;
		}
		else if( memcmp( h + p, "data", 4 ) == 0 )
		{
			if( !fmt_ok )
				return false;
			const uint32 dispo = ( p_e->taille > p + 8 ) ? ( p_e->taille - ( p + 8 )) : 0;
			*p_debut  = p_e->offset + p + 8;
			*p_taille = ( lg < dispo ) ? lg : dispo;
			return true;
		}
		p += 8 + lg + ( lg & 1 );
	}
	return false;
}

// Volumes du moteur (pourcentages, signe = phase PS2) -> 0..256, multiplies
// par le volume general des effets comme sur Xbox (PERCENT( GetMainVolume,
// canal ), p_music.cpp:1046). Xbox plafonne a 0 dB (DSBVOLUME_MAX) : le
// "volume = 190" de goal_play_stream revient donc au plein volume.
static void volumes_voix( float g, float d, int *p_g, int *p_d )
{
	if( g < 0.0f )	g = -g;
	if( d < 0.0f )	d = -d;

	float general = 100.0f;
	Sfx::CSfxManager *p_sfx = Sfx::CSfxManager::Instance();
	if( p_sfx )
		general = p_sfx->GetMainVolume();

	g = PERCENT( general, g );
	d = PERCENT( general, d );
	if( g > 100.0f )	g = 100.0f;
	if( d > 100.0f )	d = 100.0f;

	*p_g = (int)( g * 2.56f );
	*p_d = (int)( d * 2.56f );
}

// Echantillon source suivant. Rend false a la fin des donnees, ou si le
// tampon brut est vide (lecture en retard : le grain se termine en silence).
static bool echantillon_suivant( SCanalVoix *p_c, int *p_e )
{
	if( p_c->pcm_pos >= p_c->pcm_n )
	{
		if( p_c->brut_octets - p_c->brut_pos < p_c->bloc )
		{
			if( p_c->lu >= p_c->taille )
				p_c->fin_donnees = true;
			return false;
		}
		VitaAdpcm::DecodeBloc( p_c->p_brut + p_c->brut_pos, p_c->pcm, 1, p_c->bloc );
		p_c->brut_pos += p_c->bloc;
		p_c->pcm_n     = p_c->ech_bloc;
		p_c->pcm_pos   = 0;
	}
	*p_e = p_c->pcm[p_c->pcm_pos++];
	return true;
}

// Recharge le tampon brut du canal si besoin. Appele par le thread de
// melange uniquement ; le disque est lu verrou RELACHE.
static void recharge_canal( int i )
{
	SCanalVoix *p_c = &s_canal[i];

	verrouille_voix();
	if( !p_c->actif || ( p_c->lu >= p_c->taille ) ||
	    ( p_c->brut_octets - p_c->brut_pos >= VOIX_SEUIL ))
	{
		deverrouille_voix();
		return;
	}

	// Le reste non consomme passe en tete, la lecture se fait a la suite.
	const int reste = p_c->brut_octets - p_c->brut_pos;
	if(( reste > 0 ) && ( p_c->brut_pos > 0 ))
		memmove( p_c->p_brut, p_c->p_brut + p_c->brut_pos, reste );
	p_c->brut_octets = reste;
	p_c->brut_pos    = 0;

	uint32 a_lire = VOIX_TAMPON - reste;
	a_lire -= a_lire % p_c->bloc;
	if( a_lire > p_c->taille - p_c->lu )
		a_lire = p_c->taille - p_c->lu;

	const uint32 gen    = p_c->generation;
	const uint32 offset = p_c->debut + p_c->lu;
	unsigned char *p_dst = p_c->p_brut + reste;
	deverrouille_voix();

	const int lu = sceIoPread( s_voix_wad, p_dst, a_lire, offset );

	verrouille_voix();
	if( p_c->actif && ( p_c->generation == gen ))
	{
		if( lu > 0 )
		{
			p_c->brut_octets += lu;
			p_c->lu          += lu;
		}
		else
		{
			// Lecture en echec : on termine proprement le flux au lieu de
			// laisser le moteur attendre une voix qui ne viendra plus.
			VLOG( "PCM", "voix %08x : lecture en echec (0x%08x), flux arrete", p_c->checksum, lu );
			p_c->taille = p_c->lu;
		}
		p_c->pret = true;
	}
	deverrouille_voix();
}

static int thread_voix( SceSize, void * )
{
	static int accu[VOIX_GRAIN_MAX * 2];

	while( s_voix_tourne )
	{
		for( int i = 0; i < NUM_STREAMS; ++i )
			recharge_canal( i );

		const int grain = s_voix_grain;
		bool quelque_chose = false;
		memset( accu, 0, grain * 2 * sizeof( int ));

		verrouille_voix();
		for( int i = 0; i < NUM_STREAMS; ++i )
		{
			SCanalVoix *p_c = &s_canal[i];
			if( !p_c->actif || !p_c->ok_jouer || p_c->pause )
				continue;

			quelque_chose = true;
			for( int n = 0; n < grain; ++n )
			{
				const int e = p_c->e0 + ((( p_c->e1 - p_c->e0 ) * (int)( p_c->frac >> 1 )) >> 15 );
				accu[n * 2    ] += ( e * p_c->vol_g ) >> 8;
				accu[n * 2 + 1] += ( e * p_c->vol_d ) >> 8;

				p_c->frac += p_c->pas;
				bool manque = false;
				while( p_c->frac >= 0x10000 )
				{
					int suivant;
					if( !echantillon_suivant( p_c, &suivant ))
					{
						manque = true;
						break;
					}
					p_c->frac -= 0x10000;
					p_c->e0 = p_c->e1;
					p_c->e1 = suivant;
				}
				if( manque )
				{
					// Pas de position perdue en cas de lecture en retard :
					// on reprendra a ce point au grain suivant.
					if( p_c->frac >= 0x10000 )
						p_c->frac = 0x10000 - 1;
					break;
				}
			}

			if( p_c->fin_donnees )
			{
				p_c->actif = false;
				VLOG( "PCM", "voix %08x terminee (canal %d)", p_c->checksum, i );
			}
		}
		deverrouille_voix();

		if( !quelque_chose )
		{
			sceKernelDelayThread( 5000 );
			continue;
		}

		for( int n = 0; n < grain * 2; ++n )
		{
			int e = accu[n];
			if( e >  32767 )	e =  32767;
			if( e < -32768 )	e = -32768;
			s_voix_sortie[n] = (short)e;
		}
		sceAudioOutOutput( s_voix_port, s_voix_sortie );
		s_voix_sortie = ( s_voix_sortie == s_voix_sortie_base )
		                ? s_voix_sortie_base + VOIX_GRAIN_MAX * 2 : s_voix_sortie_base;
	}
	return 0;
}

static void voix_init( void )
{
	if( !charge_index_voix() )
	{
		VLOG( "PCM", "pas de voix : index indisponible" );
		return;
	}

	s_voix_wad = sceIoOpen( CHEMIN_VOIX_WAD, SCE_O_RDONLY, 0 );
	if( s_voix_wad < 0 )
	{
		VLOG( "PCM", "pas de voix : %s introuvable", CHEMIN_VOIX_WAD );
		free( s_voix_index );
		s_voix_index    = NULL;
		s_voix_index_nb = 0;
		return;
	}

	bool alloc_ok = true;
	s_voix_sortie_base = (short *)memalign( 64, 2 * VOIX_GRAIN_MAX * 2 * sizeof( short ));
	s_voix_sortie = s_voix_sortie_base;
	if( !s_voix_sortie )
		alloc_ok = false;
	for( int i = 0; i < NUM_STREAMS; ++i )
	{
		memset( &s_canal[i], 0, sizeof( SCanalVoix ));
		s_canal[i].p_brut = (unsigned char *)malloc( VOIX_TAMPON );
		if( !s_canal[i].p_brut )
			alloc_ok = false;
	}
	if( !alloc_ok )
	{
		VLOG( "PCM", "pas de voix : allocation des tampons impossible" );
		return;
	}

	// Un port a part : le port BGM est pris par la musique, et le melangeur
	// des effets (Gel/SoundFX/Vita) a le sien. Le port VOICE est prevu pour
	// la voix ; repli sur un port MAIN s'il est refuse.
	const char *p_type = "VOICE";
	s_voix_grain = VOIX_GRAIN_MAX;
	s_voix_port  = sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_VOICE, s_voix_grain,
	                                    VOIX_HZ, SCE_AUDIO_OUT_MODE_STEREO );
	if( s_voix_port < 0 )
	{
		VLOG( "PCM", "port VOICE refuse (0x%08x), repli sur MAIN", s_voix_port );
		p_type       = "MAIN";
		s_voix_grain = 512;		/* limite du port MAIN, cf. Gel/SoundFX/Vita */
		s_voix_port  = sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_MAIN, s_voix_grain,
		                                    VOIX_HZ, SCE_AUDIO_OUT_MODE_STEREO );
	}
	if( s_voix_port < 0 )
	{
		VLOG( "PCM", "pas de voix : port audio refuse (0x%08x)", s_voix_port );
		return;
	}

	s_voix_mutex  = sceKernelCreateMutex( "thug_voix", 0, 0, NULL );
	s_voix_tourne = true;
	s_voix_thread = sceKernelCreateThread( "thug_voix", thread_voix,
	                                       0x10000100, 0x4000, 0, 0, NULL );
	if( s_voix_thread >= 0 )
		sceKernelStartThread( s_voix_thread, 0, NULL );
	else
	{
		s_voix_tourne = false;
		VLOG( "PCM", "pas de voix : thread refuse (0x%08x)", s_voix_thread );
		return;
	}

	VLOG( "PCM", "voix pretes : %d flux, %d canaux, port %s grain %d",
	      s_voix_index_nb, NUM_STREAMS, p_type, s_voix_grain );
}

static inline bool voix_dispo( void )
{
	return s_voix_tourne && ( s_voix_wad >= 0 ) && ( s_voix_index != NULL );
}

static inline bool canal_valide( int i )
{
	return ( i >= 0 ) && ( i < NUM_STREAMS );
}

static bool voix_demarre( uint32 checksum, int canal, float vol_g, float vol_d, bool preload )
{
	if( !voix_dispo() || !canal_valide( canal ))
		return false;

	const SEntree *p_e = trouve_voix( checksum );
	if( !p_e )
	{
		VLOG( "PCM", "voix %08x absente de pcm.dat", checksum );
		return false;
	}

	// Comme Xbox (p_music.cpp:1285) : un canal occupe n'est pas ecrase ici,
	// music.cpp arrete d'abord le canal de plus basse priorite.
	if( s_canal[canal].actif )
		return false;

	uint32 debut, taille, hz;
	int bloc;
	if( !lit_entete_voix( p_e, &debut, &taille, &hz, &bloc ))
	{
		VLOG( "PCM", "voix %08x : en-tete RIFF illisible", checksum );
		return false;
	}

	int g, d;
	volumes_voix( vol_g, vol_d, &g, &d );

	verrouille_voix();
	SCanalVoix *p_c = &s_canal[canal];
	p_c->generation++;
	p_c->checksum    = checksum;
	p_c->debut       = debut;
	p_c->taille      = taille;
	p_c->lu          = 0;
	p_c->bloc        = bloc;
	p_c->ech_bloc    = VitaAdpcm::EchantillonsParBloc( bloc, 1 );
	if( p_c->ech_bloc > 64 )
		p_c->ech_bloc = 64;
	p_c->brut_octets = 0;
	p_c->brut_pos    = 0;
	p_c->pcm_n       = 0;
	p_c->pcm_pos     = 0;
	p_c->pas         = (unsigned int)(((unsigned long long)hz << 16 ) / VOIX_HZ );
	p_c->frac        = 0;
	p_c->e0          = 0;
	p_c->e1          = 0;
	p_c->fin_donnees = false;
	p_c->vol_g       = g;
	p_c->vol_d       = d;
	p_c->pause       = false;
	p_c->pret        = ( taille == 0 );
	p_c->ok_jouer    = !preload;
	p_c->actif       = true;
	deverrouille_voix();

	VLOG( "PCM", "voix %08x canal %d : %u Hz, %u octets%s, volume %d/%d",
	      checksum, canal, hz, taille, preload ? " (prechargee)" : "", g, d );
	return true;
}

static void voix_volume( int canal, float vol_g, float vol_d )
{
	if( !canal_valide( canal ) || !s_canal[canal].actif )
		return;
	int g, d;
	volumes_voix( vol_g, vol_d, &g, &d );
	verrouille_voix();
	s_canal[canal].vol_g = g;
	s_canal[canal].vol_d = d;
	deverrouille_voix();
}

static void voix_arrete( int canal )
{
	if( !canal_valide( canal ))
		return;
	verrouille_voix();
	if( s_canal[canal].actif )
	{
		s_canal[canal].actif = false;
		s_canal[canal].generation++;
	}
	deverrouille_voix();
}

} // namespace anonyme

// ---------------------------------------------------------------------------
// API attendue par Gel/Music/music.cpp
// ---------------------------------------------------------------------------

void	PCMAudio_Init( void )
{
	// Les voix d'abord : elles ne dependent pas de la musique, et le retour
	// anticipe ci-dessous (index musique absent) ne doit pas les priver.
	voix_init();

	if( !charge_index() )
	{
		VLOG( "PCM", "pas de musique : index indisponible" );
		return;
	}

	s_wad = sceIoOpen( CHEMIN_MUSIC_WAD, SCE_O_RDONLY, 0 );
	if( s_wad < 0 )
	{
		VLOG( "PCM", "pas de musique : %s introuvable", CHEMIN_MUSIC_WAD );
		free( s_index );
		s_index = NULL;
		s_index_nb = 0;
		return;
	}

	s_lecture = (unsigned char *)malloc( TAILLE_LECTURE );
	s_sortie_base = (short *)memalign( 64, 2 * GRAIN * AUDIO_CANAUX * sizeof( short ));
	s_sortie  = s_sortie_base;
	if( !s_lecture || !s_sortie )
	{
		VLOG( "PCM", "pas de musique : allocation des tampons impossible" );
		return;
	}

	s_port = sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN,
	                              AUDIO_HZ, SCE_AUDIO_OUT_MODE_STEREO );
	if( s_port < 0 )
	{
		VLOG( "PCM", "pas de musique : port audio refuse (0x%08x)", s_port );
		return;
	}
	applique_volume();

	s_mutex = sceKernelCreateMutex( "thug_pcm", 0, 0, NULL );

	s_thread_tourne = true;
	s_thread = sceKernelCreateThread( "thug_pcm", thread_audio,
	                                  0x10000100, 0x10000, 0, 0, NULL );
	if( s_thread >= 0 )
		sceKernelStartThread( s_thread, 0, NULL );
	else
		VLOG( "PCM", "pas de musique : thread refuse" );

	VLOG( "PCM", "audio pret : %d Hz stereo, %d pistes", AUDIO_HZ, s_index_nb );
}

int		PCMAudio_Update( void )
{
	return 0;
}

// --- musique ---------------------------------------------------------------

bool	PCMAudio_PlayMusicStream( uint32 checksum )
{
	return demarre( checksum );
}

bool	PCMAudio_PreLoadMusicStream( uint32 checksum )
{
	// Rien à précharger : la lecture est en flux, elle démarre immédiatement.
	// On mémorise la demande pour que StartPreLoaded l'exécute, comme attendu
	// par music.cpp qui sépare les deux étapes.
	s_prechargee   = checksum;
	s_a_precharger = true;
	return true;
}

bool	PCMAudio_PreLoadMusicStreamDone( void )
{
	// Toujours prêt : aucune étape de chargement séparée n'existe ici.
	// Répondre false ferait attendre le moteur indéfiniment — c'est
	// exactement ce qui figeait les cinématiques.
	return true;
}

bool	PCMAudio_StartPreLoadedMusicStream( void )
{
	if( !s_a_precharger )
		return false;
	s_a_precharger = false;
	return demarre( s_prechargee );
}

void	PCMAudio_StopMusic( bool )
{
	verrouille();
	s_joue = false;
	s_fini = true;
	deverrouille();
}

// Routage par canal comme Xbox (p_music.cpp:915). Avant les voix, ce stub
// mettait la MUSIQUE en pause aussi pour Pcm::PauseStream (EXTRA_CHANNEL).
void	PCMAudio_Pause( bool pause, int ch )
{
	if( ch == MUSIC_CHANNEL )
	{
		s_pause = pause;
		return;
	}

	// Seuls les flux en cours sont touches : un flux lance ensuite part non
	// suspendu, comme un nouveau CADPCMFileStream sur Xbox.
	verrouille_voix();
	for( int s = 0; s < NUM_STREAMS; ++s )
	{
		if( s_canal[s].actif )
			s_canal[s].pause = pause;
	}
	deverrouille_voix();
}

int		PCMAudio_SetMusicVolume( float volume )
{
	s_volume = volume;
	applique_volume();
	return 0;
}

int		PCMAudio_GetMusicStatus( void )
{
	// FREE quand rien ne joue : le moteur enchaîne. RUNNING pendant la
	// lecture, sinon il croirait le morceau fini et en lancerait un autre
	// par-dessus.
	return s_joue ? PCM_STATUS_RUNNING : PCM_STATUS_FREE;
}

// --- pistes nommees ---------------------------------------------------------

// Le chemin "musique" du moteur passe par des NOMS, pas par des checksums :
// music.cpp:1504 (AddTrackToPlaylist) et music.cpp:570 (PlayMusicTrack). Le nom
// est converti avec le CRC du moteur, celui-la meme qui a servi a construire
// l'index -- minuscules, '/' devenant '\\'.
//
// C'est ce maillon qui manquait : tant que TrackExists rendait false, AUCUNE
// piste n'entrait dans les playlists (le return precede numTracks++), donc le
// jeu ne demandait jamais de musique, et l'audio pouvait etre parfaitement
// initialise sans qu'une note ne sorte.
bool	PCMAudio_PlayMusicTrack( const char *p_nom, bool )
{
	if( !p_nom )
		return false;
	return demarre( checksum_de_piste( p_nom ));
}
bool	PCMAudio_PlaySoundtrackMusicTrack( int, int )		{ return true; }

// --- flux de voix (pcm.wad) : voir le bloc "Flux de voix" plus haut ---------

bool	PCMAudio_PlayStream( uint32 checksum, int whichStream, float volumeL, float volumeR,
                             float, bool preload )
{
	return voix_demarre( checksum, whichStream, volumeL, volumeR, preload );
}

bool	PCMAudio_PlayStream( uint32 checksum, int whichStream, Sfx::sVolume *p_volume,
                             float, bool preload )
{
	// Xbox passe NULL depuis PreLoadStream (p_music.cpp:746) : volume plein,
	// StartPreLoadedStream fixera le vrai.
	float g = 100.0f, d = 100.0f;
	if( p_volume )
	{
		g = p_volume->GetChannelVolume( 0 );
		d = p_volume->GetChannelVolume( 1 );
	}
	return voix_demarre( checksum, whichStream, g, d, preload );
}

void	PCMAudio_StopStream( int whichStream, bool )
{
	voix_arrete( whichStream );
}

void	PCMAudio_StopStreams( void )
{
	for( int i = 0; i < NUM_STREAMS; ++i )
		voix_arrete( i );
}

// Comme Xbox (p_music.cpp:740) : le flux est ouvert tout de suite, mais ne
// joue qu'a StartPreLoadedStream. Le thread de melange lit le premier paquet
// entre-temps ; PreLoadStreamDone le signale.
bool	PCMAudio_PreLoadStream( uint32 checksum, int whichStream )
{
	return voix_demarre( checksum, whichStream, 100.0f, 100.0f, true );
}

bool	PCMAudio_PreLoadStreamDone( int whichStream )
{
	// Xbox rend true quand le canal n'a pas de flux (p_music.cpp:773).
	if( !canal_valide( whichStream ) || !s_canal[whichStream].actif )
		return true;
	return s_canal[whichStream].pret;
}

static bool lance_prechargee( int whichStream, float g, float d )
{
	if( !canal_valide( whichStream ) || !s_canal[whichStream].actif )
		return false;
	voix_volume( whichStream, g, d );
	verrouille_voix();
	s_canal[whichStream].ok_jouer = true;
	deverrouille_voix();
	return true;
}

bool	PCMAudio_StartPreLoadedStream( int whichStream, float volumeL, float volumeR, float )
{
	return lance_prechargee( whichStream, volumeL, volumeR );
}

bool	PCMAudio_StartPreLoadedStream( int whichStream, Sfx::sVolume *p_volume, float )
{
	float g = 100.0f, d = 100.0f;
	if( p_volume )
	{
		g = p_volume->GetChannelVolume( 0 );
		d = p_volume->GetChannelVolume( 1 );
	}
	return lance_prechargee( whichStream, g, d );
}

bool	PCMAudio_SetStreamVolume( float volumeL, float volumeR, int whichStream )
{
	voix_volume( whichStream, volumeL, volumeR );
	return true;
}

bool	PCMAudio_SetStreamVolume( Sfx::sVolume *p_volume, int whichStream )
{
	if( p_volume )
		voix_volume( whichStream, p_volume->GetChannelVolume( 0 ), p_volume->GetChannelVolume( 1 ));
	return true;
}

// Ignore, comme sur Xbox (p_music.cpp:1105).
bool	PCMAudio_SetStreamPitch( float, int )				{ return true; }

int		PCMAudio_GetStreamStatus( int whichStream )
{
	// -1 = "n'importe lequel" : FREE des qu'un canal est libre (Xbox:882).
	int debut = whichStream, fin = whichStream + 1;
	if( whichStream == -1 )
	{
		debut = 0;
		fin   = NUM_STREAMS;
	}
	for( int s = debut; s < fin; ++s )
	{
		if( !canal_valide( s ) || !s_canal[s].actif )
			return PCM_STATUS_FREE;
	}
	return PCM_STATUS_RUNNING;
}

bool	PCMAudio_TrackExists( const char *p_nom, int )
{
	if( !p_nom || !s_index )
		return false;

	const uint32 cs = checksum_de_piste( p_nom );
	const bool   ok = ( trouve( cs ) != NULL );

	// Une trace du couple nom -> checksum vivait ici. Elle a servi une fois,
	// decisivement : elle a montre que le moteur demandait
	// "MUSIC\VAG\SONGS\ACEYALONE" quand l'index attend "ACEYALONE".
	// A remettre si une piste ne se lance pas.
	return ok;
}

// L'index tient lieu d'en-tete : il est charge une fois pour toutes par
// PCMAudio_Init et contient deja noms (par checksum), offsets et tailles.
bool	PCMAudio_LoadMusicHeader( const char * )
{
	return ( s_index != NULL );
}

// "Legacy call left over from PS2 code" (Xbox, p_music.cpp:982) : true. Rendre
// false mettrait streams_hed_there a faux et desactiverait TOUS les flux
// (music.cpp:2228, StreamsDisabled). Aucun script de l'ISO ne l'appelle.
bool	PCMAudio_LoadStreamHeader( const char * )			{ return true; }

// Pcm::StreamExists (music.cpp:2277) : c'est par ici que CGoalPed decide
// s'il y a une voix a jouer (GoalPed.cpp:347, 400). Xbox : p_music.cpp:1019.
uint32	PCMAudio_FindNameFromChecksum( uint32 checksum, int ch )
{
	if(( ch != EXTRA_CHANNEL ) || !s_voix_index )
		return 0;
	if( !trouve_voix( checksum ))
		return 0;
	VLOG( "PCM", "voix %08x presente", checksum );
	return checksum;
}
