///////////////////////////////////////////////////////////////////////////////
// p_sfx.h — effets sonores, backend Vita
//
// Les .pcm du jeu (sounds\pcm\...) sont au MÊME format que la musique — de
// l'Xbox ADPCM — mais en mono, 44100 Hz, blockAlign 36. Le décodeur est donc
// partagé : Core/Vita/adpcm.h.
//
// Différence de fond avec la musique : le jeu joue plusieurs effets EN MÊME
// TEMPS, alors que sceAudioOut ne donne qu'un flux. Il faut donc un mixeur
// logiciel — voir p_sfx.cpp.

#ifndef __GEL_SOUNDFX_VITA_P_SFX_H__
#define __GEL_SOUNDFX_VITA_P_SFX_H__

#include <core/defines.h>

namespace Sfx
{

// Le moteur en réserve un par son joué et teste l'index rendu par
// PlaySoundPlease contre cette borne.
#define NUM_VOICES		24

class CSfxManager;
struct sVolume;

// Un son décodé, gardé en mémoire. Le moteur ne regarde jamais dedans : il ne
// manipule que le PlatformWaveInfo qui le porte.
struct SSonVita
{
	short	*p_echantillons;	// PCM 16 bits mono
	int		 nb_echantillons;
	int		 hz;
	bool	 boucle;
	char	 nom[40];			// #57 : pour nommer une voix dans le journal
};

struct PlatformWaveInfo
{
	SSonVita	*p_sound_data;
	bool		 looping;		// lu directement par soundfx.cpp
	bool		 permanent;
};

extern float	gSfxVolume;

// #57 : liste les voix actives (nom, boucle, volumes, age) dans le journal.
void	VitaListeVoix( void );
// #30 : "mix N", enregistre N s de la sortie des effets (ux0:data/thug/mix.wav).
void	VitaEnregistreMix( int secondes );

void	InitSoundFX( CSfxManager *p_sfx_manager );
void	CleanUpSoundFX( void );
void	StopAllSoundFX( void );
bool	LoadSoundPlease( const char *sfxName, uint32 checksum,
                         PlatformWaveInfo *pInfo, bool loadPerm = 0 );

// Rend 0..NUM_VOICES-1, ou -1 si aucune voix n'est libre.
int		PlaySoundPlease( PlatformWaveInfo *pInfo, sVolume *p_vol,
                         float pitch = 100.0f );

void	StopSoundPlease( int whichVoice );
int		GetMemAvailable( void );
void	PauseSoundsPlease( void );
void	SetReverbPlease( float reverbLevel = 0.0f, int reverbMode = 0,
                         bool instant = false );
void	SetVolumePlease( float volumeLevel );
bool	VoiceIsOn( int whichVoice );
void	SetVoiceParameters( int whichVoice, sVolume *p_vol, float pitch = 0.0 );
void	PerFrameUpdate( void );

} // namespace Sfx

#endif // __GEL_SOUNDFX_VITA_P_SFX_H__
