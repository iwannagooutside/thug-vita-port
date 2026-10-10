/*****************************************************************************
**  THUG-Vita — backend VitaGL                                              **
**  Code/Gfx/Vita/p_nx.cpp — Nx::CEngine                                    **
**                                                                          **
**  Palier 1 : ces fonctions n'ont qu'un rôle, faire passer l'édition de    **
**  liens et signaler au palier 2 ce que le moteur atteint réellement.      **
**  Aucune ne rend un pixel.                                                **
**                                                                          **
**  GÉNÉRÉ par vita/tools/gen_stubs.py à partir des déclarations de         **
**  Code/Gfx/nx.h — les signatures doivent correspondre EXACTEMENT, sinon   **
**  le symbole reste non résolu et on ne s'en aperçoit qu'au link.          **
**  Régénérer plutôt que corriger à la main quand nx.h change.              **
**                                                                          **
**  Les valeurs de retour sont neutres (false / NULL / 0). Certaines seront  **
**  fausses au sens du moteur — c'est assumé au palier 1, et c'est          **
**  précisément ce que les logs VITA_STUB() serviront à repérer au palier 2. **
*****************************************************************************/

#include <float.h>
#include <stdlib.h>
#include <gfx/nx.h>
#include <gfx/nxtexman.h>
#include <gfx/nxviewman.h>
#include <gfx/nxsector.h>
#include <gfx/nxmesh.h>
#include <gfx/nxmodel.h>
#include <gfx/nxwin2d.h>

#include <gfx/nxviewport.h>
#include <gfx/NxQuickAnim.h>
#include <gfx/NxNewParticleMgr.h>
#include "p_NxNewParticle.h"
#include "p_NxParticle.h"
#include <gfx/NxScene.h>
#include <gfx/nxweather.h>
#include "p_NxScene.h"
#include "p_scene_load.h"
#include "p_world_render.h"
#include "p_occlusion.h"
#include "p_screenshot.h"
#include "p_NxSprite.h"
#include "p_NxFont.h"
#include "p_NxModel.h"
#include "p_debug_hud.h"
#include "p_gamma.h"
#include "p_ombre.h"

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>

#include "vita_log.h"
#include "vita_dbgsrv.h"
extern "C" int vita_ecran_boot( void );
#include <sys/file/pre.h>
#include "p_stub.h"
#include <gel/scripting/script.h>

namespace Nx
{

// Definie dans p_nx_managers.cpp.
void VitaDessineEcranChargementSiActif( void );

// Definie dans p_NxModel.cpp : dessine les morceaux translucides des modeles,
// mis de cote pendant la phase logique et rejoues une fois le decor pose.
void VitaDessinerModelesReportes( void );
int VitaDessinsEnAttente( void );
void VitaLibererDifferes( void );
extern bool g_vita_image_ouverte;
}
extern "C" { extern int vita_glspy_ouverte, vita_glspy_phase; void vita_glspy_bilan( void ); }
namespace Nx { void VitaBudgetEclairageImage( void ); }
namespace Nx {

// --- SONDE MULTITEXTURE ----------------------------------------------------
//
// Question a laquelle elle repond, et une seule : le pipeline fixe de VitaGL
// sait-il composer DEUX couches de texture, et combien d'unites expose-t-il ?
//
// Pourquoi une sonde plutot qu'une lecture du header : celui-ci declare
// GL_COMBINE, GL_TEXTURE0..3 et glActiveTexture -- mais on a deja appris ici
// qu'une DECLARATION N'EST PAS UN SUPPORT. glCompressedTexImage2D acceptait le
// DXT5 sans lever la moindre erreur et n'en restituait pas l'alpha ; le voile
// du menu sortait blanc. Le header ne fait pas foi, la mesure oui.
//
// Enjeu : 495 des 1424 materiaux de New Jersey declarent 2 a 4 couches, et
// elles couvrent 80 % de la surface visible (mesure : marquage magenta). XBox
// les compose en UN SEUL appel via un pixel shader genere a la volee
// (XBox/NX/render.cpp:302) ; la traduction naturelle en pipeline fixe est le
// multitexturing. Si la sonde echoue, il faut concevoir autrement AVANT
// d'ecrire quoi que ce soit.
//
// Methode : deux textures unies de valeurs connues, 128 et 64, et une lecture
// du pixel obtenu. Les quatre issues possibles sont bien separees, donc le
// resultat se lit sans ambiguite :
//
//     32  = MODULATE compose      -> le chemin est ouvert
//    192  = ADD compose           -> idem
//    128  = unite 1 IGNOREE       -> une seule couche, chemin ferme
//     64  = unite 1 en REPLACE    -> compose, mais ecrase au lieu de combiner
//
// PAS de glBegin/glEnd : le mode immediat de VitaGL puise dans un legacy pool
// dont la taille est le premier argument de vglInitExtended, et nous y passons
// zero (voir p_NxSprite.cpp:221). Vertex arrays uniquement.

static GLuint probe_make_1x1( unsigned char v )
{
	GLuint t = 0;
	unsigned char px[4] = { v, v, v, 255 };
	glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D, t );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE, px );
	return t;
}

static int probe_draw( GLuint tex_a, GLuint tex_b, GLenum mode_unite1 )
{
	static const float verts[12] = { -1.0f, -1.0f, 0.0f,
	                                  1.0f, -1.0f, 0.0f,
	                                  1.0f,  1.0f, 0.0f,
	                                 -1.0f,  1.0f, 0.0f };
	static const float uvs[8]    = { 0.0f, 0.0f, 1.0f, 0.0f,
	                                 1.0f, 1.0f, 0.0f, 1.0f };

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glDisableClientState( GL_COLOR_ARRAY );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	glMatrixMode( GL_PROJECTION ); glPushMatrix(); glLoadIdentity();
	glMatrixMode( GL_MODELVIEW );  glPushMatrix(); glLoadIdentity();

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 3, GL_FLOAT, 0, verts );

	// Couche 0 : la texture telle quelle.
	glActiveTexture( GL_TEXTURE0 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, tex_a );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	glClientActiveTexture( GL_TEXTURE0 );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uvs );

	// Couche 1 : celle dont on teste la composition.
	glActiveTexture( GL_TEXTURE1 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, tex_b );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode_unite1 );
	glClientActiveTexture( GL_TEXTURE1 );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uvs );

	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	unsigned char px[4] = { 0, 0, 0, 0 };
	glReadPixels( 480, 272, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px );

	// Remise en etat : la couche 1 doit disparaitre completement, sinon elle
	// teinterait tout le decor pour le reste de la partie.
	glClientActiveTexture( GL_TEXTURE1 );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glActiveTexture( GL_TEXTURE1 );
	glDisable( GL_TEXTURE_2D );
	glActiveTexture( GL_TEXTURE0 );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	glClientActiveTexture( GL_TEXTURE0 );

	glMatrixMode( GL_PROJECTION ); glPopMatrix();
	glMatrixMode( GL_MODELVIEW );  glPopMatrix();

	return (int)px[0];
}

// --- SONDE DXT1 A ALPHA 1 BIT ----------------------------------------------
//
// Question unique : vitaGL restitue-t-il l'alpha 1 bit du DXT1 ?
//
// Enjeu, mesure sur table (NJ.tex.xbx, 800 textures) : 237 DXT1 portent des
// blocs a alpha, et 40 ont plus de 5 % de leurs texels transparents -- jusqu'a
// 95 %. Une texture transparente a 95 % rendue opaque donne un bloc plein,
// exactement le symptome rapporte sur les feuillages.
//
// On sait deja que vitaGL accepte le DXT5 sans en restituer l'alpha (d'ou son
// decodage logiciel). Meme famille, mais ON NE SUPPOSE PAS : la meme session a
// failli conclure a tort que le format 2 etait du DXT3, alors que le source
// XBox traite 1 et 2 identiquement.
//
// Methode : un bloc 4x4 dont les deux lignes du haut sont OPAQUES et les deux
// du bas TRANSPARENTES, dessine en melange alpha sur un fond vert franc.
//   haut rouge + bas VERT   -> l'alpha passe, rien a corriger
//   haut et bas identiques  -> l'alpha est perdu, il faut decoder en logiciel

static void probe_dxt1_alpha( void )
{
	// color0 <= color1 : c'est ce qui arme le mode a alpha 1 bit du DXT1.
	// color0 = noir, color1 = rouge vif ; indices 1 (opaque) puis 3 (transparent).
	const unsigned char bloc[8] = {
		0x00, 0x00,				// color0 = 0x0000, noir
		0x00, 0xF8,				// color1 = 0xF800, rouge (little endian)
		0x55, 0x55,				// texels 0-7  : index 1 -> rouge opaque
		0xFF, 0xFF				// texels 8-15 : index 3 -> transparent
	};

	static const float verts[12] = { -1.0f, -1.0f, 0.0f,   1.0f, -1.0f, 0.0f,
	                                  1.0f,  1.0f, 0.0f,  -1.0f,  1.0f, 0.0f };
	static const float uvs[8]    = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };

	GLuint tex = 0;
	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	while( glGetError() != GL_NO_ERROR ) { }
	glCompressedTexImage2D( GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
	                        4, 4, 0, 8, bloc );
	const GLenum err_up = glGetError();

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisableClientState( GL_COLOR_ARRAY );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	glMatrixMode( GL_PROJECTION ); glPushMatrix(); glLoadIdentity();
	glMatrixMode( GL_MODELVIEW );  glPushMatrix(); glLoadIdentity();

	// Fond VERT franc : il ne peut venir que de la transparence du texel.
	glClearColor( 0.0f, 1.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	glActiveTexture( GL_TEXTURE0 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 3, GL_FLOAT, 0, verts );
	glClientActiveTexture( GL_TEXTURE0 );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uvs );

	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	unsigned char haut[4] = { 0, 0, 0, 0 };
	unsigned char bas[4]  = { 0, 0, 0, 0 };
	glReadPixels( 480, 410, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, haut );
	glReadPixels( 480, 130, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bas );

	VLOG( "GFX", "sonde DXT1 : upload err=0x%x  quart haut=(%d,%d,%d)  "
	             "quart bas=(%d,%d,%d)",
	      (unsigned)err_up, haut[0], haut[1], haut[2], bas[0], bas[1], bas[2] );

	// Un des deux quarts doit etre VERT si l'alpha est restitue.
	const bool vert_haut = ( haut[1] > 150 ) && ( haut[0] < 100 );
	const bool vert_bas  = ( bas[1]  > 150 ) && ( bas[0]  < 100 );
	VLOG( "GFX", "sonde DXT1 : verdict %s",
	      ( vert_haut != vert_bas )
	        ? "ALPHA RESTITUE -- rien a corriger"
	        : "ALPHA PERDU -- decodage logiciel necessaire" );

	glDisable( GL_BLEND );
	glDeleteTextures( 1, &tex );
	glMatrixMode( GL_PROJECTION ); glPopMatrix();
	glMatrixMode( GL_MODELVIEW );  glPopMatrix();
	glClearColor( 0.0f, 0.0f, 0.25f, 1.0f );
	// REMISE EN ETAT COMPLETE. Une sonde qui laisse un etat derriere elle est
	// pire qu'une sonde absente : GL_REPLACE oublie sur l'unite 0 supprime la
	// modulation par les couleurs de sommets, donc l'eclairage cuit de TOUT le
	// jeu -- menu compris. C'est la regression que l'humain a vue.
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_TEXTURE_2D );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glEnable( GL_DEPTH_TEST );
	while( glGetError() != GL_NO_ERROR ) { }
}

// --- SONDE DU TEST ALPHA ---------------------------------------------------
//
// Question unique : vitaGL honore-t-il glAlphaFunc / GL_ALPHA_TEST ?
//
// C'est le mecanisme par lequel XBox decoupe feuillages et grillages : le
// materiau porte un seuil (RS_ALPHACUTOFF -> D3DRS_ALPHAREF), le maillage
// reste OPAQUE -- profondeur ecrite -- et les pixels sous le seuil sont
// rejetes. 1169 des 1424 materiaux de New Jersey portent un seuil.
//
// Sans ce test, une texture transparente a 95 % s'affiche en bloc plein.
//
// Meme bloc DXT1 que la sonde precedente : haut opaque, bas transparent. Cette
// fois SANS melange, avec le seul test alpha au seuil 1/255.
static void probe_alpha_test( void )
{
	const unsigned char bloc[8] = { 0x00, 0x00, 0x00, 0xF8,
	                                0x55, 0x55, 0xFF, 0xFF };
	static const float verts[12] = { -1.0f, -1.0f, 0.0f,   1.0f, -1.0f, 0.0f,
	                                  1.0f,  1.0f, 0.0f,  -1.0f,  1.0f, 0.0f };
	static const float uvs[8]    = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };

	GLuint tex = 0;
	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glCompressedTexImage2D( GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
	                        4, 4, 0, 8, bloc );

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_BLEND );					// AUCUN melange : seul le test agit
	glDisableClientState( GL_COLOR_ARRAY );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	while( glGetError() != GL_NO_ERROR ) { }
	glEnable( GL_ALPHA_TEST );
	glAlphaFunc( GL_GEQUAL, 1.0f / 255.0f );
	const GLenum err_at = glGetError();

	glMatrixMode( GL_PROJECTION ); glPushMatrix(); glLoadIdentity();
	glMatrixMode( GL_MODELVIEW );  glPushMatrix(); glLoadIdentity();

	glClearColor( 0.0f, 1.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	glActiveTexture( GL_TEXTURE0 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 3, GL_FLOAT, 0, verts );
	glClientActiveTexture( GL_TEXTURE0 );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uvs );

	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	unsigned char haut[4] = { 0, 0, 0, 0 };
	unsigned char bas[4]  = { 0, 0, 0, 0 };
	glReadPixels( 480, 410, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, haut );
	glReadPixels( 480, 130, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bas );

	VLOG( "GFX", "sonde test alpha : err=0x%x  quart haut=(%d,%d,%d)  "
	             "quart bas=(%d,%d,%d)",
	      (unsigned)err_at, haut[0], haut[1], haut[2], bas[0], bas[1], bas[2] );

	const bool vert_haut = ( haut[1] > 150 ) && ( haut[0] < 100 );
	const bool vert_bas  = ( bas[1]  > 150 ) && ( bas[0]  < 100 );
	VLOG( "GFX", "sonde test alpha : verdict %s",
	      ( vert_haut != vert_bas )
	        ? "TEST ALPHA HONORE -- les decoupes sont possibles"
	        : "TEST ALPHA IGNORE -- il faudra une autre voie" );

	glDisable( GL_ALPHA_TEST );
	glDeleteTextures( 1, &tex );
	glMatrixMode( GL_PROJECTION ); glPopMatrix();
	glMatrixMode( GL_MODELVIEW );  glPopMatrix();
	glClearColor( 0.0f, 0.0f, 0.25f, 1.0f );
	// REMISE EN ETAT COMPLETE. Une sonde qui laisse un etat derriere elle est
	// pire qu'une sonde absente : GL_REPLACE oublie sur l'unite 0 supprime la
	// modulation par les couleurs de sommets, donc l'eclairage cuit de TOUT le
	// jeu -- menu compris. C'est la regression que l'humain a vue.
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_TEXTURE_2D );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glEnable( GL_DEPTH_TEST );
	while( glGetError() != GL_NO_ERROR ) { }
}

// --- SONDE DU COMBINEUR GL_INTERPOLATE -------------------------------------
//
// Question unique : vitaGL honore-t-il GL_COMBINE / GL_INTERPOLATE avec
// l'ALPHA DE LA COUCHE PRECEDENTE comme facteur ?
//
// C'est l'operation dont depend tout le decor a masque : la couche 0 porte une
// forme dans son ALPHA (mesure sur la rampe de New Jersey : luminance 1..16,
// donc noire partout, alpha 7..255 nettement contraste), et la couche 1 porte
// la matiere. Le moteur interpole de l'une vers l'autre selon cet alpha.
//
// Sans ce combineur, on peint le pochoir noir au lieu du beton -- exactement
// les � arches noires � observees.
//
// Le header declare GL_COMBINE, GL_INTERPOLATE et GL_SRC0..2. Cela ne prouve
// rien : le DXT5 etait accepte sans que son alpha soit restitue.
//
// Methode : unite 0 = ROUGE a alpha 0,5 ; unite 1 = VERT, en INTERPOLATE avec
// l'alpha du precedent. Les issues sont bien separees :
//     (128,128,0) : interpolation reelle -> le chemin est ouvert
//     (  0,255,0) : la couche 1 REMPLACE  -> combineur ignore
//     (255,  0,0) : la couche 1 est ignoree
//     (  0,  0,0) : modulation (rouge x vert)
static void probe_combineur( void )
{
	const unsigned char rouge_semi[4] = { 255, 0, 0, 128 };
	const unsigned char vert[4]       = { 0, 255, 0, 255 };
	static const float verts[12] = { -1.0f, -1.0f, 0.0f,   1.0f, -1.0f, 0.0f,
	                                  1.0f,  1.0f, 0.0f,  -1.0f,  1.0f, 0.0f };
	static const float uvs[8]    = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };

	GLuint t0 = 0, t1 = 0;
	glGenTextures( 1, &t0 );
	glBindTexture( GL_TEXTURE_2D, t0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rouge_semi );
	glGenTextures( 1, &t1 );
	glBindTexture( GL_TEXTURE_2D, t1 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, vert );

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_BLEND );
	glDisableClientState( GL_COLOR_ARRAY );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	glMatrixMode( GL_PROJECTION ); glPushMatrix(); glLoadIdentity();
	glMatrixMode( GL_MODELVIEW );  glPushMatrix(); glLoadIdentity();
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 3, GL_FLOAT, 0, verts );

	glActiveTexture( GL_TEXTURE0 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, t0 );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	glClientActiveTexture( GL_TEXTURE0 );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uvs );

	while( glGetError() != GL_NO_ERROR ) { }
	glActiveTexture( GL_TEXTURE1 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, t1 );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
	glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB,      GL_INTERPOLATE );
	glTexEnvi( GL_TEXTURE_ENV, GL_SRC0_RGB,     GL_TEXTURE );
	glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
	glTexEnvi( GL_TEXTURE_ENV, GL_SRC1_RGB,     GL_PREVIOUS );
	glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
	glTexEnvi( GL_TEXTURE_ENV, GL_SRC2_RGB,     GL_PREVIOUS );
	glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA );
	const GLenum err = glGetError();
	glClientActiveTexture( GL_TEXTURE1 );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uvs );
	glClientActiveTexture( GL_TEXTURE0 );
	glActiveTexture( GL_TEXTURE0 );

	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	unsigned char px[4] = { 0, 0, 0, 0 };
	glReadPixels( 480, 272, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px );
	VLOG( "GFX", "sonde combineur : err=0x%x  pixel=(%d,%d,%d)  attendu (128,128,0)",
	      (unsigned)err, px[0], px[1], px[2] );

	const char *verdict;
	if(( px[0] > 90 ) && ( px[0] < 170 ) && ( px[1] > 90 ) && ( px[1] < 170 ))
		verdict = "INTERPOLATION REELLE -- le chemin est ouvert";
	else if(( px[1] > 200 ) && ( px[0] < 60 ))
		verdict = "la couche 1 REMPLACE -- combineur ignore";
	else if(( px[0] > 200 ) && ( px[1] < 60 ))
		verdict = "la couche 1 est IGNOREE";
	else
		verdict = "resultat inattendu -- lire les valeurs";
	VLOG( "GFX", "sonde combineur : verdict %s", verdict );

	// Remise en etat : sans quoi le combineur teinte tout le jeu.
	glActiveTexture( GL_TEXTURE1 );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	glDisable( GL_TEXTURE_2D );
	glClientActiveTexture( GL_TEXTURE1 );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glActiveTexture( GL_TEXTURE0 );
	glClientActiveTexture( GL_TEXTURE0 );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_TEXTURE_2D );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );
	glDeleteTextures( 1, &t0 );
	glDeleteTextures( 1, &t1 );
	glMatrixMode( GL_PROJECTION ); glPopMatrix();
	glMatrixMode( GL_MODELVIEW );  glPopMatrix();
	glClearColor( 0.45f, 0.05f, 0.65f, 1.0f );
	glEnable( GL_DEPTH_TEST );
	while( glGetError() != GL_NO_ERROR ) { }
}

static void probe_multitexture( void )
{
	// Sentinelle : si glGetIntegerv ne connait pas l'enum, la valeur reste a
	// -1 au lieu de passer pour un zero legitime.
	GLint units = -1, img_units = -1;
	while( glGetError() != GL_NO_ERROR ) { }
	glGetIntegerv( GL_MAX_TEXTURE_UNITS, &units );
	GLenum e1 = glGetError();
	glGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS, &img_units );
	GLenum e2 = glGetError();
	VLOG( "GFX", "sonde multitexture : MAX_TEXTURE_UNITS=%d (err 0x%x)  "
	             "MAX_TEXTURE_IMAGE_UNITS=%d (err 0x%x)",
	      (int)units, (unsigned)e1, (int)img_units, (unsigned)e2 );

	GLuint tex_a = probe_make_1x1( 128 );
	GLuint tex_b = probe_make_1x1( 64 );

	const int mod = probe_draw( tex_a, tex_b, GL_MODULATE );
	const int add = probe_draw( tex_a, tex_b, GL_ADD );

	VLOG( "GFX", "sonde : MODULATE -> %d (attendu 32)   ADD -> %d (attendu 192)",
	      mod, add );
	VLOG( "GFX", "sonde : verdict %s",
	      ( mod == 128 && add == 128 ) ? "COUCHE 1 IGNOREE -- chemin ferme"
	      : (( mod < 64 ) ? "COMPOSITION REELLE -- chemin ouvert"
	                      : "resultat inattendu, voir les valeurs" ));

	glDeleteTextures( 1, &tex_a );
	glDeleteTextures( 1, &tex_b );
	while( glGetError() != GL_NO_ERROR ) { }
}


// Initialisation de vitaGL et ecran de lancement, appelee le plus tot possible
// depuis main() (Sk/Main.cpp) : le moteur met ~4,5 s a arriver ici
// (s_plat_start_engine), et rien ne peut s'afficher avant vglInit -- l'ecran
// restait noir. Le tas newlib (192 Mo) est reserve au lancement du processus :
// initialiser vitaGL plus tot ne change rien a la memoire du moteur.
extern "C" void vita_vgl_init_tot( void )
{
	static bool s_fait = false;
	if( s_fait )
		return;
	s_fait = true;

	// Anneaux de commandes GXM (#18/#56). Par defaut : sommets 2 Mo, fragments
	// 512 Ko, VDM 128 Ko, parametres 16 Mo. Chaque dessin skinne pousse ses
	// matrices d'os en uniforms dans l'anneau de sommets : un millier par image
	// le fait deborder et sceGxm attend le GPU AU MILIEU des dessins (cout des
	// modeles skinnes tres variable, 40 a 450 ms/s pour un meme nombre).
	vglSetVertexBufferSize( 8 * 1024 * 1024 );
	vglSetFragmentBufferSize( 2 * 1024 * 1024 );
	vglSetVDMBufferSize( 512 * 1024 );
	vglSetParamBufferSize( 32 * 1024 * 1024 );
	GLboolean res_fallback = vglInitExtended( 0, 960, 544, 6 * 1024 * 1024,
											  SCE_GXM_MULTISAMPLE_NONE );
	VLOG( "GFX", "vglInit -> %d (%s)", (int)res_fallback,
		  res_fallback ? "resolution rabaissee" : "resolution accordee" );
	VLOG( "GFX", "GL_VERSION=%s", (const char *)glGetString( GL_VERSION ));

	// Ecran de lancement (vita/src/vita_ecran_boot.c) : l'image de pic0 reste
	// a l'ecran pendant le demarrage du moteur. A defaut, l'ancien effacement.
	if( !vita_ecran_boot())
	{
		glClearColor( 0.0f, 0.0f, 0.25f, 1.0f );	// bleu nuit : « le moteur rend »
		glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		vglSwapBuffers( GL_FALSE );
	}

}

void	CEngine::s_plat_start_engine()
{
	VITA_STUB();

	// PAS un stub : c'est ici que vitaGL doit être initialisé.
	//
	// Au palier 0, main_vita.cpp s'en chargeait. Ce fichier est sorti du build
	// quand Sk/Main.cpp est devenu le point d'entrée — donc plus personne
	// n'initialisait vitaGL, et aucun appel GL n'aurait pu aboutir.
	//
	// Rappel du palier 0 : la valeur de retour de vglInit* n'est PAS un statut
	// de succès, c'est un indicateur de repli de résolution. Ne pas la tester
	// comme une erreur.
	static bool s_inited = false;
	if( s_inited )
		return;
	s_inited = true;

	// vitaGL est initialisee des main() (Sk/Main.cpp) pour que l'ecran de
	// lancement s'affiche tout de suite ; sans effet si c'est deja fait.
	vita_vgl_init_tot();

	// Gestionnaire de particules. Chaque backend le cree ici (NGPS p_nx.cpp:75,
	// NGC p_nx.cpp:1500) ; sans lui mp_particle_manager reste NULL et le moteur
	// le dereference sans verifier -- a chaque frame dans sRenderWorld
	// (nx.cpp:232) comme a l'arret dans Mdl::Skate::Cleanup.
	// [VERIFIE par psp2core] Data abort dans CNewParticleManager::Cleanup,
	// this=0.
	// Les trois singletons que chaque backend fabrique dans son start_engine
	// (NGPS p_nx.cpp:75-77, NGC p_nx.cpp:1500/1533). Sans eux, les getters
	// statiques rendent NULL et le moteur les dereference sans verifier :
	//   - mp_particle_manager  -> nx.cpp:232, a CHAQUE frame
	//   - mp_weather           -> CScene::LoadCollision, et une vingtaine de
	//                             fonctions de script dans nx.h qui font
	//                             directement mp_weather->...
	// [VERIFIE par psp2core] les deux ont plante, l'un apres l'autre.
	// Sonde du multitexturing : une fois, juste apres l'init GL, avant que le
	// moteur n'installe le moindre etat.
	probe_multitexture();
	probe_dxt1_alpha();
	probe_alpha_test();
	probe_combineur();

	// Rampe gamma par defaut, comme XBox/NX/nx_init.cpp:354 (issue #45). Elle
	// s'applique a toute l'image affichee, HUD compris (p_gamma.cpp).
	NxVita::SetGammaNormalized( 0.14f, 0.13f, 0.12f );

	// Gestionnaire Vita (p_NxNewParticle.cpp) : simulation et rendu des
	// systemes NEWFLAT, coupes par defaut (" prt 0 ").
	mp_particle_manager = new CVitaNewParticleManager;
	mp_weather          = new CWeather;

	// L'ecran de la Vita est en 16:9 : meme chemin que la XBox reglee en
	// ecran large dans son tableau de bord (XBox/p_nx.cpp:134). Le script
	// (camera.qb) pose l'aspect 1,7778 et widescreen_camera_fov = 88,18 :
	// meme champ VERTICAL que les 72 degres en 4:3, le cadrage XBox (xemu).
	// Sans lui, 72 degres a l'horizontale en 16:9 zoomaient l'image de 1,3.
	Script::RunScript( "screen_setup_widescreen" );
}

// � mem 0/1 � : sonde periodique du tas et des pools vitaGL.
bool g_vita_sonde_memoire = false;

// Traceur d'a-coups : horodatage des etapes d'une image ; toute image de plus
// de 50 ms est tracee avec sa ventilation ("ACC"). Sans lui, une image de 200 ms
// ne dit pas si elle vient de la logique, du dessin ou de la presentation.
enum { ACC_DEBUT, ACC_OMBRE, ACC_PRE, ACC_MONDE0, ACC_MONDE, ACC_POST0, ACC_2D,
       ACC_GAMMA, ACC_DBG, ACC_SWAP, ACC_N };
static SceUInt64 s_acc[ ACC_N ];
static SceUInt64 s_acc_fin_prec = 0;
#define ACC( i )	( s_acc[ i ] = sceKernelGetProcessTimeWide() )
static void acc_bilan()
{
	NxVita::TeinteSceneBilanImage();	// issue #15
	const SceUInt64 tot = s_acc[ ACC_SWAP ] - s_acc_fin_prec;
	if( s_acc_fin_prec && ( tot > 50000 ) && ( tot < 2000000 ))
	{
		#define D( a, b )	(int)(( s_acc[ b ] - s_acc[ a ] ) / 1000 )
		VLOG( "ACC", "image %d ms : logique %d | ombre %d | pre %d | ->monde %d | monde %d | ->post %d | 2d %d | gamma %d | dbg %d | swap %d",
		      (int)( tot / 1000 ), (int)(( s_acc[ ACC_DEBUT ] - s_acc_fin_prec ) / 1000 ),
		      D( ACC_DEBUT, ACC_OMBRE ), D( ACC_OMBRE, ACC_PRE ), D( ACC_PRE, ACC_MONDE0 ),
		      D( ACC_MONDE0, ACC_MONDE ), D( ACC_MONDE, ACC_POST0 ), D( ACC_POST0, ACC_2D ),
		      D( ACC_2D, ACC_GAMMA ), D( ACC_GAMMA, ACC_DBG ), D( ACC_DBG, ACC_SWAP ));
		#undef D
	}
	s_acc_fin_prec = s_acc[ ACC_SWAP ];
}

void	CEngine::s_plat_pre_render()
{
	ACC( ACC_DEBUT );
	VITA_STUB();

	// Nouvelle frame : la vue partagee des modeles sera recalculee par le
	// premier modele rendu, puis reutilisee par tous -- une camera par frame.
	// SANS cet appel, la vue resterait figee a la toute premiere frame.
	NxVita::BeginRenderFrame();

	// Debut de frame. Sans cet effacement, le tampon de profondeur garde

	// Battement : prouver que le debut de frame TOURNE. Un niveau charge mais
	// entierement noir (0,0,0 releve pixel par pixel) ne peut pas venir de cet
	// effacement-ci, qui peint en bleu nuit -- sauf s'il n'a pas lieu.
	{
		static int s_f = 0;
		// [MESURE] Sonde COUTEUSE -- malloc de 96 Mo en descendant, puis
		// vglMemFree qui parcourt les listes de vitaGL : � reste � montait a
		// 1,5 ms de moyenne sur 60 images, soit un a-coup toutes les 120
		// images. Desormais sur demande (� mem 1 �), issue #18.
		if( g_vita_sonde_memoire && (( ++s_f % 120 ) == 1 ))
		{
			// Le plantage du chargement de niveau est un sprintf qui echoue
			// dans _sbrk_r : le tas newlib n'a plus de quoi s'etendre. On
			// mesure donc ce qui reste VRAIMENT, des deux cotes -- le tas
			// systeme (par un malloc d'essai) et les pools vitaGL.
			size_t plus_gros = 0;
			for( size_t t = 96u << 20; t >= 64u << 10; t >>= 1 )
			{
				void *p = malloc( t );
				if( p ) { free( p ); plus_gros = t; break; }
			}
			VLOG( "GFX", "frame %d | tas systeme : plus gros bloc %u Ko | "
			             "vitaGL RAM libre %u Ko | VRAM libre %u Ko | phycont libre %u Ko",
			      s_f, (unsigned)( plus_gros >> 10 ),
			      (unsigned)( vglMemFree( VGL_MEM_RAM ) >> 10 ),
			      (unsigned)( vglMemFree( VGL_MEM_VRAM ) >> 10 ),
			      (unsigned)( vglMemFree( (vglMemType)2 ) >> 10 ));	// VGL_MEM_SLOW, renomme VGL_MEM_PHYCONT dans vitaGL le 2026-08-11 (meme index ; PR publique #10, rreha) : compile avec les deux
		}
	}
	// celui de la frame precedente et plus rien ne se dessine correctement.
	// VIOLET FRANC pour le diagnostic -- mais plus par defaut depuis #18 :
	// XBox laisse voir sa couleur d'effacement (gris-bleu 0x506070) sous les
	// translucides du parc plage. Voir plus bas.
	//
	// Le bleu nuit (0,0,38) qui etait ici est INDISCERNABLE du ciel nocturne de
	// New Jersey. Des heures d'enquete ont pris le tampon efface pour du ciel
	// vu au travers : � les arches laissent voir la skybox �, � le sol du
	// bassin affiche la couleur du ciel �. Mesure a l'appui -- dominante
	// (0,0,38) -- alors que c'etait cette ligne.
	//
	// Une couleur d'effacement doit etre IMPOSSIBLE a confondre avec le rendu.
	//
	// Rampe gamma (#45) : l'image est rendue dans la texture de p_gamma.cpp,
	// qui doit donc etre la cible AVANT cet effacement.
	//
	// Carte de l'ombre portee AVANT : vitaGL ne garde pas la profondeur d'une
	// scene GXM a l'autre, la carte doit donc avoir sa propre scene, fermee
	// avant que la scene principale s'ouvre (p_ombre.cpp, "omb 0/1/2").
	Nx::VitaBudgetEclairageImage();
	VitaLibererDifferes();		// #48 : destructions differees, avant tout dessin
	// #48 : des modeles deja dessines dans cette image AVANT l'effacement ?
	{
		const int n = VitaDessinsEnAttente();
		static int s_dit = 0;
		if( n && ( s_dit < 300 ))
		{
			++s_dit;
			VLOG( "CHUTE", "%d modeles dessines AVANT l'effacement de l'image : perdus", n );
		}
	}
	NxVita::OmbreCarteRendu();
	ACC( ACC_OMBRE );
	NxVita::GammaImageDebut();
	// Issue #18 (parc a theme plage du Create-a-Park) : la couleur
	// d'effacement FAIT PARTIE de l'image d'origine. [SOURCE] XBox/p_nx.cpp:202
	// efface a EngineGlobals.clear_color = 0x00506070 (XBox/NX/nx_init.cpp:138,
	// jamais modifiee ailleurs ; DX9/NX/nx_init.cpp:180 idem). Le theme 2
	// (sk5ed3) a un ciel en demi-dome qui ne descend pas sous y = 5
	// (sk5ed3_sky.scn.xbx, rayon ~470 : bord a ~0,6 degre au-dessus de
	// l'horizon) et une mer TRANSLUCIDE jusqu'a l'horizon (materiau 7dad8472,
	// BLEND, alpha de sommets 51..90/128, soit 40 a 70 %) sans rien d'opaque
	// dessous, plus un anneau de brume translucide (1d9e8ab2, r ~59000).
	// Sous le bord du dome, XBox melange donc mer et brume avec ce gris-bleu ;
	// nous les melangions avec le violet de diagnostic.
	// Violet garde pour le diagnostic : rendu par identifiant (� id 1 �) ou
	// g_vita_fond_violet (p_world_render.h).
	if( NxVita::g_vita_fond_violet || NxVita::g_vita_id_debug )
		glClearColor( 0.45f, 0.05f, 0.65f, 1.0f );
	else
		glClearColor( 80.0f / 255.0f, 96.0f / 255.0f, 112.0f / 255.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	g_vita_image_ouverte = true;	// #48
	vita_glspy_ouverte = 1; vita_glspy_phase = 1;
	ACC( ACC_PRE );
}

void	CEngine::s_plat_post_render()
{
	vita_glspy_phase = 3;
	ACC( ACC_POST0 );
	VITA_STUB();

	// Presentation. C'etait un stub vide : le moteur dessinait sans que rien
	// n'atteigne jamais l'ecran, hors ecran de chargement (qui a son propre
	// vglSwapBuffers dans CLoadScreen::s_plat_display).
	// Le 2D par-dessus le monde. Sprites et textes ENTRELACES par priorite,
	// comme la liste unique SDraw2D de XBox (NX/sprite.cpp:177) : dessiner
	// tous les textes apres tous les sprites mettait le chrono du HUD
	// par-dessus le voile des menus en jeu (#54). A priorite egale, le texte
	// reste au-dessus du sprite (ordre d'avant).
	{
		static float s_pri[256];
		const SceUInt64 t2d = sceKernelGetProcessTimeWide();
		const int n = NxVita::TextePriorites( s_pri, 256 );
		float bas = -FLT_MAX;
		for( int i = 0; i < n; ++i )
		{
			NxVita::RenderSprites2D( bas, s_pri[i] );
			NxVita::RenderText2D( s_pri[i] );
			bas = s_pri[i];
		}
		NxVita::RenderSprites2D( bas, FLT_MAX );

		// Issue #17 (menu View Stats) : cout CPU de la tranche 2D, en moyenne
		// sur 120 images, avec le nombre de tranches de priorite, de glyphes
		// et d'appels de dessin du texte. Le [ACC] ne la ventile que pour les
		// images de plus de 50 ms.
		static SceUInt64 s_2d_us = 0, s_2d_max = 0;
		static int s_2d_img = 0, s_2d_pri = 0, s_2d_gly = 0, s_2d_app = 0;
		const SceUInt64 d2d = sceKernelGetProcessTimeWide() - t2d;
		s_2d_us += d2d;
		if( d2d > s_2d_max ) s_2d_max = d2d;
		s_2d_pri += n;
		s_2d_gly += NxVita::g_vita_2d_glyphes;
		s_2d_app += NxVita::g_vita_2d_appels_txt;
		NxVita::g_vita_2d_glyphes = NxVita::g_vita_2d_appels_txt = 0;
		if( ++s_2d_img == 120 )
		{
			VLOG( "2D", "bilan 120 images : 2d %d.%d ms/image (max %d.%d), %d priorites, %d glyphes, %d appels texte (xgl %d)",
			      (int)( s_2d_us / 120000 ), (int)(( s_2d_us / 12000 ) % 10 ),
			      (int)( s_2d_max / 1000 ), (int)(( s_2d_max / 100 ) % 10 ),
			      s_2d_pri / 120, s_2d_gly / 120, s_2d_app / 120, NxVita::g_vita_lots_glyphes );
			s_2d_us = s_2d_max = 0;
			s_2d_img = s_2d_pri = s_2d_gly = s_2d_app = 0;
		}
	}
	ACC( ACC_2D );

	// Affichage de debogage PAR-DESSUS tout le reste, juste avant de
	// presenter. Le temps de frame se mesure ici : c'est le seul endroit
	// traverse exactement une fois par image.
	{
		static SceUInt64 s_last = 0;
		static float     s_ms   = 0.0f;
		const SceUInt64 now = sceKernelGetProcessTimeWide();
		if( s_last != 0 )
		{
			// Moyenne glissante : un compteur qui saute a chaque image est
			// illisible a l'ecran.
			const float ms = (float)( now - s_last ) / 1000.0f;
			s_ms = ( s_ms == 0.0f ) ? ms : ( s_ms * 0.9f + ms * 0.1f );
		}
		s_last = now;
		NxVita::DrawDebugHud( s_ms, 0, 0 );
	}

	// L'ecran de chargement se repeint a CHAQUE frame, juste avant la
	// presentation : il doit couvrir ce que le moteur vient de rendre, pas
	// l'inverse. Ne fait rien si aucun chargement n'est en cours.
	VitaDessineEcranChargementSiActif();

	// Mesure periodique du temps d'image. Sans elle, � ca rame � n'est qu'une
	// impression : on ne peut ni comparer deux versions, ni savoir si une
	// modification a coute ou rapporte.
	{
		static SceUInt64 s_dernier_dit = 0;
		static SceUInt64 s_precedent   = 0;
		static float     s_moy         = 0.0f;
		const SceUInt64  maintenant    = sceKernelGetProcessTimeWide();

		if( s_precedent != 0 )
		{
			const float ms = (float)( maintenant - s_precedent ) / 1000.0f;
			s_moy = ( s_moy == 0.0f ) ? ms : ( s_moy * 0.95f + ms * 0.05f );
		}
		s_precedent = maintenant;

		if(( maintenant - s_dernier_dit ) > 5000000ULL )
		{
			s_dernier_dit = maintenant;
			VLOG( "PERF", "image : %d.%d ms (%d fps)",
			      (int)s_moy, (int)(( s_moy - (int)s_moy ) * 10.0f ),
			      ( s_moy > 0.01f ) ? (int)( 1000.0f / s_moy + 0.5f ) : 0 );
		}
	}

	// Rampe gamma Xbox sur l'image FINALE, 2D et HUD compris (#45, #22) :
	// apres tout le rendu, avant les captures -- qui lisent ainsi l'image
	// corrigee, comparable a xemu -- et avant la presentation.
	NxVita::OmbreVueCarte();		// "omb 2/3" : carte en coin d'ecran
	NxVita::GammaImageFin();
	ACC( ACC_GAMMA );

	vita_dbgsrv_frame();
#ifndef THUG_RELEASE
	NxVita::MaybeGrabScreenshot();
#endif
	// Pas de mise en veille automatique pendant le jeu (cinematiques, longs
	// chargements sans toucher aux commandes). En dev, le chien de garde de
	// vita_log.c le faisait deja ; le build public n'a pas de chien de garde.
	static int s_tick_veille = 0;
	if(( ++s_tick_veille & 63 ) == 0 )
	{
		sceKernelPowerTick( SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND );
		sceKernelPowerTick( SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF );
	}
	ACC( ACC_DBG );
	vglSwapBuffers( GL_FALSE );
	ACC( ACC_SWAP );
	acc_bilan();
	g_vita_image_ouverte = false;	// #48
	vita_glspy_ouverte = 0;
	vita_glspy_bilan();
}

void	CEngine::s_plat_render_world()
{
	vita_glspy_phase = 2;
	ACC( ACC_MONDE0 );
	VITA_STUB();
	NxVita::RenderWorld();
	NxVita::VitaMondeEtape( NxVita::MW_PART_A );		// issue #69 : decoupe du poste monde

	// Particules parametriques, apres les translucides du monde et avant les
	// translucides des modeles : XBox/p_nx.cpp:396-399 (render_particles puis
	// UpdateParticles/RenderParticles, profondeur non ecrite), avant
	// NxXbox::render_instances( ... POST_WORLD_SEMITRANSPARENT ). La mise a
	// jour est deja faite par CEngine::sRenderWorld (nx.cpp:269).
	// Anciennes particules (CParticle : sang, etincelles, eclaboussures)
	// d'abord : XBox/p_nx.cpp:393 appelle render_particles() avant les
	// nouvelles. Toujours appele (logique d'emission), dessin si "prs 1".
	NxVita::RenderParticulesAnciennes();
	NxVita::VitaMondeEtape( NxVita::MW_PART_N );
	static_cast< CVitaNewParticleManager * >( mp_particle_manager )->RenderVita();
	NxVita::VitaMondeEtape( NxVita::MW_REPORTES );

	// LE DECOR EST POSE : on dessine maintenant les morceaux translucides des
	// modeles, mis de cote pendant la phase logique. C'est l'ordre du moteur
	// d'origine (etapes 10 et 14, NOTES/pipeline-rendu-xbox.md) et il supprime
	// la cause au lieu de la compenser : un morceau translucide n'a plus
	// besoin d'ecrire la profondeur pour ne pas etre recouvert, donc il ne
	// masque plus le decor la ou il est transparent.
	VitaDessinerModelesReportes();
	NxVita::VitaMondeFin();
	ACC( ACC_MONDE );
}

void	CEngine::s_plat_set_screen_blur(uint32 amount)
{
	VITA_STUB();
}

void	CEngine::s_plat_set_letterbox(bool letterbox)
{
	VITA_STUB();
}

void	CEngine::s_plat_set_color_buffer_clear(bool clear)
{
	VITA_STUB();
}

CScene*	CEngine::s_plat_load_scene(const char* p_name, CTexDict* p_tex_dict, bool add_super_sectors, bool is_sky, bool is_dictionary)
{
	VITA_STUB();
	bool p_scene_monde = false;

	// Scene VIDE mais VALIDE. L'appelant (nx.cpp:309) enchaine sans verifier
	// sur loaded_scene->SetID(), donc NULL le tuait net.
	//
	// AUCUNE geometrie n'est lue ici : c'est precisement le travail qui reste
	// a faire au palier 3 -- decoder le format de scene Xbox et remplir des
	// buffers VitaGL. En attendant, le moteur peut aller jusqu'a sa boucle de
	// frames, ce qui est deja ce qu'on veut prouver.
	// Decodage reel du format Xbox. On ne garde encore que positions et
	// indices -- le critere du palier 3 est de la geometrie NON TEXTUREE a
	// l'ecran, pas un rendu complet.
	{
		NxVita::SVitaSceneGeom geom;
		bool pose = false;
		if( NxVita::LoadSceneGeometry( p_name, &geom ))
		{
			NxVita::AddSceneToWorld( &geom, p_tex_dict, is_sky, is_dictionary );
			pose = true;
		}
		else
			NxVita::FreeSceneGeometry( &geom );
		p_scene_monde = pose;
	}

	CScene *p_scene = new CVitaScene;
	if( p_scene_monde )
		NxVita::MondeScenePosee( p_scene );
	p_scene->SetInSuperSectors( add_super_sectors );
	p_scene->SetIsSky( is_sky );

	// PEUPLER LA SCENE EN SECTEURS.
	//
	// Sans cela la scene est une coquille : les scripts qui cherchent un
	// secteur par checksum pour l'eteindre (cfuncs.cpp) ne trouvent rien, et
	// les objets de mission -- barrieres de chantier, rampes de defi --
	// restent dessines alors que le moteur les considere absents. Leur
	// collision n'etant pas installee, on les traverse.
	//
	// Un secteur par checksum distinct, chacun avec son geom : c'est le geom
	// qui memorise l'etat actif (CSector::SetActive le lui transmet), et le
	// rendu du decor consulte cet etat par maillage.
	// Le CIEL aussi (#63) : SetSceneColor teinte la scene du ciel avec la
	// couleur sky (cfuncs.cpp:4338, set_all_colors sur sGetSkyScene) -- sans
	// secteurs, le ciel restait en plein jour la nuit.
	//
	// Issue #65 : SES secteurs seulement (ceux de la scene que l'on vient
	// d'ajouter), avec la boite du fichier. Avant, la scene prenait tous les
	// secteurs du monde : la coquille de l'editeur de parc, chargee apres la
	// bibliotheque de pieces, se les appropriait et les rebranchait sur ses
	// propres geoms.
	//
	// SCENE DICTIONNAIRE (LoadScene is_dictionary, Levels.q : bibliotheque de
	// pieces de l'editeur de parc, toutes posees autour de l'origine) : jamais
	// dessinee -- [SOURCE] XBox/NX/render.cpp:2474. Ses secteurs existent
	// (clonage, mesure des pieces) mais ses maillages sont relies a un etat
	// eteint fixe, que SetActive/SetVisibility ne touchent pas. Ses clones
	// sont dessines par dessiner_instances, qui ne consulte pas cet etat.
	{
		static unsigned int s_checksums[4096];
		static float        s_boites[4096 * 6];
		static const bool   s_dico_eteint = false;
		const int scene_no = p_scene_monde ? NxVita::SceneMondeCourante() : -1;
		const int n = p_scene_monde
		              ? NxVita::ListerSecteurs( s_checksums, 4096, is_sky, scene_no, s_boites )
		              : 0;
		int lies = 0;
		for( int i = 0; i < n; ++i )
		{
			CSector *p_sector = p_scene->CreateSector();
			if( !p_sector )
				continue;
			p_sector->SetChecksum( s_checksums[i] );
			CVitaGeom *p_geom = new CVitaGeom;
			p_geom->VitaSetSecteur( s_checksums[i] );		// clonage (#29)
			p_geom->VitaSetOrigine( scene_no, &s_boites[i * 6] );	// #65
			p_sector->SetGeom( p_geom );
			p_scene->AddSector( p_sector );
			lies += NxVita::LierSecteur( s_checksums[i],
			                             is_dictionary ? &s_dico_eteint : p_geom->VitaActivePtr(),
			                             scene_no );
		}
		VLOG( "SCN", "secteurs : %d enregistres, %d maillages relies%s", n, lies,
		      is_dictionary ? " (DICTIONNAIRE : jamais dessine a sa place, #65)" : "" );
	}

	return p_scene;
}

bool	CEngine::s_plat_add_scene(CScene *p_scene, const char *p_filename)
{
	// CE STUB COUTAIT LA GEOMETRIE DES NIVEAUX.
	//
	// Un niveau n'arrive pas par sLoadScene mais par sAddScene (nx.cpp:434) :
	// le moteur cree d'abord la scene, puis lui AJOUTE un fichier. En ne
	// faisant rien ici, on enregistrait bien « scene NJ en place 1/4 » -- d'ou
	// l'illusion que tout allait bien -- mais aucun triangle n'etait lu. Le
	// monde restait aux 10 maillages du menu et du ciel, et le joueur se
	// retrouvait dans le vide bleu.
	//
	// Le dictionnaire de textures n'est pas passe en parametre. Il porte le
	// meme chemin que la scene, extension « .tex » (nx.cpp:405) : on le
	// reconstruit plutot que de dessiner un niveau sans textures.
	char tex_name[160];
	tex_name[0] = 0;
	if( p_filename )
	{
		strncpy( tex_name, p_filename, sizeof( tex_name ) - 1 );
		tex_name[sizeof( tex_name ) - 1] = 0;
		// « levels\NJ\NJ.scn.xbx » -> « levels\NJ\NJ.tex »
		char *p_scn = strstr( tex_name, ".scn" );
		if( p_scn )
			strcpy( p_scn, ".tex" );
	}

	CTexDict *p_tex_dict = NULL;
	if( tex_name[0] )
		p_tex_dict = CTexDictManager::sGetTextureDictionary(
			Script::GenerateCRC( tex_name ));

	VLOG( "SCN", "ajout a la scene : '%s' (textures '%s' %s)",
	      p_filename ? p_filename : "?", tex_name,
	      p_tex_dict ? "trouvees" : "ABSENTES" );

	NxVita::SVitaSceneGeom geom;
	if( NxVita::LoadSceneGeometry( p_filename, &geom ))
	{
		NxVita::AddSceneToWorld( &geom, p_tex_dict );
		NxVita::MondeScenePosee( p_scene );
		return true;
	}

	NxVita::FreeSceneGeometry( &geom );
	return false;
}

bool	CEngine::s_plat_unload_scene(CScene *p_scene)
{
	VITA_STUB();

	// Une scene se decharge : un niveau va etre charge. C'est le moment ou la
	// memoire redevient necessaire, donc celui de liberer les .pre gardes en
	// sursis (voir PRE.cpp). Sans cela, les 10 Mo de skaterparts manqueraient
	// au chargement -- c'est exactement ce qui cassait les niveaux quand le
	// cache etait permanent.
	File::PreMgr::FlushDeferred();

	// Derniere scene du monde partie : geometrie et textures de niveau
	// liberees (issue #32, voir MondeSceneRetiree).
	NxVita::MondeSceneRetiree( p_scene );

	// Le retour n'est PAS cosmetique : sUnloadScene (nx.cpp:440) ne retire la
	// scene de sp_loaded_scenes QUE si on rend true. Avec false, le tableau --
	// qui ne compte que MAX_LOADED_SCENES = 4 places -- ne se vidait jamais.
	//
	// Chaine complete, mesuree : mainmenu, CAS_bedroom_Sky, cas_bedroom et
	// NJ_Sky prenaient les quatre places ; � NJ � arrivait cinquieme et n'etait
	// pas enregistree. L'assert cense le signaler est compile a vide chez nous,
	// donc sGetScene("NJ") rendait NULL en silence, et ScriptLoadCollision
	// appelait LoadCollision sur ce NULL. read_collision ecrit alors dans
	// this->m_coll_filename, c'est-a-dire a l'adresse 0xd8 -- exactement le R0
	// du psp2core, et le Data abort tombait DANS sprintf.
	//
	// Un � return false � de stub, six maillons plus loin, devient un plantage
	// dans la libc. C'est le prix des valeurs de retour posees au hasard.
	//
	// La geometrie GPU est rendue par MondeSceneRetiree, plus haut, quand la
	// derniere scene du monde s'en va (issue #32 : elle ne l'etait jamais).
	return ( p_scene != NULL );
}

CSprite *	CEngine::s_plat_create_sprite(CWindow2D *p_window)
{
	VITA_STUB();
	// CVitaSprite s'inscrit tout seul dans la liste de dessin 2D.
	return new CVitaSprite( p_window );
}

bool	CEngine::s_plat_destroy_sprite(CSprite *p_sprite)
{
	VITA_STUB();
	if( p_sprite )
	{
		delete p_sprite;
		return true;
	}
	return false;
}

CTextured3dPoly *	CEngine::s_plat_create_textured_3d_poly()
{
	VITA_STUB();
	// Objet vide mais valide : NULL ferait mourir l'appelant sur place.
	return new CTextured3dPoly();
}

bool	CEngine::s_plat_destroy_textured_3d_poly(CTextured3dPoly *p_poly)
{
	VITA_STUB();
	return false;
}

// Ombre portee "detailed" du skater (issues #45 V2 / #4) : p_ombre.cpp.
// [SOURCE] XBox/p_nx.cpp:950-1010 et Gfx/shadow.cpp:39-125.
CTexture *	CEngine::s_plat_create_render_target_texture(int width, int height, int depth, int z_depth)
{
	(void)depth; (void)z_depth;
	return NxVita::OmbreCreerCible( width, height );
}

void	CEngine::s_plat_project_texture_into_scene(Nx::CTexture *p_texture, Nx::CModel *p_model, Nx::CScene *p_scene)
{
	// Comme XBox : la scene n'est pas retenue, l'ombre est recue par toutes
	// les scenes non-ciel.
	(void)p_scene;
	NxVita::OmbreProjeter( p_texture, p_model );
}

void	CEngine::s_plat_set_projection_texture_camera(Nx::CTexture *p_texture, Gfx::Camera *p_camera)
{
	NxVita::OmbreCamera( p_texture, p_camera );
}

void	CEngine::s_plat_stop_projection_texture(Nx::CTexture *p_texture)
{
	NxVita::OmbreArreter( p_texture );
}

void	CEngine::s_plat_add_occlusion_poly(uint32 num_verts, Mth::Vector *p_vert_array, uint32 checksum)
{
	// Le moteur n'envoie que des quads (CollTriData.cpp apparie les triangles
	// de collision deux a deux avant d'appeler ici). On ne sait traiter que
	// cela : un triangle isole ne definit pas le volume attendu.
	if(( num_verts != 4 ) || !p_vert_array )
		return;

	float v[12];
	for( int i = 0; i < 4; ++i )
	{
		v[( i * 3 ) + 0] = p_vert_array[i][X];
		v[( i * 3 ) + 1] = p_vert_array[i][Y];
		v[( i * 3 ) + 2] = p_vert_array[i][Z];
	}
	NxVita::OcclusionAjouter( v, checksum );
}

void	CEngine::s_plat_enable_occlusion_poly(uint32 checksum, bool enable)
{
	NxVita::OcclusionActiver( checksum, enable );
}

void	CEngine::s_plat_remove_all_occlusion_polys(void)
{
	// Indispensable au changement de niveau : les quads sont en coordonnees
	// monde et leurs checksums appartiennent a la scene qui s'en va.
	NxVita::OcclusionVider();
}

const char *	CEngine::s_plat_get_platform_extension()
{
	// PAS un stub : rendre NULL produisait des chemins du genre
	// « skeletons/anl_horse.ske.(null) » — le moteur concatene cette
	// extension a beaucoup de noms de fichiers (NxMesh.cpp, NxScene.cpp...).
	//
	// Nos assets viennent d'une ISO Xbox, ou les fichiers portent « .xbx »
	// (NS_Head_Zac.img.xbx).
	//
	// SANS le point : les formats appelants l'ajoutent deja
	// (NxScene.cpp : "levels\\%s\\%s%s.col.%s"). Le rendre avec un point
	// produisait « anl_horse.ske..xbx ».
	return "xbx";
}

CModel*	CEngine::s_plat_init_model()
{
	VITA_STUB();
	return new CVitaModel();
}

bool	CEngine::s_plat_uninit_model(CModel* pModel)
{
	// [SOURCE] XBox/p_nx.cpp : delete. Reste un stub jusqu'au 2026-10-04 :
	// AUCUN modele, geom ni anim rapide n'etait detruit -- pietons, vehicules
	// et objets de chaque niveau restaient en memoire avec leurs maillages
	// (~5 Mo par niveau, tas plein et plantage apres ~9 niveaux, #47).
	delete pModel;
	return true;
}

CGeom*	CEngine::s_plat_init_geom()
{
	VITA_STUB();
	return new CVitaGeom();
}

bool	CEngine::s_plat_uninit_geom(CGeom* pGeom)
{
	delete pGeom;		// voir s_plat_uninit_model
	return true;
}

CQuickAnim*	CEngine::s_plat_init_quick_anim()
{
	VITA_STUB();
	return new CQuickAnim();
}

void	CEngine::s_plat_uninit_quick_anim(CQuickAnim* pQuickAnim)
{
	delete pQuickAnim;	// voir s_plat_uninit_model
}

CMesh*	CEngine::s_plat_load_mesh(const char* pMeshFileName, Nx::CTexDict* pTexDict, uint32 texDictOffset, bool isSkin, bool doShadowVolume)
{
	VITA_STUB();

	// Un maillage EST une scene : le backend DX9 (p_nx.cpp:999) appelle
	// simplement s_plat_load_scene sur le .mdl. On reutilise le decodeur.
	CVitaMesh *p_mesh = new CVitaMesh();
	p_mesh->Build( pMeshFileName, pTexDict );
	return p_mesh;
}

CMesh*	CEngine::s_plat_load_mesh(uint32 id, uint32* pModelData, int modelDataSize, uint8* pCASData, Nx::CTexDict* pTexDict, uint32 texDictOffset, bool isSkin, bool doShadowVolume)
{
	VITA_STUB();

	// Le chemin Create-A-Skater : la piece arrive EN RAM (skaterparts.pre est
	// deja charge), au meme format .skin.xbx que sur disque, avec ses donnees
	// CAS a part. La version precedente rendait un CVitaMesh VIDE -- pose pour
	// arreter un plantage, jamais implementee : le skater custom perdait
	// toutes les pieces servies par ce chemin (jambes, pantalon, mains...).
	char label[32];
	sprintf( label, "cas:%08x", (unsigned)id );

	CVitaMesh *p_mesh = new CVitaMesh();
	p_mesh->BuildFromMemory( pModelData, modelDataSize, pCASData, pTexDict, label );
	return p_mesh;
}

bool	CEngine::s_plat_unload_mesh(CMesh* pMesh)
{
	// [SOURCE] XBox/p_nx.cpp et DX9/p_nx.cpp : delete pMesh. Le stub ne
	// detruisait rien : chaque changement de niveau laissait ~58 maillages de
	// modele (pietons, vehicules, objets) avec leurs tampons GL, ~5,8 Mo de
	// VRAM par aller-retour -- jusqu'au plantage GPU (issue #32).
	// L'appelant (CModel::ClearGeoms, CSkinAsset::Unload) a deja retire les
	// geoms qui le referencaient -- mais pas forcement leurs CLONES, qui le
	// partagent : CVitaMesh::Decharger attend le dernier.
	static_cast< CVitaMesh * >( pMesh )->Decharger();
	return true;
}

void	CEngine::s_plat_set_mesh_scaling_parameters(SMeshScalingParameters* pParams)
{
	VITA_STUB();
}

// "vis 0/1" : test de visibilite REEL pour le moteur (issue #18).
//
// [SOURCE] XBox/p_nx.cpp:1059 -> NxXbox::IsVisible (XBox/NX/render.cpp:1845) :
// sphere contre le tronc de la DERNIERE camera posee. Appelants portables :
// CSuspendComponent::CheckModelActive (SuspendComponent.cpp:423) met le
// modele inactif hors champ, d'ou CAnimationComponent::ShouldAnimate faux
// (animationcomponent.cpp:2005 : pas d'update_skeleton), pas de
// CModel::Render (modelcomponent.cpp:686) ni d'ombre (shadowcomponent.cpp:205)
// pour les personnages hors champ. Le stub rendait vrai : tous les squelettes
// a portee etaient recalcules chaque image, meme derriere la camera.
//
// Memes plans que le culling des personnages du backend (SphereVisible,
// p_world_render.cpp, tronc de RefreshViewFromCamera). Rayon porte a 150 au
// moins : le CModel par defaut rend une sphere de 48 pour TOUT modele skinne
// (NxModel.cpp:151, voitures comprises) ; 150 est le rayon deja utilise par
// p_NxModel.cpp pour ne plus dessiner ces modeles -- on ne rend donc pas
// inactif un modele que le backend aurait dessine.
// Defaut 0 tant que l'A/B et la validation a l'ecran ne sont pas faits.
bool g_vita_vis = false;

bool	CEngine::s_plat_is_visible(Mth::Vector& center, float radius)
{
	VITA_STUB();

	if( g_vita_vis )
		return NxVita::SphereVisible( center[X], center[Y], center[Z],
		                              ( radius < 150.0f ) ? 150.0f : radius );

	// Test de visibilite (sphere contre tronc de vision). Tant qu'il n'est pas
	// implemente, la seule valeur SURE est true : un culling trop permissif
	// coute des triangles, un culling trop agressif fait disparaitre des objets
	// et bloque toute logique qui attend qu'une chose soit visible.
	//
	// La version precedente rendait false -- � rien n'est jamais visible �.
	return true;
}

void	CEngine::s_plat_set_max_multipass_distance(float dist)
{
	VITA_STUB();
}

void	CEngine::s_plat_finish_rendering()
{
	// [SOURCE] XBox/p_nx.cpp : BlockUntilIdle(). Le moteur l'appelle avant de
	// decharger une scene, une texture, une piece de parc -- pour ne pas
	// retirer au GPU ce qu'il dessine encore.
	//
	// Chez nous c'est vital (#32) : vitaGL libere les donnees d'une texture ou
	// d'un tampon SUR-LE-CHAMP s'il ne les a pas vus servir depuis 4 images
	// (gpu_free_texture_data, last_frame). Or le decor et les personnages
	// passent par GXM direct, que vitaGL ne voit pas : pour lui, rien ne sert
	// jamais. Sans cette attente, la memoire etait rendue -- et reecrite par
	// le chargement suivant -- pendant que le GPU finissait l'image : plantage
	// GPU au changement de niveau. Hors de toute scene ici (phase logique).
	glFinish();
}

int	CEngine::s_plat_get_num_soundtracks()
{
	VITA_STUB();
	return 0;
}

const char*	CEngine::s_plat_get_soundtrack_name(int soundtrack_number)
{
	VITA_STUB();
	return NULL;
}

// --- Premier morceau RÉEL du backend -------------------------------------
//
// Pas un stub : rendre NULL ici bloquait tout le démarrage du moteur
// graphique. sSetScreenMode() récupère le viewport actif et appelle
// GetCamera() dessus sans vérifier le pointeur (le Dbg_MsgAssert qui le
// protégeait est compilé à vide chez nous) — donc NULL = déréférencement.
//
// CViewport a un constructeur public prenant exactement ces arguments : il
// n'y a rien de spécifique à la plateforme à faire ici, juste à le
// construire. Les backends console font pareil.
CViewport *CViewportManager::s_plat_create_viewport( const Mth::Rect *rect, Gfx::Camera *cam )
{
	VITA_STUB();
	return new CViewport( rect, cam );
}

} // namespace Nx


namespace NxVita
{
// Couleur d'effacement de diagnostic (violet franc) au lieu de celle de XBox
// (issue #18). Faux par defaut : XBox montre sa couleur d'effacement a
// l'ecran, sous les translucides qui n'ont rien derriere eux.
bool g_vita_fond_violet = false;

// � fps 30|60 � (p_siodev.cpp). vitaGL attend N vsync par image
// (gxm.c:265, sceDisplayWaitVblankStartMulti). A 30, chaque image dure
// exactement 2 vsync : un rythme regulier la ou 60 n'est pas tenu (22 ms
// mesures en roulant dans New Jersey) et ou les images alternent 1 et 2 vsync.
// Cadence courante, lue par le menu VITA OPTIONS et ecrite dans controls.txt
// (cle "framerate", Sys/SIO/Vita/p_siodev.cpp). 60 = reglage de vitaGL au
// demarrage (vsync_interval = 1, vgl.c).
int g_vita_cadence = 60;

void FixerCadence( int images_par_seconde )
{
	g_vita_cadence = ( images_par_seconde == 30 ) ? 30 : 60;
	eglSwapInterval( 0, ( images_par_seconde == 30 ) ? 2 : 1 );
	VLOG( "GFX", "cadence : %d images/s", ( images_par_seconde == 30 ) ? 30 : 60 );
}
}
