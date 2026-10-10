/*****************************************************************************
**  THUG-Vita -- lecteur Bink 1 (FMV)                                       **
**  Code/Gel/Movies/Vita/vita_bink.h                                        **
**                                                                          **
**  Petite API C au-dessus d'un FFmpeg minimal (demuxeur bink, decodeurs    **
**  binkvideo + binkaudio_dct ; voir build_ffmpeg_bink.sh). Le moteur est   **
**  en gnu++98 et les en-tetes FFmpeg n'ont pas de garde extern "C" : tout  **
**  ce qui touche a libav* reste dans vita_bink.c (C99), p_movies.cpp ne    **
**  voit que ceci.                                                          **
**                                                                          **
**  Ajoute au build par vita/CMakeLists.txt (THUG_FMV), hors du glob *.cpp. **
**  FFmpeg est LGPL 2.1+ : ce fichier et vita_bink.c en derivent.           **
*****************************************************************************/

#ifndef VITA_BINK_H
#define VITA_BINK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VitaBink VitaBink;

typedef struct VitaBinkInfo
{
	int		width, height;			/* 640x480 pour tout le disque USA */
	int		fps_num, fps_den;		/* 2997/100 */
	int		frames;					/* nombre d'images annonce par l'en-tete */
	int		sample_rate;			/* 44100, 0 si pas d'audio */
	int		channels;				/* 2 */
} VitaBinkInfo;

/* Image decodee : trois plans 8 bits YUV 4:2:0 BT.601 plage limitee (Bink
 * version 'i'), pointeurs valides jusqu'au prochain vita_bink_next_frame. */
typedef struct VitaBinkFrame
{
	const unsigned char	*plane[3];
	int					 pitch[3];
	int					 index;		/* numero d'image, 0..frames-1 */
} VitaBinkFrame;

/* Callback audio : echantillons s16 entrelaces (count = par canal), deja
 * multiplies par le volume passe a l'ouverture. Appele depuis
 * vita_bink_next_frame, dans le thread du decodeur. */
typedef void (*VitaBinkAudioCb)( void *user, const short *pcm, int count );

VitaBink	*vita_bink_open( const char *path, VitaBinkInfo *info,
							 VitaBinkAudioCb audio_cb, void *user, float volume );

/* Lit et decode jusqu'a la prochaine image video (l'audio rencontre en route
 * part dans le callback). 1 = image rendue, 0 = fin du fichier, <0 = erreur. */
int			 vita_bink_next_frame( VitaBink *b, VitaBinkFrame *out );

void		 vita_bink_close( VitaBink *b );

#ifdef __cplusplus
}
#endif

#endif /* VITA_BINK_H */
