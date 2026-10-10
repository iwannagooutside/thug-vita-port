/*****************************************************************************
**  THUG-Vita -- lecture des FMV Bink (issue #3)                            **
**  Code/Gel/Movies/Vita/p_movies.h                                         **
**                                                                          **
**  Meme contrat que gel/movies/xbox/p_movies.h, sans bink.h (le decodeur   **
**  est un FFmpeg minimal, voir vita_bink.h).                               **
*****************************************************************************/

#ifndef __P_MOVIES_H
#define __P_MOVIES_H

#include <core/defines.h>

namespace Flx
{

	void PMovies_PlayMovie( const char *pName );

} // namespace Flx

#endif	// __P_MOVIES_H
