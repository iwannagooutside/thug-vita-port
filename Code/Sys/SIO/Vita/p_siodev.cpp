/*****************************************************************************
**  THUG-Vita â€” couche systÃ¨me                                              **
**  Code/Sys/SIO/Vita/p_siodev.cpp                                          **
**                                                                          **
**  Manette, via sceCtrl.                                                   **
**                                                                          **
**  Le moteur ne connait qu'un seul format d'entree : le tampon BRUT d'une  **
**  DualShock 2. Chaque backend traduit son materiel vers ce format ; on    **
**  fait pareil ici. Le contrat, releve sur le backend Win32                **
**  (Sys/SIO/Win32/p_siodev.cpp:197) :                                      **
**                                                                          **
**    [0]      drapeau de validite (0 = valide)                             **
**    [1]      identifiant DUALSHOCK2 + longueur                            **
**    [2]      boutons, ACTIF A ZERO : bit0 SELECT, 1 L3, 2 R3, 3 START,    **
**             4 HAUT, 5 DROITE, 6 BAS, 7 GAUCHE                            **
**    [3]      boutons, ACTIF A ZERO : bit0 L2, 1 R2, 2 L1, 3 R1,           **
**             4 TRIANGLE, 5 ROND, 6 CROIX, 7 CARRE                         **
**    [4][5]   stick droit X, Y   (0..255, 128 = centre)                    **
**    [6][7]   stick gauche X, Y                                            **
**    [8..19]  pressions analogiques (0 chez nous)                          **
**                                                                          **
**  Â« Actif a zero Â» : on part de 0xFF et on efface le bit du bouton        **
**  enfonce. Se tromper de sens donne un jeu ou toutes les touches sont      **
**  appuyees en permanence -- symptome deroutant s'il en est.                **
**                                                                          **
**  La Vita n'a ni L2/R2 ni L3/R3. L2/R2 portent pourtant des figures      **
**  (scripts skater : manualtricks, groundtricks, tricks, grindscripts) :   **
**  ils passent par le pave tactile ARRIERE, moitie gauche = L2, moitie     **
**  droite = R2 (issue #30). L3/R3 ne sont lus par aucun script de jeu.     **
*****************************************************************************/

#include <core/defines.h>
#include <sys/sioman.h>
#include <sys/siodev.h>
#include <sys/mem/memman.h>

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include <string.h>
#include <stdio.h>

#include "vita_log.h"
namespace NxVita { extern float g_vita_plan_loin; extern int g_vita_cam_modeles; extern float g_vita_ombre_douce; extern int g_vita_ombre_decoupe; extern int g_vita_ombre_region; }
#include "vita_dbgsrv.h"
namespace Sfx { void VitaListeVoix( void ); void VitaEnregistreMix( int ); }
namespace NxVita { extern bool g_vita_fond_violet; }	// "violet" (#18)	// "voix" (#57)
extern int g_vita_trace_trk;	// trickcomponent.cpp, "trk" (#6)

#include <sk/modules/skate/skate.h>
#include <sk/objects/skatercareer.h>
#include <sk/objects/skater.h>
#include <sk/modules/skate/goalmanager.h>
#include <sk/modules/skate/goal.h>
#include <gel/scripting/script.h>
#include <gel/scripting/checksum.h>
#include <gel/scripting/symboltable.h>
#include "../../../Gfx/Vita/p_screenshot.h"
#include <stdlib.h>
#include <stdint.h>
extern unsigned long long g_vita_cine_vue;	// moviecam.cpp (#51)

// Declare ici plutot qu'en incluant p_world_render.h : ce header tire tout le
// chargeur de scenes et les types de textures, dont ce fichier n'a que faire.
namespace Obj { extern bool g_vita_prof_composants; }
namespace Nx { extern bool g_vita_dessin_modeles; extern bool g_vita_sonde_memoire; }
namespace NxVita { extern bool g_vita_gxm_peau; extern bool g_vita_cache_plages; extern bool g_vita_gxm_seuls; extern bool g_vita_gxm_transp; extern bool g_vita_cand_listes; extern bool g_vita_listes_mt; extern bool g_vita_seuil_malin; extern bool g_vita_env; extern bool g_vita_listes_gxm; extern bool g_vita_lots_mt; extern bool g_vita_cull_plages; extern unsigned int g_vita_gen_pre; extern bool g_vita_gxm_pre; void VitaAppliquerMip( int ); extern bool g_vita_gxm_direct; extern bool g_vita_cull_compact; void VitaTexSum( unsigned int ); extern bool g_vita_sprite_dump; extern bool g_vita_tri_lots; extern bool g_vita_lots_transp; extern bool g_vita_lots_multi; void FixerCadence( int ); extern int g_vita_shader_decor; extern bool g_vita_dxt_align; extern float g_vita_world_zoom; extern bool g_vita_cull; extern bool g_vita_sky; extern bool g_vita_hud; extern bool g_vita_mat_debug; extern bool g_vita_force_opaque; extern bool g_vita_transp_flag; extern bool g_vita_zwrite_transp; extern bool g_vita_alpha_test; extern bool g_vita_multitex; extern bool g_vita_id_debug; extern bool g_vita_sky_last; extern bool g_vita_ignore_vertex_alpha; extern bool g_vita_tri_profondeur; extern bool g_vita_prepasse; extern bool g_vita_lots; void VitaDumpMeshInfo( int ); void VitaListePlan( float ); extern bool g_vita_occlusion; }
namespace Nx { extern int g_vita_mbf; extern int g_vita_zw_peau; }
namespace NxVita { extern bool g_vita_alpha_fixe_une; }	// issue #72
namespace NxVita { extern bool g_vita_env_x; }	// issue #72, passes 2-3 en reflet
namespace NxVita { extern bool g_vita_zeq; }	// test de profondeur du decor (LEQUAL)
namespace NxVita { extern int g_vita_cadence; }	// p_nx.cpp, FixerCadence ("framerate")
namespace NxVita { extern int g_vita_mx2; void JournaliserTextes( void ); }
namespace NxVita { extern int g_vita_sprites_sans_tex; }
namespace Nx { extern bool g_vita_rigide_gpu; }	// "rgp" (#67)
namespace Obj { extern int g_vita_veh_sous_pas; extern int g_vita_veh_log; }	// "vss" / "vlg" (#13, #14)
namespace Nx { extern bool g_vita_lum_instance; extern bool g_vita_lum_atomique; }	// "lpi", "lpa" (#78)
namespace Nx { extern int g_vita_rejeu; extern bool g_vita_rigide_lum; extern int g_vita_os_persist; extern int g_vita_zw_modeles; extern int g_vita_differer_lib; }
namespace NxVita { extern float g_vita_lum_k; extern int g_vita_alpha_x2; }
namespace NxVita { extern bool g_vita_peau_uc; }
namespace NxVita { extern bool g_vita_var_hash; extern bool g_vita_prof_monde; }	// "vhs", "pmd" (#69)
namespace NxVita { extern bool g_vita_zbl; extern bool g_vita_vpr; }	// "zbl", "vpr" (#69)
namespace NxVita { extern bool g_vita_bbl; }	// "bbl" (#69)
namespace Nx { extern bool g_vita_mro; }		// "mro" (#69)
namespace Nx { extern bool g_vita_gyr; extern bool g_vita_pad; }	// "gyr", "pad" (#77)
namespace NxVita { extern bool g_vita_teinte_scene; void TeinteSceneReappliquer( void ); }	// "tsc" (#63)
namespace NxVita { extern int g_vita_teinte_index; extern int g_vita_teinte_budget_us; extern bool g_vita_teinte_neon; }	// "tix" "tbu" "tvn" (#15)
namespace NxVita { extern int g_vita_lots_glyphes; }	// "xgl" (#17)
namespace Nx { extern bool g_vita_vis; }
namespace NxVita { extern bool g_vita_melange_opaques; }	// "bop" (banc #69, beige d'Hawaii)
namespace NxVita { extern int g_vita_bfc; extern bool g_vita_zbias; void VitaListeReflets( float, float ); void VitaListeUVAnimes( void ); }
namespace NxVita { extern bool g_vita_uvw; extern int g_vita_fog; bool BrouillardActif( void ); }
namespace NxVita { extern int g_vita_particules; }
namespace NxVita { extern int g_vita_prs; }
namespace NxVita { extern bool g_vita_vcw; }
namespace NxVita { extern bool g_vita_billboards; }
namespace NxVita { extern int g_vita_gamma; void SetGammaNormalized( float, float, float ); void GetGammaNormalized( float *, float *, float * ); }
namespace NxVita { extern int g_vita_ombre; extern float g_vita_ombre_pf; extern float g_vita_ombre_pu; extern int g_vita_ombre_transp; }
namespace NxVita { extern int g_vita_auto_ombre; extern float g_vita_aom_b0; extern float g_vita_aom_b1; extern float g_vita_aom_k; }
namespace Nx { extern int g_vita_pdt; }
namespace Nx { extern bool g_vita_skin_gpu; extern bool g_vita_lighting; extern bool g_vita_trace_modeles; extern bool g_vita_trace_reset; extern bool g_vita_trace_reset_inactifs; }

// Sortie de secours : appelle Skate::EndRun(), ce que l'entree « quit » du menu
// de pause est censee declencher. CONTOURNEMENT DE TEST, pas un correctif : le
// vrai defaut est que ce clic n'appelle aucune c-function (mesure a la trace
// nommee), et il reste a trouver dans la couche menu.
extern "C" void VitaForceEndRun( void );
// Rappelle OpenLevel avec le script du frontend, celui-la meme que le jeu
// emploie au demarrage. Poser le drapeau EndRun ne suffisait pas : en free
// skate il n'y a aucune « run » a terminer, donc rien ne se declenchait.
// Demande DIFFEREE : on ne peut pas relancer un niveau depuis la lecture de
// la manette, en plein milieu d'une frame. Le premier essai le faisait et
// plantait dans le chargement de la collision (sConvertTypeChecksum). La
// boucle principale consommera la demande entre deux images, la ou le jeu
// s'attend a ce genre d'operation.
extern "C" void VitaRequestFrontend( void );

// Profileur par instrumentation (vita/src/vita_prof.c). Absent du binaire si
// -DTHUG_PROFILE=OFF : les symboles existent quand meme, ils ne collectent
// simplement rien.
extern "C" void vita_prof_enable( int on );
extern "C" int  vita_prof_dump( unsigned int *p_addr, unsigned long long *p_total,
                                unsigned int *p_calls, int max );

// Definie dans Gel/Scripting/script.cpp : arme la trace des c-functions QB.
extern void VitaSetCfuncTrace( bool on );

namespace SIO
{

// K: index du dernier pad dont un bouton a ete presse. Le moteur s'en sert
// pour decider quelle manette pilote le joueur.
int gLastPadPressed = 0;


// ---------------------------------------------------------------------------
// Injection d'entrees a distance.
//
// Raison d'etre : quand personne n'est devant la console, il n'y a aucun moyen
// d'appuyer sur une touche -- et donc aucun moyen de tester ce qui se passe
// APRES le menu. On lit un petit fichier depose par FTP, on rejoue les touches
// qu'il nomme, puis on l'efface.
//
// Format, un mot par ligne, insensible a la casse :
//     HAUT BAS GAUCHE DROITE CROIX ROND CARRE TRIANGLE START SELECT L1 R1
// Chaque mot vaut un appui court suivi d'un relachement -- il FAUT relacher,
// sinon le moteur ne voit qu'un appui maintenu et non une succession. La duree
// se compte en microsecondes, pas en frames : voir INJECT_HOLD_US.
//
// Sondage toutes les 30 frames seulement : ouvrir un fichier a chaque frame
// couterait exactement ce qu'on vient de payer cher ailleurs.
// ---------------------------------------------------------------------------
#define INJECT_PATH		"ux0:data/thug/inject.txt"
// Duree en MICROSECONDES, pas en frames.
//
// La version en frames (20 tenues / 20 relachees) tenait la touche ~700 ms a
// 30 fps. Or le front-end repete une direction maintenue au-dela de 300 ms,
// puis toutes les 50 ms (FrontEnd.cpp:97). Un appui injecte valait donc SEPT
// deplacements de surbrillance, et la selection finale retombait presque a son
// point de depart par modulo -- ce qui s'est lu pendant une session entiere
// comme Ã‚Â« la touche HAUT ne fait rien Ã‚Â».
//
// On tient donc franchement sous le seuil de repetition, et on relache assez
// longtemps pour que le front-end repasse a NOT_DOWN.
//
// Le plancher en frames reste necessaire : la manette n'est echantillonnee
// qu'une fois par frame, et a 4 fps pendant un chargement une duree de 150 ms
// ne couvrirait aucune frame. C'est le compromis inverse de l'ancien -- borne
// par le temps en haut, par les frames en bas.
#define INJECT_HOLD_US		150000
#define INJECT_GAP_US		250000
#define INJECT_MIN_FRAMES	2
#define INJECT_MAX		32

static unsigned int	s_inject_seq[INJECT_MAX];
static int			s_inject_count   = 0;
static int			s_inject_index   = 0;
static SceUInt64	s_inject_start   = 0;	// debut de l'appui courant
static int			s_inject_frames  = 0;	// frames ecoulees dans la phase
static bool			s_inject_held    = false;
// Capture demandee A LA FIN de la sequence, pas a la lecture du fichier.
// Premiere version : Â« shot Â» declenchait la capture des l'analyse, donc AVANT
// que les touches ne soient rejouees -- on photographiait l'etat d'avant.
// Plusieurs observations de cette session en ont ete faussees.
static bool			s_inject_shot    = false;

// Ramene au centre exact tout ce qui est proche du centre.
#define ZONE_MORTE 24

static unsigned char zone_morte( unsigned char v )
{
	int d = (int)v - 128;
	if(( d > -ZONE_MORTE ) && ( d < ZONE_MORTE ))
		return 128;
	return v;
}


// Gachettes absentes de la Vita, rendues par le pave arriere. Bits hors de
// ceux de sceCtrl : sur la console portable, SCE_CTRL_L2 n'est qu'un alias de
// SCE_CTRL_LTRIGGER, c'est-a-dire du bouton L lui-meme.
#define VITA_PAD_L2		0x01000000
#define VITA_PAD_R2		0x02000000

// Pave arriere : moitie gauche = L2, moitie droite = R2. Un doigt pose
// n'importe ou suffit ; deux doigts donnent les deux.
static unsigned int pave_arriere( void )
{
	static bool s_init = false;
	static int  s_milieu = 960;
	if( !s_init )
	{
		s_init = true;
		sceTouchSetSamplingState( SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START );
		SceTouchPanelInfo info;
		if( sceTouchGetPanelInfo( SCE_TOUCH_PORT_BACK, &info ) >= 0 )
			s_milieu = ( info.minAaX + info.maxAaX ) / 2;
		VLOG( "PAD", "pave arriere : L2 a gauche, R2 a droite (x < %d)", s_milieu );
	}
	SceTouchData td;
	if( sceTouchPeek( SCE_TOUCH_PORT_BACK, &td, 1 ) < 1 )
		return 0;
	unsigned int bits = 0;
	for( unsigned int i = 0; i < td.reportNum; ++i )
		bits |= ( td.report[i].x < s_milieu ) ? VITA_PAD_L2 : VITA_PAD_R2;
	return bits;
}

// COINS BAS DE L'ECRAN TACTILE AVANT (#76, demande de l'humain) : les doigts
// reposent en permanence sur le pave arriere, qui portait L1/R1 (rotations) --
// le skater tournait tout seul. Coin bas gauche = L1, coin bas droit = R1, les
// deux ensemble = L1+R1, la descente de planche : SwitchControl_Trigger =
// { PressTwoAnyOrder L1 R1 400 } (airtricks.q, trickcomponent.cpp). Le pave
// arriere ne fait plus rien ("tac 0" : ancien comportement).
//
// Un coin seul n'emet qu'apres TAC_GRACE images sans l'autre : poser les deux
// doigts n'est jamais exactement simultane, et sans ce delai la descente
// commencerait par un debut de rotation. Une fois la combinaison vue, plus de
// L1/R1 tant que les deux doigts ne sont pas leves.
int g_vita_tactile = 1;
#define TAC_GRACE		3
#define TAC_COIN_X		480		// 1/4 de 1920
#define TAC_COIN_Y		760		// ~30 % du bas sur 1088
enum { TAC_L1 = 1, TAC_R1 = 2, TAC_DESCENTE = 4 };

static int ecran_coins( void )
{
	static bool s_init = false;
	static int  s_attente = 0;		// images depuis le premier coin pose
	static bool s_combo = false;
	if( !s_init )
	{
		s_init = true;
		sceTouchSetSamplingState( SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START );
		VLOG( "PAD", "ecran avant : coin bas gauche L1, bas droit R1, les deux = descente (#76)" );
	}
	SceTouchData td;
	bool g = false, d = false;
	if( sceTouchPeek( SCE_TOUCH_PORT_FRONT, &td, 1 ) >= 1 )
		for( unsigned int i = 0; i < td.reportNum; ++i )
		{
			const int x = td.report[i].x, y = td.report[i].y;	// 0..1919, 0..1087
			if( y < TAC_COIN_Y )
				continue;
			if( x < TAC_COIN_X )			g = true;
			else if( x >= 1920 - TAC_COIN_X )	d = true;
		}
	if( !g && !d )
	{
		s_attente = 0;
		s_combo = false;
		return 0;
	}
	if( g && d )
		s_combo = true;
	if( s_combo )
		return TAC_DESCENTE;
	if( s_attente < TAC_GRACE )
	{
		++s_attente;
		return 0;
	}
	return g ? TAC_L1 : TAC_R1;
}

// RACCOURCIS DE TEST DU STORY MODE (#51) sur l'ECRAN TACTILE AVANT, que le jeu
// n'utilise pas (SELECT n'arrive pas a l'application : intercepte avant).
// Doigt maintenu 1 s dans le coin haut-droit : gagner la mission en cours ;
// coin haut-gauche : etape suivante du scenario. Scripts de triche du jeu.
static void ecran_raccourcis( void )
{
	static bool s_init = false;
	static SceUInt64 s_debut = 0;
	static int s_zone = 0;
	static bool s_fait = false;
	if( !s_init )
	{
		s_init = true;
		sceTouchSetSamplingState( SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START );
	}
	SceTouchData td;
	int zone = 0;
	if(( sceTouchPeek( SCE_TOUCH_PORT_FRONT, &td, 1 ) >= 1 ) && ( td.reportNum >= 1 ))
	{
		const int x = td.report[0].x, y = td.report[0].y;	// 0..1919, 0..1087
		if( y < 250 )
			zone = ( x > 1600 ) ? 1 : ( x < 320 ) ? 2 : 0;
	}
	const SceUInt64 t = sceKernelGetProcessTimeWide();
	if( zone != s_zone )
	{
		s_zone = zone; s_debut = t; s_fait = false;
		return;
	}
	if( !zone || s_fait || ( t - s_debut < 1000000 ))
		return;
	s_fait = true;
	if( t - g_vita_cine_vue < 500000 )
	{
		VLOG( "DBG", "RACCOURCI tactile refuse : cinematique en cours" );
		return;
	}
	const char *p_scr = ( zone == 1 ) ? "cheats_menu_beat_current_goal" : "cheats_menu_advance_stage";
	Script::CStruct *p_params = new Script::CStruct;
	Script::SpawnScript( Script::GenerateCRC( p_scr ), p_params, NO_NAME, NULL, -1, 0, false, true, true );
	delete p_params;
	VLOG( "DBG", "RACCOURCI tactile : %s", p_scr );
}

int g_vita_inv_gachettes = 1;	// #59/#60 : L/R = L2/R2, pave arriere = L1/R1

// --- REGLAGES MANETTE DU PORT (#31, #2, #21) --------------------------------
//
// Le jeu d'origine n'a AUCUNE option d'inversion des sticks (verifie : ni le
// C++ -- seul Gunslinger, absent de THUG, lit "GunslingerInvertAiming" -- ni
// le menu CONTROL SETUP de gamemenu.qb : Vibration, Autokick, 180 Spin Taps,
// No Reverts/Manuals/Walking). Les axes eux-memes sont conformes a XBox :
// XBox/p_siodev.cpp:281-284 rend X droite = 255 et Y haut = 0 (sThumbY nie),
// sceCtrl rend la meme chose. L'inversion est donc une PREFERENCE, pas un
// correctif.
//
// Fichier texte cle=valeur, lu une fois au demarrage, build public compris
// (inject.txt et le serveur de debug y sont coupes). Absent : on l'ecrit avec
// les valeurs par defaut, pour que le joueur le trouve et l'edite en FTP ou
// dans VitaShell. Commande de dev "ctl" : relecture sans relancer le jeu.
//
// Menu en jeu : OPTIONS > Control Setup > Vita Options (vita/qb/vita_options.q,
// Sk/Scripting/Vita/vita_qb_options.cpp). Chaque bascule applique tout de
// suite et REECRIT le fichier (reglages_ecrire : modele ci-dessous, les
// commentaires ajoutes a la main par le joueur sont perdus). La cle
// "framerate" (30/60, = commande de dev "fps") vit dans le meme fichier :
// un seul fichier de reglages du port, nom inchange pour les installations
// existantes.
#define CONTROLS_PATH	"ux0:data/thug/controls.txt"

static bool s_inv_lx = false, s_inv_ly = false;
static bool s_inv_rx = false, s_inv_ry = false;

// Modele du fichier ecrit (absent au demarrage, ou bascule depuis le menu) :
// sept %d, dans l'ordre de reglages_ecrire.
static const char s_controls_modele[] =
	"# THUG Vita - port settings, read when the game starts.\n"
	"# Also editable in game: OPTIONS > Control Setup > Vita Options.\n"
	"# 0 = off, 1 = on. Delete this file to restore the defaults.\n"
	"#\n"
	"# Invert a stick axis. Left stick = skater (and menus), right stick = camera.\n"
	"invert_left_x=%d\n"
	"invert_left_y=%d\n"
	"invert_right_x=%d\n"
	"invert_right_y=%d\n"
	"#\n"
	"# 1: L/R buttons are L2/R2 (nollie, revert), spins L1/R1 are on the touch\n"
	"#    surface below. 0: L/R buttons are L1/R1, L2/R2 are on the touch surface.\n"
	"triggers_as_l2r2=%d\n"
	"#\n"
	"# 0: touch buttons are the bottom corners of the front touchscreen\n"
	"#    (both corners = L1+R1, get off the board).\n"
	"# 1: touch buttons are the rear touchpad, left half / right half.\n"
	"touch_on_rear_pad=%d\n"
	"#\n"
	"# Display frame rate cap: 60, or 30 (steadier: every frame lasts 2 vsyncs).\n"
	"framerate=%d\n";

static bool s_reglages_lus_une_fois = false;

static bool reglages_ecrire( void )
{
	char buf[2048];
	const int n = snprintf( buf, sizeof( buf ), s_controls_modele,
	                        s_inv_lx ? 1 : 0, s_inv_ly ? 1 : 0,
	                        s_inv_rx ? 1 : 0, s_inv_ry ? 1 : 0,
	                        g_vita_inv_gachettes ? 1 : 0,
	                        g_vita_tactile ? 0 : 1,
	                        ( NxVita::g_vita_cadence == 30 ) ? 30 : 60 );
	if(( n <= 0 ) || ( n >= (int)sizeof( buf )))
		return false;
	SceUID fd = sceIoOpen( CONTROLS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if( fd < 0 )
		return false;
	const int ecrit = sceIoWrite( fd, buf, n );
	sceIoClose( fd );
	return ( ecrit == n );
}

static void reglages_appliquer( const char *p_cle, int v )
{
	if( strcmp( p_cle, "invert_left_x" ) == 0 )				s_inv_lx = ( v != 0 );
	else if( strcmp( p_cle, "invert_left_y" ) == 0 )		s_inv_ly = ( v != 0 );
	else if( strcmp( p_cle, "invert_right_x" ) == 0 )		s_inv_rx = ( v != 0 );
	else if( strcmp( p_cle, "invert_right_y" ) == 0 )		s_inv_ry = ( v != 0 );
	else if( strcmp( p_cle, "triggers_as_l2r2" ) == 0 )		g_vita_inv_gachettes = ( v != 0 );
	else if( strcmp( p_cle, "touch_on_rear_pad" ) == 0 )	g_vita_tactile = ( v == 0 );
	else if( strcmp( p_cle, "framerate" ) == 0 )			NxVita::FixerCadence(( v == 30 ) ? 30 : 60 );
	else VLOG( "PAD", "controls.txt : cle inconnue '%s'", p_cle );
}

static void reglages_lire( void )
{
	s_reglages_lus_une_fois = true;
	SceUID fd = sceIoOpen( CONTROLS_PATH, SCE_O_RDONLY, 0777 );
	if( fd < 0 )
	{
		const bool ok = reglages_ecrire();
		VLOG( "PAD", "controls.txt absent : ecrit avec les valeurs par defaut (%s)",
		      ok ? "ok" : "echec" );
		return;		// les valeurs du fichier ecrit sont celles du code
	}
	char buf[2048];
	const int n = sceIoRead( fd, buf, sizeof( buf ) - 1 );
	sceIoClose( fd );
	if( n <= 0 )
		return;
	buf[n] = 0;

	// Lignes "cle=valeur" ; '#' commente jusqu'a la fin de ligne ; espaces et
	// fins de ligne CRLF (Bloc-notes Windows) toleres.
	char *p = buf;
	while( *p )
	{
		char *fin = p;
		while( *fin && *fin != '\n' )
			++fin;
		const bool derniere = ( *fin == 0 );
		*fin = 0;
		char *diese = strchr( p, '#' );
		if( diese )
			*diese = 0;
		char *egal = strchr( p, '=' );
		if( egal )
		{
			*egal = 0;
			char *cle = p;
			while( *cle == ' ' || *cle == '\t' )
				++cle;
			char *q = egal;
			while(( q > cle ) && ( q[-1] == ' ' || q[-1] == '\t' ))
				*--q = 0;
			if( *cle )
				reglages_appliquer( cle, atoi( egal + 1 ));
		}
		if( derniere )
			break;
		p = fin + 1;
	}
	VLOG( "PAD", "controls.txt : inversion lx=%d ly=%d rx=%d ry=%d, L/R=%s, tactile=%s, %d images/s",
	      s_inv_lx, s_inv_ly, s_inv_rx, s_inv_ry,
	      g_vita_inv_gachettes ? "L2/R2" : "L1/R1",
	      g_vita_tactile ? "coins avant" : "pave arriere",
	      ( NxVita::g_vita_cadence == 30 ) ? 30 : 60 );
}

// --- API du menu VITA OPTIONS (Sk/Scripting/Vita/vita_qb_options.cpp) -----
// Valeurs telles qu'ecrites dans controls.txt : 0/1, framerate 30/60.
// -1 : cle inconnue.
int VitaReglageValeur( const char *p_cle )
{
	if( !s_reglages_lus_une_fois )
		reglages_lire();
	if( strcmp( p_cle, "invert_left_x" ) == 0 )		return s_inv_lx ? 1 : 0;
	if( strcmp( p_cle, "invert_left_y" ) == 0 )		return s_inv_ly ? 1 : 0;
	if( strcmp( p_cle, "invert_right_x" ) == 0 )	return s_inv_rx ? 1 : 0;
	if( strcmp( p_cle, "invert_right_y" ) == 0 )	return s_inv_ry ? 1 : 0;
	if( strcmp( p_cle, "triggers_as_l2r2" ) == 0 )	return g_vita_inv_gachettes ? 1 : 0;
	if( strcmp( p_cle, "touch_on_rear_pad" ) == 0 )	return g_vita_tactile ? 0 : 1;
	if( strcmp( p_cle, "framerate" ) == 0 )			return ( NxVita::g_vita_cadence == 30 ) ? 30 : 60;
	return -1;
}

// Bascule entre les deux valeurs, applique tout de suite (memes chemins que
// la lecture du fichier) et reecrit controls.txt. Rend la nouvelle valeur,
// -1 si la cle est inconnue.
int VitaReglageBasculer( const char *p_cle )
{
	const int v = VitaReglageValeur( p_cle );
	if( v < 0 )
		return -1;
	const int nv = ( strcmp( p_cle, "framerate" ) == 0 ) ? (( v == 30 ) ? 60 : 30 ) : !v;
	reglages_appliquer( p_cle, nv );
	const bool ok = reglages_ecrire();
	VLOG( "PAD", "Vita Options : %s = %d (controls.txt %s)", p_cle, nv, ok ? "ecrit" : "NON ECRIT" );
	return VitaReglageValeur( p_cle );
}

// Symetrie autour du centre PS2 (128) : 0 -> 255 (256 plafonne), 128 -> 128,
// 255 -> 1. Le repos de cette console (127/123) reste dans la zone morte.
static unsigned char inverser_axe( unsigned char v )
{
	const int r = 256 - (int)v;
	return (unsigned char)(( r > 255 ) ? 255 : r );
}

static unsigned int nom_vers_bouton( const char *p_mot )
{
	struct { const char *nom; unsigned int bit; } table[] = {
		{ "haut",     SCE_CTRL_UP },       { "bas",      SCE_CTRL_DOWN },
		{ "gauche",   SCE_CTRL_LEFT },     { "droite",   SCE_CTRL_RIGHT },
		{ "croix",    SCE_CTRL_CROSS },    { "rond",     SCE_CTRL_CIRCLE },
		{ "carre",    SCE_CTRL_SQUARE },   { "triangle", SCE_CTRL_TRIANGLE },
		{ "start",    SCE_CTRL_START },    { "select",   SCE_CTRL_SELECT },
		{ "l1",       SCE_CTRL_LTRIGGER }, { "r1",       SCE_CTRL_RTRIGGER },
		{ "l2",       VITA_PAD_L2 },       { "r2",       VITA_PAD_R2 },
		{ NULL, 0 }
	};
	for( int i = 0; table[i].nom; ++i )
	{
		const char *a = table[i].nom;
		const char *b = p_mot;
		while( *a && *b )
		{
			char cb = *b;
			if(( cb >= 'A' ) && ( cb <= 'Z' ))
				cb = (char)( cb - 'A' + 'a' );
			if( *a != cb )
				break;
			++a; ++b;
		}
		if( !*a && (( *b == 0 ) || ( *b == '\n' ) || ( *b == '\r' ) || ( *b == ' ' )))
			return table[i].bit;
	}
	return 0;
}

static void injection_analyser( char *buf );

static void injection_relire( void )
{
	SceUID fd = sceIoOpen( INJECT_PATH, SCE_O_RDONLY, 0777 );
	if( fd < 0 )
		return;

	char buf[512];
	int n = sceIoRead( fd, buf, sizeof( buf ) - 1 );
	sceIoClose( fd );
	sceIoRemove( INJECT_PATH );		// consomme : on ne rejoue pas en boucle

	if( n <= 0 )
		return;
	buf[n] = 0;
	injection_analyser( buf );
}

// Analyse une ligne de commandes : venue d'inject.txt ou du serveur de debug
// TCP (vita_dbgsrv.c), qui la depose telle quelle.
static void injection_analyser( char *buf )
{
	// Une ligne SANS touche (« unlock », « zoom 1.2 »...) ne doit pas annuler
	// une sequence en cours de rejeu : on la sauvegarde, et on la restaure si
	// l'analyse n'a ajoute aucune touche. Avec le serveur TCP, reglages et
	// touches arrivent desormais entrelaces.
	const int		old_count  = s_inject_count;
	const int		old_index  = s_inject_index;
	const SceUInt64	old_start  = s_inject_start;
	const int		old_frames = s_inject_frames;
	const bool		old_held   = s_inject_held;

	s_inject_count  = 0;
	s_inject_index  = 0;
	s_inject_start  = 0;
	s_inject_frames = 0;
	s_inject_held   = true;

	char *p = buf;
	while( *p && ( s_inject_count < INJECT_MAX ))
	{
		while( *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' )
			++p;
		if( !*p )
			break;
		// Â« shot Â» n'est pas une touche : c'est une demande de capture.
		if(( p[0] == 's' || p[0] == 'S' ) && ( p[1] == 'h' || p[1] == 'H' )
		   && ( p[2] == 'o' || p[2] == 'O' ) && ( p[3] == 't' || p[3] == 'T' ))
		{
			s_inject_shot = true;
		}
		// ï¿½ trace ï¿½ arme la trace des c-functions QB, ï¿½ notrace ï¿½ la desarme.
		// Armee au demarrage elle noierait le signal sous le chargement ; ce
		// qu'on veut observer arrive bien plus tard, a un instant choisi.
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'r' || p[1] == 'R' )
		        && ( p[2] == 'a' || p[2] == 'A' ) && ( p[3] == 'c' || p[3] == 'C' ))
		{
			VitaSetCfuncTrace( true );
		}
		else if(( p[0] == 'n' || p[0] == 'N' ) && ( p[1] == 'o' || p[1] == 'O' )
		        && ( p[2] == 't' || p[2] == 'T' ) && ( p[3] == 'r' || p[3] == 'R' ))
		{
			VitaSetCfuncTrace( false );
		}
		// « qb <script> [nom=valeur ...] » lance un script QB du jeu, les
		// valeurs etant des checksums (« qb change_level level=load_tampa »)
		// ou des chaines entre guillemets, sans espace (« name="a\b.cut" »).
		// Meme chemin qu'un menu : SpawnScript, execute au prochain passage du
		// gestionnaire de scripts, pas au milieu de la lecture manette.
		// Depuis un niveau : « qb level_select_change_level level=load_nj ».
		else if(( p[0] == 'q' ) && ( p[1] == 'b' ) && ( p[2] == ' ' ))
		{
			char ligne[160];
			int n = 0;
			p += 3;
			while( *p && ( *p != '\n' ) && ( *p != '\r' ) && ( n < (int)sizeof( ligne ) - 1 ))
				ligne[n++] = *p++;
			ligne[n] = 0;
			char *mots[8];
			int nm = 0;
			for( char *t = strtok( ligne, " \t" ); t && ( nm < 8 ); t = strtok( NULL, " \t" ))
				mots[nm++] = t;
			if( nm > 0 )
			{
				Script::CStruct *p_params = new Script::CStruct;
				for( int k = 1; k < nm; ++k )
				{
					char *eg = strchr( mots[k], '=' );
					if( eg )
					{
						*eg = 0;
						// "nom=\"texte\"" : chaine (view_cutscene
						// name="cutscenes\NJ_03.cut", trailer).
						if( eg[1] == '"' )
						{
							char *fin = strrchr( eg + 2, '"' );
							if( fin )
								*fin = 0;
							p_params->AddString( Script::GenerateCRC( mots[k] ), eg + 2 );
							continue;
						}
						// "nom=0x1234abcd" : checksum brut (objectifs sans nom
						// lisible, "goals all").
						const uint32 v = ( eg[1] == '0' && ( eg[2] == 'x' || eg[2] == 'X' ))
						                 ? (uint32)strtoul( eg + 1, NULL, 16 )
						                 : Script::GenerateCRC( eg + 1 );
						p_params->AddChecksum( Script::GenerateCRC( mots[k] ), v );
					}
					else
						p_params->AddChecksum( NO_NAME, Script::GenerateCRC( mots[k] ));
				}
				// permanent + hors session : un changement de niveau tue les
				// scripts de session, dont celui-ci s'il charge un niveau.
				Script::SpawnScript( Script::GenerateCRC( mots[0] ), p_params,
				                     NO_NAME, NULL, -1, 0, false, true, true );
				delete p_params;
				VLOG( "DBG", "qb : %s lance (%d parametres)", mots[0], nm - 1 );
			}
			continue;
		}
		// « unlock » deverrouille TOUS les niveaux du free skate.
		//
		// [VERIFIE] all_levels_unlocked est une VARIABLE DE SCRIPT booleenne,
		// pas un numero de drapeau. Le bytecode de qb.prx le montre sans
		// ambiguite : la sequence « 16 <checksum e9c2cd10> 07 17 01000000 »
		// se lit « all_levels_unlocked = 1 » dans gamemenu.qb, et la meme
		// avec 00000000 dans startup.qb, qui l'initialise a zero.
		//
		// [REFUTE] premiere version : passer par CSkaterCareer::SetGlobalFlag
		// avec Script::GetInteger comme le fait ScriptClearCheats. Ce chemin
		// vaut pour les CHEAT_ON_n, qui portent bien un numero ; ici
		// GetInteger rendait 0 -- la valeur de la variable, pas un indice --
		// et on armait donc le drapeau 0, au hasard.
		//
		// Rien n'est ecrit sur la carte : la variable vit en memoire et
		// disparait a l'extinction. C'est voulu -- outil de test, pas triche
		// permanente.
		else if(( p[0] == 'u' || p[0] == 'U' ) && ( p[1] == 'n' || p[1] == 'N' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			const uint32 cs = CRCD( 0xe9c2cd10, "all_levels_unlocked" );

			// Resolve cree l'entree si elle n'existe pas encore : le menu
			// peut n'avoir jamais ete ouvert.
			Script::CSymbolTableEntry *p_sym = Script::Resolve( cs );
			if( p_sym )
			{
				p_sym->mType         = ESYMBOLTYPE_INTEGER;
				p_sym->mIntegerValue = 1;
				VLOG( "DBG", "all_levels_unlocked = 1 : niveaux deverrouilles" );
			}
			else
			{
				VLOG( "DBG", "unlock : symbole introuvable" );
			}
		}
		// « sky 0 » cesse de dessiner le ciel.
		else if(( p[0] == 's' || p[0] == 'S' ) && ( p[1] == 'k' || p[1] == 'K' )
		        && ( p[2] == 'y' || p[2] == 'Y' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_sky = ( *q != '0' );
			VLOG( "SCN", "ciel : %s", NxVita::g_vita_sky ? "dessine" : "COUPE" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « cull 0 » dessine TOUT le decor, sans test de visibilite.
		else if(( p[0] == 'c' || p[0] == 'C' ) && ( p[1] == 'u' || p[1] == 'U' )
		        && ( p[2] == 'l' || p[2] == 'L' ) && ( p[3] == 'l' || p[3] == 'L' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_cull = ( *q != '0' );
			VLOG( "SCN", "culling du decor : %s",
			      NxVita::g_vita_cull ? "actif" : "COUPE" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « hud 1 » affiche les compteurs a l'ecran, « hud 0 » les cache.
		else if(( p[0] == 'h' || p[0] == 'H' ) && ( p[1] == 'u' || p[1] == 'U' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_hud = ( *q != '0' );
			VLOG( "SYS", "affichage de debogage : %s",
			      NxVita::g_vita_hud ? "actif" : "cache" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « pdt » : sonde ponctuelle des pietons (issue #46), une image, au plus
		// 40 lignes [PDT]. Voir pdt_journal, p_NxModel.cpp.
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'd' || p[1] == 'D' )
		        && ( p[2] == 't' || p[2] == 'T' ))
		{
			Nx::g_vita_pdt = 1;
			VLOG( "PDT", "sonde armee : prochaine image" );
			while( *p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' )
				++p;
			continue;
		}
		// « endrun » termine la partie en cours (voir VitaForceEndRun).
		else if(( p[0] == 'e' || p[0] == 'E' ) && ( p[1] == 'n' || p[1] == 'N' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			VitaForceEndRun();
			VLOG( "SYS", "fin de partie forcee" );
			while( *p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' )
				++p;
			continue;
		}
		// « prof 1 » demarre la mesure, « prof 0 » l'arrete ET vide le
		// resultat dans le log. Les adresses sont resolues cote PC par
		// vita/tools/prof.py -- inutile d'embarquer une table de symboles.
		// « mp 1 » peint en MAGENTA les maillages dont le materiau declare
		// plus d'une passe -- on n'en dessine qu'une. Voir p_world_render.cpp.
		//
		// Mesure sur table : 495 des 1424 materiaux de New Jersey en declarent
		// 2 a 4. Ce marquage dit si les surfaces noires du decor en font partie.
		// « op 1 » force tout le decor en opaque : voir p_world_render.cpp.
		// « tflag 0/1 » : classement par MATFLAG_TRANSPARENT
		// « mt 0/1 » : deuxieme couche de texture.
		// « id 1 » : rendu par identifiant de maillage.
		// « info N » : tout ce que le rendu sait du maillage N.
		// « sl 0/1 » : ciel dessine en dernier (ordre XBox) ou en premier.
		// « iva 0/1 » : ignorer l'alpha des sommets quand le materiau le demande.
		// " occ 0/1 " : rejeter ce qui est cache derriere un batiment.
		else if(( p[0] == 'o' || p[0] == 'O' ) && ( p[1] == 'c' || p[1] == 'C' )
		                                       && ( p[2] == 'c' || p[2] == 'C' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_occlusion = ( *q != '0' );
			VLOG( "SCN", "occlusion : %s",
			      NxVita::g_vita_occlusion ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " fps 30|60 " : cadence d'affichage (2 ou 1 vsync par image).
		else if(( p[0] == 'f' || p[0] == 'F' ) && ( p[1] == 'p' || p[1] == 'P' )
		                                       && ( p[2] == 's' || p[2] == 'S' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::FixerCadence(( q[0] == '3' ) ? 30 : 60 );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " shd 0/1 " : decor dessine par shader (essai option B).
		else if(( p[0] == 's' || p[0] == 'S' ) && ( p[1] == 'h' || p[1] == 'H' )
		                                       && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_shader_decor = ( *q >= '0' && *q <= '9' ) ? ( *q - '0' ) : 0;
			VLOG( "SHD", "decor par shader : mode %d", NxVita::g_vita_shader_decor );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " dxa 0/1 " : realigner le pool de vitaGL avant un envoi DXT (#11).
		else if(( p[0] == 'd' || p[0] == 'D' ) && ( p[1] == 'x' || p[1] == 'X' )
		                                       && ( p[2] == 'a' || p[2] == 'A' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_dxt_align = ( *q != '0' );
			VLOG( "TEX", "realignement DXT : %s",
			      NxVita::g_vita_dxt_align ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gsk 0/1 " : skinning des personnages par le GPU.
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 's' || p[1] == 'S' )
		        && ( p[2] == 'k' || p[2] == 'K' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_skin_gpu = ( *q != '0' );
			VLOG( "SHD", "skinning GPU : %s", Nx::g_vita_skin_gpu ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " tril 0/1 " : lots opaques tries par variante de shader.
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'r' || p[1] == 'R' )
		        && ( p[2] == 'i' || p[2] == 'I' ) && ( p[3] == 'l' || p[3] == 'L' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_tri_lots = ( *q != '0' );
			VLOG( "SCN", "lots tries : %s", NxVita::g_vita_tri_lots ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " fust 0/1 " : lots dans la passe translucide.
		else if(( p[0] == 'f' || p[0] == 'F' ) && ( p[1] == 'u' || p[1] == 'U' )
		        && ( p[2] == 's' || p[2] == 'S' ) && ( p[3] == 't' || p[3] == 'T' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_lots_transp = ( *q != '0' );
			VLOG( "SCN", "lots translucides : %s", NxVita::g_vita_lots_transp ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " fusm 0/1 " : lots multi-passes (option B).
		else if(( p[0] == 'f' || p[0] == 'F' ) && ( p[1] == 'u' || p[1] == 'U' )
		        && ( p[2] == 's' || p[2] == 'S' ) && ( p[3] == 'm' || p[3] == 'M' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_lots_multi = ( *q != '0' );
			VLOG( "SCN", "lots multi-passes : %s", NxVita::g_vita_lots_multi ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " fus 0/1 " : dessiner le decor en lots fusionnes.
		else if(( p[0] == 'f' || p[0] == 'F' ) && ( p[1] == 'u' || p[1] == 'U' )
		                                       && ( p[2] == 's' || p[2] == 'S' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_lots = ( *q != '0' );
			VLOG( "SCN", "lots fusionnes : %s",
			      NxVita::g_vita_lots ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " prep 0/1 " : parcours unique du monde par image (0 = un par passe).
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'r' || p[1] == 'R' )
		                                       && ( p[2] == 'e' || p[2] == 'E' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_prepasse = ( *q != '0' );
			VLOG( "SCN", "parcours unique du monde par image : %s",
			      NxVita::g_vita_prepasse ? "OUI" : "non (un par passe)" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " dpt 0/1 " : tri par profondeur des translucides marques tries.
		else if(( p[0] == 'd' || p[0] == 'D' ) && ( p[1] == 'p' || p[1] == 'P' )
		                                       && ( p[2] == 't' || p[2] == 'T' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_tri_profondeur = ( *q != '0' );
			VLOG( "SCN", "tri par profondeur des translucides marques : %s",
			      NxVita::g_vita_tri_profondeur ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'i' || p[0] == 'I' ) && ( p[1] == 'v' || p[1] == 'V' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_ignore_vertex_alpha = ( *q != '0' );
			VLOG( "SCN", "alpha des sommets ignore si demande : %s",
			      NxVita::g_vita_ignore_vertex_alpha ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 's' || p[0] == 'S' ) && ( p[1] == 'l' || p[1] == 'L' ))
		{
			const char *q = p + 2;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_sky_last = ( *q != '0' );
			VLOG( "SCN", "ciel dessine en dernier : %s",
			      NxVita::g_vita_sky_last ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'l' || p[1] == 'L' )
		        && ( p[2] == 'a' || p[2] == 'A' ) && ( p[3] == 'n' || p[3] == 'N' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::VitaListePlan( (float)atof( q ));
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'i' || p[0] == 'I' ) && ( p[1] == 'n' || p[1] == 'N' )
		        && ( p[2] == 'f' || p[2] == 'F' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::VitaDumpMeshInfo( atoi( q ));
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'i' || p[0] == 'I' ) && ( p[1] == 'd' || p[1] == 'D' ))
		{
			const char *q = p + 2;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_id_debug = ( *q != '0' );
			VLOG( "SCN", "rendu par identifiant : %s",
			      NxVita::g_vita_id_debug ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « tm 1 » : nommer les modeles dessines.
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'm' || p[1] == 'M' ))
		{
			const char *q = p + 2;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_trace_modeles = ( *q != '0' );
			Nx::g_vita_trace_reset   = Nx::g_vita_trace_modeles;
			Nx::g_vita_trace_reset_inactifs = Nx::g_vita_trace_modeles;
			VLOG( "DRAW", "trace des modeles : %s",
			      Nx::g_vita_trace_modeles ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 't' || p[1] == 'T' ))
		{
			const char *q = p + 2;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_multitex = ( *q != '0' );
			VLOG( "SCN", "deuxieme couche de texture : %s",
			      NxVita::g_vita_multitex ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'f' || p[1] == 'F' ))
		{
			const char *q = p + 2;
			while(( *q >= 'a' && *q <= 'z' ) || ( *q >= 'A' && *q <= 'Z' ))	// mot entier (tflag, zwr, atest)
				++q;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_transp_flag = ( *q != '0' );
			VLOG( "SCN", "classement par MATFLAG_TRANSPARENT : %s", NxVita::g_vita_transp_flag ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « zwr 0/1 » : profondeur ecrite par les translucides
		else if(( p[0] == 'z' || p[0] == 'Z' ) && ( p[1] == 'w' || p[1] == 'W' ) && ( p[2] != 'm' ) && ( p[2] != 'M' ))
		{
			const char *q = p + 2;
			while(( *q >= 'a' && *q <= 'z' ) || ( *q >= 'A' && *q <= 'Z' ))	// mot entier (tflag, zwr, atest)
				++q;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_zwrite_transp = ( *q != '0' );
			VLOG( "SCN", "profondeur ecrite par les translucides : %s", NxVita::g_vita_zwrite_transp ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « atest 0/1 » : test alpha du materiau
		else if(( p[0] == 'a' || p[0] == 'A' ) && ( p[1] == 't' || p[1] == 'T' ))
		{
			const char *q = p + 2;
			while(( *q >= 'a' && *q <= 'z' ) || ( *q >= 'A' && *q <= 'Z' ))	// mot entier (tflag, zwr, atest)
				++q;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_alpha_test = ( *q != '0' );
			VLOG( "SCN", "test alpha du materiau : %s", NxVita::g_vita_alpha_test ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'o' || p[0] == 'O' ) && ( p[1] == 'p' || p[1] == 'P' ))
		{
			const char *q = p + 2;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_force_opaque = ( *q != '0' );
			VLOG( "SCN", "decor force en opaque : %s",
			      NxVita::g_vita_force_opaque ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 'p' || p[1] == 'P' ))
		{
			const char *q = p + 2;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_mat_debug = ( *q != '0' );
			VLOG( "SCN", "marquage multi-passes : %s",
			      NxVita::g_vita_mat_debug ? "MAGENTA" : "coupe" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'r' || p[1] == 'R' )
		        && ( p[2] == 'o' || p[2] == 'O' ) && ( p[3] == 'f' || p[3] == 'F' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			if( *q == '0' )
			{
				vita_prof_enable( 0 );
				static unsigned int       addr[512];
				static unsigned long long tot[512];
				static unsigned int       calls[512];
				int n = vita_prof_dump( addr, tot, calls, 512 );
				VLOG( "PROF", "=== profil : %d fonctions ===", n );
				// Repere pour prof.py : adresse d'execution d'un symbole connu,
				// d'ou le decalage entre l'ELF et le module charge.
				VLOG( "PROF", "repere vita_prof_enable = 0x%08x",
				      (unsigned int)(uintptr_t)&vita_prof_enable );
				for( int k = 0; k < n; ++k )
				{
					if( tot[k] < 1000 )
						continue;
					VLOG( "PROF", "  0x%08x  %6llu us  %5u appels",
					      addr[k], tot[k], calls[k] );
				}
			}
			else
			{
				vita_prof_enable( 1 );
				VLOG( "PROF", "profileur ARME" );
			}
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// Â« lum 0 Â» coupe l'eclairage par sommet, Â« lum 1 Â» le remet. Sert a
		// repondre par la mesure, sur la meme scene, a Â« est-ce l'eclairage
		// qui coute ? Â» -- une comparaison vaut mieux qu'une deduction.
		else if(( p[0] == 'l' || p[0] == 'L' ) && ( p[1] == 'u' || p[1] == 'U' )
		        && ( p[2] == 'm' || p[2] == 'M' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_lighting = ( *q != '0' );
			VLOG( "SCN", "eclairage par sommet : %s",
			      Nx::g_vita_lighting ? "actif" : "coupe" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// Â« zoom 1.35 Â» regle le champ de vision du rendu 3D sans recompiler.
		else if(( p[0] == 'z' || p[0] == 'Z' ) && ( p[1] == 'o' || p[1] == 'O' )
		        && ( p[2] == 'o' || p[2] == 'O' ) && ( p[3] == 'm' || p[3] == 'M' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			float v = (float)atof( q );
			if(( v >= 0.2f ) && ( v <= 5.0f ))
			{
				NxVita::g_vita_world_zoom = v;
				VLOG( "SCN", "zoom du rendu 3D : %.3f", v );
			}
			else
			{
				VLOG( "SCN", "zoom refuse (%.3f) -- garde %.3f",
				      v, NxVita::g_vita_world_zoom );
			}
			// Sauter la valeur numerique pour ne pas la relire comme touches.
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " texsum N " : empreinte du contenu de la texture GL N.
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'e' || p[1] == 'E' )
		        && ( p[2] == 'x' || p[2] == 'X' ) && ( p[3] == 's' || p[3] == 'S' ))
		{
			const char *q = p + 6;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::VitaTexSum( (unsigned int)atoi( q ));
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " arr " : vitesse du skater a zero (vues de reference, issue #18).
		else if(( p[0] == 'a' || p[0] == 'A' ) && ( p[1] == 'r' || p[1] == 'R' )
		        && ( p[2] == 'r' || p[2] == 'R' ))
		{
			Obj::CSkater *p_sk = Mdl::Skate::Instance()->GetLocalSkater();
			if( p_sk )
			{
				p_sk->SetVel( Mth::Vector( 0.0f, 0.0f, 0.0f ));
				VLOG( "DBG", "skater arrete" );
			}
		}
		// " pos " : position et cap du skater ; " tp X Y Z CAP " : l'y poser.
		// Vues de reference reproductibles pour les mesures (issue #18).
		else if((( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'o' || p[1] == 'O' )
		         && ( p[2] == 's' || p[2] == 'S' ))
		        || (( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'p' || p[1] == 'P' )
		            && ( p[2] == ' ' )))
		{
			const bool tp = ( p[0] == 't' || p[0] == 'T' );
			const char *q = p + 3;
			Obj::CSkater *p_sk = Mdl::Skate::Instance()->GetLocalSkater();
			if( !p_sk )
				VLOG( "DBG", "pos/tp : pas de skater" );
			else if( !tp )
			{
				const Mth::Vector &v = p_sk->GetPos();
				VLOG( "DBG", "skater en %.1f %.1f %.1f cap %.1f", v[X], v[Y], v[Z], p_sk->VitaCap());
			}
			else
			{
				float c[4] = { 0, 0, 0, 0 };
				for( int k = 0; k < 4; ++k )
				{
					while( *q == ' ' ) ++q;
					c[k] = (float)atof( q );
					while( *q && *q != ' ' ) ++q;
				}
				p_sk->VitaTeleport( Mth::Vector( c[0], c[1], c[2], 1.0f ), c[3] );
				VLOG( "DBG", "skater pose en %.1f %.1f %.1f cap %.1f", c[0], c[1], c[2], c[3] );
			}
			while( *q && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// " mip 0/1/2 " : filtrage des mipmaps du decor (issue #18).
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 'i' || p[1] == 'I' )
		        && ( p[2] == 'p' || p[2] == 'P' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::VitaAppliquerMip( atoi( q ));
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " mem 0/1 " : sonde periodique de la memoire (couteuse).
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 'e' || p[1] == 'E' )
		        && ( p[2] == 'm' || p[2] == 'M' ) && (( p[3] == ' ' ) || ( p[3] == '=' )))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_sonde_memoire = ( *q != '0' );
			VLOG( "PROF", "sonde memoire : %s", Nx::g_vita_sonde_memoire ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " cpl 0/1 " : culling par plage des lots multi-passes.
		else if(( p[0] == 'c' || p[0] == 'C' ) && ( p[1] == 'p' || p[1] == 'P' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_cull_plages = ( *q != '0' );
			VLOG( "PROF", "culling par plage des lots multi : %s", NxVita::g_vita_cull_plages ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " lmd 0/1 " : lots opaques precalcules enregistres sur un autre coeur.
		else if(( p[0] == 'l' || p[0] == 'L' ) && ( p[1] == 'm' || p[1] == 'M' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_lots_mt = ( *q != '0' );
			VLOG( "PROF", "lots opaques sur l'autre coeur : %s", NxVita::g_vita_lots_mt ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gcl 0/1 " : listes de commandes GXM (contexte differe), issue #18.
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'c' || p[1] == 'C' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_listes_gxm = ( *q != '0' );
			VLOG( "PROF", "listes de commandes GXM : %s", NxVita::g_vita_listes_gxm ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « uvl » : liste des maillages a UV animes (issue #43).
		else if(( p[0] == 'u' || p[0] == 'U' ) && ( p[1] == 'v' || p[1] == 'V' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			NxVita::VitaListeUVAnimes();
			while( *p && *p != '\n' && *p != '\r' )
				++p;
			continue;
		}
		// « rfl X Z » : liste des maillages a reflet proches (issue #5).
		else if(( p[0] == 'r' || p[0] == 'R' ) && ( p[1] == 'f' || p[1] == 'F' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			float x = 0.0f, z = 0.0f;
			sscanf( p + 3, "%f %f", &x, &z );
			NxVita::VitaListeReflets( x, z );
			while( *p && *p != '\n' && *p != '\r' )
				++p;
			continue;
		}
		// "txt" : textes visibles a l'ecran (#51).
		else if( strncmp( p, "txt", 3 ) == 0 )
		{
			NxVita::JournaliserTextes();
			p += 3;
			continue;
		}
		// "omc 0/1" : decoupe materielle (plans CLP) de la reception de l'ombre
		// portee aux bords de sa carte, #70. 0 = sols recepteurs redessines en
		// entier (ancien rendu, meme image, ~9 ms de GPU de plus a Vancouver).
		// "omr 0/1" (#73) : reception de l'ombre limitee au rectangle ecran de
		// sa boite par la region de tuiles GXM (remplace la decoupe CLP).
		else if( strncmp( p, "omr", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			if( *q >= '0' && *q <= '9' )
				NxVita::g_vita_ombre_region = ( *q++ != '0' ) ? 1 : 0;
			VLOG( "OMB", "region de tuiles de la reception : %s",
			      NxVita::g_vita_ombre_region ? "MARCHE" : "ARRET" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		else if( strncmp( p, "omc", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			if( *q >= '0' && *q <= '9' )
				NxVita::g_vita_ombre_decoupe = ( *q++ != '0' ) ? 1 : 0;
			VLOG( "OMB", "decoupe de la reception (CLP) : %s",
			      NxVita::g_vita_ombre_decoupe ? "MARCHE" : "ARRET" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "pll X" : plan lointain de la projection du decor (#69).
		else if( strncmp( p, "pll", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			NxVita::g_vita_plan_loin = (float)atof( q );
			VLOG( "SCN", "plan lointain : %.0f", NxVita::g_vita_plan_loin );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "omd X" : PCF de l'ombre portee, decalage en texels (0 = ancien rendu), #69.
		else if( strncmp( p, "omd", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			NxVita::g_vita_ombre_douce = (float)atof( q );
			VLOG( "OMB", "ombre douce : %.2f texel", NxVita::g_vita_ombre_douce );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "cvm 0/1" : vue du decor = celle des modeles (1) ou camera active (0), #65.
		else if( strncmp( p, "cvm", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			NxVita::g_vita_cam_modeles = atoi( q );
			VLOG( "CAM", "vue du decor = vue des modeles : %d", NxVita::g_vita_cam_modeles );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "violet 0/1" : fond d'effacement violet de diagnostic (#18 ; defaut =
		// gris-bleu XBox 0x506070).
		else if( strncmp( p, "violet", 6 ) == 0 )
		{
			const char *q = p + 6;
			while( *q == ' ' ) ++q;
			NxVita::g_vita_fond_violet = ( atoi( q ) != 0 );
			VLOG( "GFX", "fond violet de diagnostic : %d", (int)NxVita::g_vita_fond_violet );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "voix" : voix sonores actives, nom et age (#57).
		else if( strncmp( p, "voix", 4 ) == 0 )
		{
			Sfx::VitaListeVoix();
			p += 4;
			continue;
		}
		// "mix N" : enregistre N s de la sortie des effets (#30).
		else if( strncmp( p, "mix", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			Sfx::VitaEnregistreMix( atoi( q ));
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "ctl" : relit ux0:data/thug/controls.txt (#31).
		else if( strncmp( p, "ctl", 3 ) == 0 )
		{
			reglages_lire();
			p += 3;
			continue;
		}
		// "tac 0/1" : L1/R1 aux coins bas de l'ecran avant, les deux = descente (#76).
		else if( strncmp( p, "tac", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			g_vita_tactile = atoi( q );
			VLOG( "PAD", "coins tactiles avant (#76) : %d", g_vita_tactile );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "trk 0/1" : trace du chemin d'un manual, Up/Down -> file -> declenchement
		// -> equilibre (issue #6, trickcomponent.cpp). Muette par defaut.
		else if( strncmp( p, "trk", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			g_vita_trace_trk = atoi( q );
			VLOG( "TRK", "trace des manuals (#6) : %d", g_vita_trace_trk );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "inv 0/1" : gachettes L/R = L2/R2 et pave arriere = L1/R1 (#59, #60).
		else if( strncmp( p, "inv", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			g_vita_inv_gachettes = atoi( q );
			VLOG( "PAD", "gachettes inversees : %d", g_vita_inv_gachettes );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "goals" : chapitre, etape et objectifs actifs ou gagnes (Story Mode, #51).
		else if( strncmp( p, "goals", 5 ) == 0 )
		{
			const bool tous = ( strncmp( p, "goals all", 9 ) == 0 );	// liste complete
			Game::CGoalManager *gm = Game::GetGoalManager();
			if( !gm )
				VLOG( "GOAL", "pas de GoalManager" );
			else
			{
				const int n = gm->GetNumGoals();
				int na = 0, nw = 0, nl_ = 0;
				for( int i = 0; i < n; ++i )
				{
					Game::CGoal *g = gm->GetGoalByIndex( i );
					if( !g ) continue;
					const bool a = g->IsActive(), w = g->HasWonGoal(), l = g->IsLocked();
					na += a; nw += w; nl_ += l;
					if( tous )
						VLOG( "GOAL", "  objectif %08x %s : %s%s%s", (unsigned)g->GetGoalId(),
						      Script::FindChecksumName( g->GetGoalId()), l ? "verrouille" : "libre",
						      w ? " gagne" : "", a ? " ACTIF" : "" );
					if( a )
						VLOG( "GOAL", "  actif %08x %s%s", (unsigned)g->GetGoalId(),
						      Script::FindChecksumName( g->GetGoalId()), w ? " (gagne)" : "" );
				}
				VLOG( "GOAL", "chapitre %d etape %d : %d objectifs, %d actifs, %d gagnes, %d verrouilles",
				      gm->GetCurrentChapter(), gm->GetCurrentStage(), n, na, nw, nl_ );
			}
			p += tous ? 9 : 5;
			continue;
		}
		// "osp 0/1" : derniere pose d'os des vehicules (#48).
		else if( strncmp( p, "osp", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			Nx::g_vita_os_persist = atoi( q );
			VLOG( "PROF", "pose d'os persistante : %d", Nx::g_vita_os_persist );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "vss 0/1" : sous-pas de la physique des vehicules au-dela de 1/60 s
		// (#13) ; 0 = regle d'origine (au-dela de 1/30 s seulement).
		else if( strncmp( p, "vss", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			Obj::g_vita_veh_sous_pas = atoi( q );
			VLOG( "VEH", "sous-pas au-dela de 1/60 s : %d", Obj::g_vita_veh_sous_pas );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "vlg 0/1" : journal [VEH] du vehicule conduit et de sa camera (#13, #14).
		else if( strncmp( p, "vlg", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			Obj::g_vita_veh_log = atoi( q );
			VLOG( "VEH", "journal vehicule : %d", Obj::g_vita_veh_log );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "dlb 0/1" : destructions differees hors de l'image (#48).
		else if( strncmp( p, "dlb", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			Nx::g_vita_differer_lib = atoi( q );
			VLOG( "PROF", "destructions differees : %d", Nx::g_vita_differer_lib );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "zwm 0/1" : ecriture de profondeur imposee aux modeles (#48).
		else if( strncmp( p, "zwm", 3 ) == 0 )
		{
			const char *q = p + 3;
			while( *q == ' ' ) ++q;
			Nx::g_vita_zw_modeles = atoi( q );
			VLOG( "PROF", "profondeur imposee aux modeles : %d", Nx::g_vita_zw_modeles );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' ) ++q;
			p = (char *)q;
			continue;
		}
		// "rlu 0/1" : eclairage des modeles rigides (XBox instance.cpp:200).
		else if(( p[0] == 'r' || p[0] == 'R' ) && ( p[1] == 'l' || p[1] == 'L' ) && ( p[2] == 'u' || p[2] == 'U' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			Nx::g_vita_rigide_lum = ( atoi( q ) != 0 );
			VLOG( "PROF", "eclairage des modeles rigides : %d", (int)Nx::g_vita_rigide_lum );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "lpi 0/1" : couleurs eclairees des pieces rigides propres a chaque
		// instance (#78) ; 0 = cache unique par piece du maillage partage.
		// Aucune commande "lp" ni "l" seule.
		else if(( p[0] == 'l' || p[0] == 'L' ) && ( p[1] == 'p' || p[1] == 'P' ) && ( p[2] == 'i' || p[2] == 'I' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			Nx::g_vita_lum_instance = ( atoi( q ) != 0 );
			VLOG( "PROF", "eclairage des rigides par instance (lpi) : %d", (int)Nx::g_vita_lum_instance );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "lpa 0/1" : reeclairage decide pour le vehicule entier (#78) ; 0 = piece par piece.
		else if(( p[0] == 'l' || p[0] == 'L' ) && ( p[1] == 'p' || p[1] == 'P' ) && ( p[2] == 'a' || p[2] == 'A' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			Nx::g_vita_lum_atomique = ( atoi( q ) != 0 );
			VLOG( "PROF", "reeclairage par vehicule entier (lpa) : %d", (int)Nx::g_vita_lum_atomique );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "rgp 0/1" : eclairage des modeles rigides par le GPU (#67).
		else if(( p[0] == 'r' || p[0] == 'R' ) && ( p[1] == 'g' || p[1] == 'G' ) && ( p[2] == 'p' || p[2] == 'P' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			Nx::g_vita_rigide_gpu = ( atoi( q ) != 0 );
			VLOG( "PROF", "eclairage des modeles rigides par le GPU (rgp) : %d", (int)Nx::g_vita_rigide_gpu );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "rej 0/1" : rejeu des poses des modeles non mis a jour (#54).
		else if(( p[0] == 'r' || p[0] == 'R' ) && ( p[1] == 'e' || p[1] == 'E' ) && ( p[2] == 'j' || p[2] == 'J' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			Nx::g_vita_rejeu = atoi( q );
			VLOG( "PROF", "rejeu des poses : %d", Nx::g_vita_rejeu );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "mx2 0/1" : modulation x2 des sprites/textes comme XBox (#52).
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 'x' || p[1] == 'X' ) && ( p[2] == '2' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			NxVita::g_vita_mx2 = atoi( q );
			VLOG( "PROF", "sprites x2 : %d", NxVita::g_vita_mx2 );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "snt 0/1" : sprites sans texture dessines comme XBox (#65).
		else if(( p[0] == 's' || p[0] == 'S' ) && ( p[1] == 'n' || p[1] == 'N' ) && ( p[2] == 't' || p[2] == 'T' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			NxVita::g_vita_sprites_sans_tex = atoi( q );
			VLOG( "PROF", "sprites sans texture : %d", NxVita::g_vita_sprites_sans_tex );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "pzw 0/1" : profondeur ecrite par les translucides des personnages (#46).
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'z' || p[1] == 'Z' ) && ( p[2] == 'w' || p[2] == 'W' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			Nx::g_vita_zw_peau = atoi( q );
			VLOG( "PROF", "profondeur des translucides des personnages : %d", Nx::g_vita_zw_peau );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "ax2 0/1" : alpha des personnages double comme XBox (#46).
		else if(( p[0] == 'a' || p[0] == 'A' ) && ( p[1] == 'x' || p[1] == 'X' ) && ( p[2] == '2' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			NxVita::g_vita_alpha_x2 = atoi( q );
			VLOG( "PROF", "alpha des personnages x2 : %d", NxVita::g_vita_alpha_x2 );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "lmk N" : facteur de la lumiere des personnages (#46).
		else if(( p[0] == 'l' || p[0] == 'L' ) && ( p[1] == 'm' || p[1] == 'M' ) && ( p[2] == 'k' || p[2] == 'K' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' ) ++q;
			NxVita::g_vita_lum_k = (float)atof( q );
			VLOG( "PROF", "lumiere des personnages x%.2f", NxVita::g_vita_lum_k );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' ) ++q;
			p = (char *)q;
			continue;
		}
		// "mbf 0/1" : faces arriere des personnages selon le materiau (#46).
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 'b' || p[1] == 'B' )
		        && ( p[2] == 'f' || p[2] == 'F' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_mbf = atoi( q );
			VLOG( "PROF", "culling des personnages : %d", Nx::g_vita_mbf );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « bop 0/1 » : opaques a melange (passe 0 BLEND... sans
		// MATFLAG_TRANSPARENT) dessines avec leur melange, comme XBox
		// (material.cpp:299). 0 : sans melange, comportement precedent.
		else if(( p[0] == 'b' || p[0] == 'B' ) && ( p[1] == 'o' || p[1] == 'O' )
		        && ( p[2] == 'p' || p[2] == 'P' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_melange_opaques = ( *q != '0' );
			VLOG( "SCN", "opaques a melange (passe 0 BLEND sans drapeau transparent) : %s",
			      NxVita::g_vita_melange_opaques ? "melanges, passe translucide (XBox)" : "opaques, sans melange" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « bfc 0/1/2 » : culling des faces arriere du decor (issue #38).
		else if(( p[0] == 'b' || p[0] == 'B' ) && ( p[1] == 'f' || p[1] == 'F' )
		        && ( p[2] == 'c' || p[2] == 'C' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_bfc = atoi( q );
			VLOG( "PROF", "culling du decor : %d (0 aucun, 1 GL_BACK, 2 GL_FRONT)", NxVita::g_vita_bfc );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « kick 0/1 » : autokick du skater 0. Coupe, le skater reste
		// immobile apres un « tp » -- comparaisons avec la reference xemu
		// (vita/tools/compare.py), dont les etats ont l'autokick coupe.
		else if(( p[0] == 'k' || p[0] == 'K' ) && ( p[1] == 'i' || p[1] == 'I' )
		        && ( p[2] == 'c' || p[2] == 'C' ) && ( p[3] == 'k' || p[3] == 'K' ))
		{
			const char *q = p + 4;
			while( *q == ' ' || *q == '=' )
				++q;
			Mdl::Skate::Instance()->SetAutoKick( 0, *q != '0' );
			VLOG( "DBG", "autokick : %s", ( *q != '0' ) ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "fog 0/1" : brouillard des niveaux (issue #45).
		else if(( p[0] == 'f' || p[0] == 'F' ) && ( p[1] == 'o' || p[1] == 'O' )
		        && ( p[2] == 'g' || p[2] == 'G' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_fog = ( *q != '0' ) ? 1 : 0;
			VLOG( "FOG", "brouillard : %s (etat du niveau : %s)", NxVita::g_vita_fog ? "OUI" : "non",
			      NxVita::BrouillardActif() ? "actif" : "inactif" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "aom 0/1/2 [b0 b1 [k]]" : auto-ombrage du skater (issue #45,
		// p_ombre.h). 0 = coupe (defaut), 1 = comme XBox (part 0,25 attenuee
		// par la distance), 2 = diagnostic (part 1, sans attenuation). b0 b1 :
		// biais constant et de pente en unites monde (defaut 1,5 et 1) ; k :
		// part d'ombre du mode 1 (defaut 0,25, XBox instance.cpp:547).
		// Sans effet si l'ombre portee est coupee (omb 0).
		else if(( p[0] == 'a' || p[0] == 'A' ) && ( p[1] == 'o' || p[1] == 'O' )
		        && ( p[2] == 'm' || p[2] == 'M' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			if( *q >= '0' && *q <= '9' )
				NxVita::g_vita_auto_ombre = *q++ - '0';
			float v[3];
			int n = 0;
			while( n < 3 )
			{
				while( *q == ' ' )
					++q;
				char *fin = NULL;
				const double x = strtod( q, &fin );
				if( fin == q )
					break;
				v[n++] = (float)x;
				q = fin;
			}
			if( n >= 2 )
			{
				NxVita::g_vita_aom_b0 = v[0];
				NxVita::g_vita_aom_b1 = v[1];
			}
			if( n >= 3 )
				NxVita::g_vita_aom_k = v[2];
			VLOG( "OMB", "auto-ombrage du skater : %d (%s), biais %.2f %.2f, part %.2f%s",
			      NxVita::g_vita_auto_ombre,
			      NxVita::g_vita_auto_ombre == 0 ? "ARRET" : ( NxVita::g_vita_auto_ombre == 2 ? "DIAGNOSTIC" : "MARCHE" ),
			      NxVita::g_vita_aom_b0, NxVita::g_vita_aom_b1, NxVita::g_vita_aom_k,
			      NxVita::g_vita_ombre ? "" : " -- omb 0 : sans effet" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "omb 0/1/2/3 [f u]" : ombre portee du skater (issue #45 V2 / #4,
		// p_ombre.cpp). 0 = coupee, 1 = ombre, 2 = ombre + carte en coin
		// d'ecran, 3 = comme 2, carte lue a l'envers en V (diagnostic). f u
		// optionnels : glPolygonOffset de la reception (XBox -2 -4).
		// "omt 0/1" : reception de l'ombre portee sur les translucides
		// (XBox/NX/render.cpp:2793 ; p_world_render.cpp). Defaut 1 (#8).
		else if(( p[0] == 'o' || p[0] == 'O' ) && ( p[1] == 'm' || p[1] == 'M' )
		        && ( p[2] == 't' || p[2] == 'T' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			if( *q >= '0' && *q <= '9' )
				NxVita::g_vita_ombre_transp = ( *q++ != '0' ) ? 1 : 0;
			VLOG( "OMB", "ombre portee sur les translucides : %s",
			      NxVita::g_vita_ombre_transp ? "MARCHE" : "ARRET" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		else if(( p[0] == 'o' || p[0] == 'O' ) && ( p[1] == 'm' || p[1] == 'M' )
		        && ( p[2] == 'b' || p[2] == 'B' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			if( *q >= '0' && *q <= '9' )
				NxVita::g_vita_ombre = *q++ - '0';
			float v[2];
			int n = 0;
			while( n < 2 )
			{
				while( *q == ' ' )
					++q;
				char *fin = NULL;
				const double x = strtod( q, &fin );
				if( fin == q )
					break;
				v[n++] = (float)x;
				q = fin;
			}
			if( n == 2 )
			{
				NxVita::g_vita_ombre_pf = v[0];
				NxVita::g_vita_ombre_pu = v[1];
			}
			VLOG( "OMB", "ombre portee : %d (%s), decalage reception %.1f %.1f",
			      NxVita::g_vita_ombre,
			      NxVita::g_vita_ombre == 0 ? "ARRET" : ( NxVita::g_vita_ombre == 1 ? "MARCHE"
			      : ( NxVita::g_vita_ombre == 2 ? "MARCHE + carte" : "MARCHE + carte, V inverse" )),
			      NxVita::g_vita_ombre_pf, NxVita::g_vita_ombre_pu );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « zbi 0/1 » : z-bias des decalques (issue #36).
		else if(( p[0] == 'z' || p[0] == 'Z' ) && ( p[1] == 'b' || p[1] == 'B' )
		        && ( p[2] == 'i' || p[2] == 'I' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_zbias = ( *q != '0' );
			VLOG( "PROF", "z-bias des decalques : %s", NxVita::g_vita_zbias ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "gam 0/1/2 [r g b]" : rampe gamma Xbox sur l'image finale (#45, #22).
		// 0 = sans, 1 = rampe (defaut), 2 = rampe, texture lue a l'envers en V
		// (diagnostic). r g b optionnels : valeurs normalisees 0..1 du menu
		// Adjust Gamma (defaut Xbox 0.14 0.13 0.12, nx_init.cpp:354).
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'a' || p[1] == 'A' )
		        && ( p[2] == 'm' || p[2] == 'M' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			if( *q >= '0' && *q <= '9' )
				NxVita::g_vita_gamma = *q++ - '0';
			float v[3];
			int n = 0;
			while( n < 3 )
			{
				while( *q == ' ' )
					++q;
				char *fin = NULL;
				const double x = strtod( q, &fin );
				if( fin == q )
					break;
				v[n++] = (float)x;
				q = fin;
			}
			if( n == 3 )
				NxVita::SetGammaNormalized( v[0], v[1], v[2] );
			float r, g, b;
			NxVita::GetGammaNormalized( &r, &g, &b );
			VLOG( "GAM", "rampe gamma : %s, normalisee %.2f %.2f %.2f",
			      NxVita::g_vita_gamma == 0 ? "ARRET" : ( NxVita::g_vita_gamma == 2 ? "MARCHE (V inverse)" : "MARCHE" ),
			      r, g, b );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "prt 0..4" : particules parametriques (CNewParticle, systemes
		// NEWFLAT : vapeur, fumees, feux). 0 = coupees, 1 = comme XBox (point
		// sprites bornes a 64 px au-dela de 240 u), 2 = quads sans borne
		// calcules sur CPU, 3 = calcul CPU seul (mesure), 4 = quads calcules
		// par vertex shader (repli CPU si le shader manque).
		// p_NxNewParticle.cpp, journal [PRT].
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'r' || p[1] == 'R' )
		        && ( p[2] == 't' || p[2] == 'T' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			if( *q >= '0' && *q <= '4' )
				NxVita::g_vita_particules = *q - '0';
			static const char *s_noms_prt[5] = { "ARRET", "MARCHE, comme XBox",
			                                     "MARCHE, quads sans borne (CPU)",
			                                     "calcul CPU seul, sans dessin",
			                                     "MARCHE, quads par vertex shader" };
			VLOG( "PRT", "particules : %d (%s)", NxVita::g_vita_particules,
			      (unsigned)NxVita::g_vita_particules <= 4u
			      ? s_noms_prt[NxVita::g_vita_particules] : "?" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "prs 0/1" : anciennes particules (CParticle, CreateParticleSystem :
		// sang, etincelles des grinds, eclaboussures). 0 = simulees mais pas
		// dessinees (defaut tant que non valide), 1 = dessinees.
		// p_NxParticle.cpp, journal [PRS].
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'r' || p[1] == 'R' )
		        && ( p[2] == 's' || p[2] == 'S' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			if( *q == '0' || *q == '1' )
				NxVita::g_vita_prs = *q - '0';
			VLOG( "PRS", "anciennes particules : %s", NxVita::g_vita_prs ? "DESSINEES" : "simulees, NON dessinees" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "uvw 0/1" : animation des UV des materiaux (UV wibble, issue #43).
		else if(( p[0] == 'u' || p[0] == 'U' ) && ( p[1] == 'v' || p[1] == 'V' )
		        && ( p[2] == 'w' || p[2] == 'W' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_uvw = ( *q != '0' );
			VLOG( "PROF", "UV wibble des materiaux : %s", NxVita::g_vita_uvw ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "bbd 0/1" : billboards orientes vers la camera (issue #2). A 0, les
		// quads reprennent leur orientation d'export a l'image suivante.
		else if(( p[0] == 'b' || p[0] == 'B' ) && ( p[1] == 'b' || p[1] == 'B' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_billboards = ( *q != '0' );
			VLOG( "SCN", "billboards : %s", NxVita::g_vita_billboards ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "vcw 0/1" : couleurs de sommets animees (vertex color wibble, issue
		// #45 V2). A 0, les couleurs d'origine sont remises a l'image suivante.
		else if(( p[0] == 'v' || p[0] == 'V' ) && ( p[1] == 'c' || p[1] == 'C' )
		        && ( p[2] == 'w' || p[2] == 'W' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_vcw = ( *q != '0' );
			VLOG( "VCW", "vertex color wibble : %s", NxVita::g_vita_vcw ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "tsc 0/1" : teinte de scene du decor (SetSceneColor, ambiance de
		// nuit, issue #63). A 0, les couleurs d'origine sont remises.
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 's' || p[1] == 'S' )
		        && ( p[2] == 'c' || p[2] == 'C' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_teinte_scene = ( *q != '0' );
			NxVita::TeinteSceneReappliquer();
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "tix N" : teinte de scene (issue #15). 0 ancien parcours complet,
		// 1 index synchrone, 2 (defaut) index + petits changements etales sur
		// plusieurs images, 3 tout etale.
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'i' || p[1] == 'I' )
		        && ( p[2] == 'x' || p[2] == 'X' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			const int v = ( *q >= '0' && *q <= '3' ) ? ( *q - '0' ) : 2;
			NxVita::g_vita_teinte_index = v;
			VLOG( "SCN", "teinte de scene (tix) : mode %d", v );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "tbu N" : budget de l'etalement de la teinte, en ms par image (#15).
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'b' || p[1] == 'B' )
		        && ( p[2] == 'u' || p[2] == 'U' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			int ms = atoi( q );
			if( ms < 1 ) ms = 1;
			NxVita::g_vita_teinte_budget_us = ms * 1000;
			VLOG( "SCN", "teinte de scene : budget %d ms par image", ms );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "tvn 0/1" : reecriture des couleurs par NEON (1) ou scalaire (0) (#15).
		else if(( p[0] == 't' || p[0] == 'T' ) && ( p[1] == 'v' || p[1] == 'V' )
		        && ( p[2] == 'n' || p[2] == 'N' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_teinte_neon = ( *q != '0' );
			VLOG( "SCN", "teinte de scene : NEON %s", NxVita::g_vita_teinte_neon ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// "xgl 0/1" : lots de glyphes du texte 2D (1) ou un glDrawArrays par
		// caractere (0), pour l'A/B de l'issue #17 (menu View Stats).
		else if(( p[0] == 'x' || p[0] == 'X' ) && ( p[1] == 'g' || p[1] == 'G' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_lots_glyphes = ( *q != '0' );
			VLOG( "2D", "lots de glyphes (xgl) : %s", NxVita::g_vita_lots_glyphes ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " env 0/1 " : couche 1 en environment mapping (issue #5).
		else if(( p[0] == 'e' || p[0] == 'E' ) && ( p[1] == 'n' || p[1] == 'N' )
		        && ( p[2] == 'v' || p[2] == 'V' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_env = ( *q != '0' );
			VLOG( "PROF", "reflets (envmap) : %s", NxVita::g_vita_env ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " epx 0/1 " (issue #72) : passes 2-3 en environment mapping dessinees
		// en reflet, suivies des passes d'apres. 0 = ancien rendu (chaine
		// arretee a la passe en reflet).
		else if(( p[0] == 'e' || p[0] == 'E' ) && ( p[1] == 'p' || p[1] == 'P' )
		        && ( p[2] == 'x' || p[2] == 'X' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_env_x = ( *q != '0' );
			VLOG( "SHD", "passes 2-3 en reflet (epx, #72) : %s",
			      NxVita::g_vita_env_x ? "OUI (XBox)" : "non (ancien)" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// « zeq 0/1 » : test de profondeur du decor LEQUAL comme XBox
		// (nx_init.cpp:48). 0 = ancien rendu, GL_LESS : un maillage superpose
		// a un autre (memes sommets, materiau decoupe) etait rejete -- tour
		// de Tampa en miroir de ciel.
		else if(( p[0] == 'z' || p[0] == 'Z' ) && ( p[1] == 'e' || p[1] == 'E' )
		        && ( p[2] == 'q' || p[2] == 'Q' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_zeq = ( *q != '0' );
			VLOG( "SHD", "profondeur du decor (zeq) : %s",
			      NxVita::g_vita_zeq ? "LEQUAL (XBox)" : "LESS (ancien)" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " sma 0/1 " : test alpha seulement la ou il peut retirer un pixel.
		else if(( p[0] == 's' || p[0] == 'S' ) && ( p[1] == 'm' || p[1] == 'M' )
		        && ( p[2] == 'a' || p[2] == 'A' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_seuil_malin = ( *q != '0' );
			++NxVita::g_vita_gen_pre;
			VLOG( "PROF", "test alpha utile seulement : %s", NxVita::g_vita_seuil_malin ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " lmt 0/1 " : listes de dessin construites sur un autre coeur.
		else if(( p[0] == 'l' || p[0] == 'L' ) && ( p[1] == 'm' || p[1] == 'M' )
		        && ( p[2] == 't' || p[2] == 'T' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_listes_mt = ( *q != '0' );
			VLOG( "PROF", "listes sur un autre coeur : %s", NxVita::g_vita_listes_mt ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " cnd 0/1 " : listes de dessin depuis les seuls candidats (issue #18).
		else if(( p[0] == 'c' || p[0] == 'C' ) && ( p[1] == 'n' || p[1] == 'N' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_cand_listes = ( *q != '0' );
			VLOG( "PROF", "listes par candidats : %s", NxVita::g_vita_cand_listes ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gxs 0/1 " : maillages translucides seuls en GXM direct (issue #18).
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'x' || p[1] == 'X' )
		        && ( p[2] == 's' || p[2] == 'S' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_gxm_seuls = ( *q != '0' );
			VLOG( "PROF", "translucides seuls en GXM : %s", NxVita::g_vita_gxm_seuls ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gxt 0/1 " : translucides restants (une couche, passe triee) en GXM
		// direct (issue #18). Defaut 0.
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'x' || p[1] == 'X' )
		        && ( p[2] == 't' || p[2] == 'T' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_gxm_transp = ( *q != '0' );
			VLOG( "PROF", "translucides restants en GXM (gxt) : %s", NxVita::g_vita_gxm_transp ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " afu 0/1 " (issue #72) : alpha FIXE des materiaux *_FIXED a une
		// couche dans le chemin gxt, comme XBox. Defaut 1 ; 0 = ancien rendu.
		else if(( p[0] == 'a' || p[0] == 'A' ) && ( p[1] == 'f' || p[1] == 'F' )
		        && ( p[2] == 'u' || p[2] == 'U' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_alpha_fixe_une = ( *q != '0' );
			VLOG( "SHD", "alpha fixe des *_FIXED a une couche (afu, #72) : %s",
			      NxVita::g_vita_alpha_fixe_une ? "OUI (XBox)" : "non (ancien)" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " plc 0/1 " : cache du parcours des plages des lots (issue #18).
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'l' || p[1] == 'L' )
		        && ( p[2] == 'c' || p[2] == 'C' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_cache_plages = ( *q != '0' );
			VLOG( "PROF", "cache des plages : %s", NxVita::g_vita_cache_plages ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gpk 0/1 " : personnages skinnes en GXM direct (issue #18).
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'p' || p[1] == 'P' )
		        && ( p[2] == 'k' || p[2] == 'K' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_gxm_peau = ( *q != '0' );
			VLOG( "PROF", "peau en GXM direct : %s", NxVita::g_vita_gxm_peau ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " puc 0/1 " : uniformes de sommets de la peau eclairee poses
		// seulement s'ils changent (issue #18, p_shader_decor.cpp).
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'u' || p[1] == 'U' )
		        && ( p[2] == 'c' || p[2] == 'C' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_peau_uc = ( *q != '0' );
			VLOG( "PROF", "uniformes de peau en cache (puc) : %s", NxVita::g_vita_peau_uc ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " vhs 0/1 " : variantes de shader materiau cherchees par table de
		// hachage (issue #69, p_shader_decor.cpp) ; 0 = parcours lineaire.
		else if(( p[0] == 'v' || p[0] == 'V' ) && ( p[1] == 'h' || p[1] == 'H' )
		        && ( p[2] == 's' || p[2] == 'S' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_var_hash = ( *q != '0' );
			VLOG( "PROF", "variantes par hachage (vhs) : %s", NxVita::g_vita_var_hash ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " pmd 0/1 " : decoupe du poste monde, journal [PROF] monde detail
		// toutes les 120 images (issue #69, p_world_render.cpp).
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'm' || p[1] == 'M' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_prof_monde = ( *q != '0' );
			VLOG( "PROF", "decoupe du poste monde (pmd) : %s", NxVita::g_vita_prof_monde ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " zbl 0/1 " : lots a z-bias (issue #69, p_world_render.cpp) ; 0 =
		// decalques dessines un par un, comme avant. Trois lettres : aucune
		// autre commande ne commence par "zb" hormis "zbi" (z-bias lui-meme).
		else if(( p[0] == 'z' || p[0] == 'Z' ) && ( p[1] == 'b' || p[1] == 'B' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_zbl = ( *q != '0' );
			VLOG( "PROF", "lots a z-bias (zbl) : %s", NxVita::g_vita_zbl ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " bbl 0/1 " : lots de billboards (issue #69, p_world_render.cpp) ; 0 =
		// billboards dessines un par un, comme avant. Aucune commande a deux
		// lettres "bb" ; "bbd" (billboards eux-memes) compare ses trois lettres.
		else if(( p[0] == 'b' || p[0] == 'B' ) && ( p[1] == 'b' || p[1] == 'B' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_bbl = ( *q != '0' );
			VLOG( "PROF", "lots de billboards (bbl) : %s", NxVita::g_vita_bbl ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gyr 0/1 " : UV wibble des modeles -- gyrophares de police, halos
		// de phares (issue #77, p_NxModel.cpp). Aucune commande "g" ni "gy".
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'y' || p[1] == 'Y' )
		        && ( p[2] == 'r' || p[2] == 'R' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_gyr = ( *q != '0' );
			VLOG( "PROF", "UV wibble des modeles (gyr) : %s", Nx::g_vita_gyr ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " pad 0/1 " : passe 1 additive des pieces rigides opaques, eclat des
		// gyrophares (issue #77, p_NxModel.cpp). Aucune commande "pa".
		else if(( p[0] == 'p' || p[0] == 'P' ) && ( p[1] == 'a' || p[1] == 'A' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_pad = ( *q != '0' );
			VLOG( "PROF", "passe 1 additive des modeles rigides (pad) : %s", Nx::g_vita_pad ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " mro 0/1 " : vidange des translucides rigides des modeles sans etats
		// redondants (issue #69, p_NxModel.cpp). Aucune commande "mr" ; "mt" et
		// "mp" (deux lettres) ne la captent pas.
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 'r' || p[1] == 'R' )
		        && ( p[2] == 'o' || p[2] == 'O' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_mro = ( *q != '0' );
			VLOG( "PROF", "translucides rigides sans etats redondants (mro) : %s", Nx::g_vita_mro ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " vpr 0/1 " : programme de sommets du chemin GXM non repose quand il
		// est le meme d'une variante a l'autre (issue #69, p_shader_decor.cpp).
		else if(( p[0] == 'v' || p[0] == 'V' ) && ( p[1] == 'p' || p[1] == 'P' )
		        && ( p[2] == 'r' || p[2] == 'R' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_vpr = ( *q != '0' );
			VLOG( "PROF", "reposes du programme de sommets sautees (vpr) : %s", NxVita::g_vita_vpr ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " vis 0/1 " : test de visibilite reel du moteur, modeles hors champ
		// inactifs (issue #18, p_nx.cpp s_plat_is_visible).
		else if(( p[0] == 'v' || p[0] == 'V' ) && ( p[1] == 'i' || p[1] == 'I' )
		        && ( p[2] == 's' || p[2] == 'S' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_vis = ( *q != '0' );
			VLOG( "PROF", "visibilite moteur (vis) : %s", Nx::g_vita_vis ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " mdl 0/1 " : dessin des modeles (mesure, issue #18).
		else if(( p[0] == 'm' || p[0] == 'M' ) && ( p[1] == 'd' || p[1] == 'D' )
		        && ( p[2] == 'l' || p[2] == 'L' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Nx::g_vita_dessin_modeles = ( *q != '0' );
			VLOG( "PROF", "dessin des modeles : %s", Nx::g_vita_dessin_modeles ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " cpt 0/1 " : classement des composants par cout (issue #18).
		else if(( p[0] == 'c' || p[0] == 'C' ) && ( p[1] == 'p' || p[1] == 'P' )
		        && ( p[2] == 't' || p[2] == 'T' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			Obj::g_vita_prof_composants = ( *q != '0' );
			VLOG( "PROF", "classement des composants : %s", Obj::g_vita_prof_composants ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gxp 0/1 " : dessins GXM precalcules pour les lots entiers (issue #18).
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'x' || p[1] == 'X' )
		        && ( p[2] == 'p' || p[2] == 'P' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_gxm_pre = ( *q != '0' );
			VLOG( "GXD", "dessins precalcules : %s", NxVita::g_vita_gxm_pre ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " gxd 0/1 " : lots opaques par le chemin GXM direct (issue #18).
		else if(( p[0] == 'g' || p[0] == 'G' ) && ( p[1] == 'x' || p[1] == 'X' )
		        && ( p[2] == 'd' || p[2] == 'D' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_gxm_direct = ( *q != '0' );
			VLOG( "GXD", "chemin GXM direct : %s", NxVita::g_vita_gxm_direct ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " clk ARM GPU " : horloges a chaud, pour la mesure A/B (issue #18).
		// Ex. « clk 333 111 » (defaut Sony) ou « clk 444 222 » (reglage du jeu).
		else if(( p[0] == 'c' || p[0] == 'C' ) && ( p[1] == 'l' || p[1] == 'L' )
		        && ( p[2] == 'k' || p[2] == 'K' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			const int arm = atoi( q );
			while( *q && *q != ' ' ) ++q;
			while( *q == ' ' ) ++q;
			const int gpu = atoi( q );
			if( arm > 0 )
				scePowerSetArmClockFrequency( arm );
			if( gpu > 0 )
			{
				scePowerSetGpuClockFrequency( gpu );
				scePowerSetGpuXbarClockFrequency( gpu >= 222 ? 166 : 111 );
			}
			VLOG( "SYS", "horloges : ARM=%d GPU=%d XBAR=%d",
			      scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(),
			      scePowerGetGpuXbarClockFrequency());
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " ccp 0/1 " : culling sur tableau compact (issue #18).
		else if(( p[0] == 'c' || p[0] == 'C' ) && ( p[1] == 'c' || p[1] == 'C' )
		        && ( p[2] == 'p' || p[2] == 'P' ))
		{
			const char *q = p + 3;
			while( *q == ' ' || *q == '=' )
				++q;
			NxVita::g_vita_cull_compact = ( *q != '0' );
			VLOG( "SCN", "culling compact : %s", NxVita::g_vita_cull_compact ? "OUI" : "non" );
			while( *q && *q != ' ' && *q != '\n' && *q != '\r' && *q != '\t' )
				++q;
			p = (char *)q;
			continue;
		}
		// " spr " : liste complete des sprites de la prochaine image.
		else if(( p[0] == 's' || p[0] == 'S' ) && ( p[1] == 'p' || p[1] == 'P' )
		        && ( p[2] == 'r' || p[2] == 'R' ))
		{
			NxVita::g_vita_sprite_dump = true;
			VLOG( "SPR", "liste des sprites demandee" );
		}
		else
		{
			unsigned int bit = nom_vers_bouton( p );
			if( bit )
				s_inject_seq[s_inject_count++] = bit;
		}
		while( *p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' )
			++p;
	}

	if( s_inject_count )
		VLOG( "PAD", "injection : %d touches a rejouer", s_inject_count );
	else
	{
		s_inject_count  = old_count;
		s_inject_index  = old_index;
		s_inject_start  = old_start;
		s_inject_frames = old_frames;
		s_inject_held   = old_held;
	}
}

// Rend les bits a forcer cette frame, 0 si rien en cours.
static unsigned int injection_bits( void )
{
	if( s_inject_index >= s_inject_count )
	{
		// Sequence terminee : c'est MAINTENANT qu'on capture, une fois les
		// touches rejouees et le moteur stabilise.
		if( s_inject_shot )
		{
			static int s_wait = 0;
			if( ++s_wait >= 30 )
			{
				s_wait = 0;
				s_inject_shot = false;
				NxVita::RequestScreenshot();
			}
			return 0;
		}

		// --- DEMARRAGE AUTOMATIQUE ---------------------------------------
		//
		// Ce qui coute le plus cher dans une boucle de test n'est pas la
		// compilation, c'est de RETRAVERSER les menus a la main a chaque essai.
		// On rejoue donc une sequence enregistree une fois pour toutes.
		//
		// Format de ux0:data/thug/autostart.txt :
		//     <secondes d'attente> <touches...>
		// exemple :  22 croix bas bas croix
		//
		// L'attente est indispensable : injecter pendant un chargement ne mene
		// nulle part, les touches sont consommees par un ecran qui n'ecoute pas.
		{
#ifndef THUG_RELEASE	/* demarrage auto : outil de dev */
			static bool  s_auto_fait  = false;
			static float s_auto_delai = -1.0f;
			static char  s_auto_seq[256];

			if( !s_auto_fait )
			{
				if( s_auto_delai < 0.0f )
				{
					s_auto_delai = 0.0f;			/* lu une seule fois */
					SceUID fd = sceIoOpen( "ux0:data/thug/autostart.txt",
					                       SCE_O_RDONLY, 0777 );
					if( fd >= 0 )
					{
						char buf[256];
						int n = sceIoRead( fd, buf, sizeof( buf ) - 1 );
						sceIoClose( fd );
						if( n > 0 )
						{
							buf[n] = 0;
							s_auto_delai = (float)atof( buf );
							const char *q = buf;
							while( *q && *q != ' ' && *q != '\t' )
								++q;
							strncpy( s_auto_seq, q, sizeof( s_auto_seq ) - 1 );
							s_auto_seq[sizeof( s_auto_seq ) - 1] = 0;
							VLOG( "PAD", "demarrage auto dans %.1f s :%s",
							      s_auto_delai, s_auto_seq );
						}
					}
				}

				if(( s_auto_delai > 0.0f )
				   && ( sceKernelGetProcessTimeWide()
				        > (SceUInt64)( s_auto_delai * 1000000.0f )))
				{
					s_auto_fait = true;
					/* Meme chemin que l'injection normale : on ecrit le fichier,
					   il est relu au sondage suivant. */
					SceUID fd = sceIoOpen( INJECT_PATH,
					                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
					if( fd >= 0 )
					{
						sceIoWrite( fd, s_auto_seq, strlen( s_auto_seq ));
						sceIoClose( fd );
						VLOG( "PAD", "demarrage auto : sequence lancee" );
					}
				}
			}
#endif
		}

		// Build public : pas de commandes par fichier. Tout le sondage est sous
		// le #ifndef : avec le seul appel dedans, le if gardait le return 0 et
		// 29 images sur 30 tombaient dans la lecture d'une sequence vide, index
		// croissant hors du tableau -> touches fantomes au bout de ~10 min.
#ifndef THUG_RELEASE
		static int s_poll = 0;
		if(( ++s_poll % 30 ) == 0 )
			injection_relire();
#endif
		return 0;
	}

	const SceUInt64 maintenant = sceKernelGetProcessTimeWide();
	if( s_inject_start == 0 )
	{
		s_inject_start  = maintenant;
		s_inject_frames = 0;
		s_inject_held   = true;
	}

	const SceUInt64 ecoule = maintenant - s_inject_start;
	const SceUInt64 limite = s_inject_held ? INJECT_HOLD_US : INJECT_GAP_US;

	unsigned int bits = s_inject_held ? s_inject_seq[s_inject_index] : 0;
	++s_inject_frames;

	// Deux conditions, et les DEUX doivent etre remplies : assez de temps pour
	// que le front-end distingue les appuis, assez de frames pour qu'il les
	// echantillonne.
	if(( ecoule >= limite ) && ( s_inject_frames >= INJECT_MIN_FRAMES ))
	{
		s_inject_start  = maintenant;
		s_inject_frames = 0;
		if( s_inject_held )
		{
			// Fin de l'appui : on passe au relachement, meme touche.
			s_inject_held = false;
		}
		else
		{
			// Fin du relachement : touche suivante.
			s_inject_held = true;
			++s_inject_index;
			s_inject_start = 0;		// re-amorce proprement la suivante
		}
	}
	return bits;
}


Device::Device( int index, int port, int slot )
{
	m_node = new Lst::Node< Device >( this );

	memset( &m_data, 0, sizeof( m_data ));
	m_data.m_port  = port;
	m_data.m_slot  = slot;
	m_data.m_valid = false;
	m_data.m_type  = vANALOG_CTRL;

	m_index      = index;
	m_state      = vIDLE;
	m_next_state = vIDLE;
	m_plugged_in = false;

	m_unplugged_counter  = 0;
	m_unplugged_retry    = 0;
	m_pressed            = false;
	m_start_or_a_pressed = false;

	// Mode analogique large : sans cet appel, les sticks restent centres et
	// le skater ne bougera jamais. C'est le piege classique de sceCtrl.
	sceCtrlSetSamplingMode( SCE_CTRL_MODE_ANALOG_WIDE );

	// Reglages du port (#31) : une seule lecture, quel que soit le nombre de
	// Device crees (un par port/slot).
	static bool s_reglages_lus = false;
	if( !s_reglages_lus )
	{
		s_reglages_lus = true;
		reglages_lire();
	}
}


Device::~Device()
{
	delete m_node;
}


void Device::Acquire( void )
{
	m_state = vACQUIRED;
	query_capabilities();
}


void Device::Unacquire( void )
{
	m_state      = vIDLE;
	m_next_state = vIDLE;
	m_data.m_valid = false;
}


int Device::get_status( void )
{
	return vREADY;
}


void Device::query_capabilities( void )
{
	m_data.m_type = vANALOG_CTRL;
	m_data.m_caps.SetMask( mANALOG_BUTTONS );

	// La Vita ne vibre pas : zero actionneur. Annoncer le contraire
	// entrainerait le moteur dans un protocole qu'on ne peut pas honorer.
	m_data.m_num_actuators = 0;

	m_state      = vACQUIRED;
	m_next_state = vIDLE;
}


void Device::wait( void )
{
	m_state      = m_next_state;
	m_next_state = vIDLE;
}


void Device::acquisition_pending( void )
{
	m_state = vACQUIRED;
}


void Device::process( void )
{
	switch( m_state )
	{
		case vBUSY:			wait();					break;
		case vACQUIRING:	acquisition_pending();	break;
		case vACQUIRED:		read_data();			break;
		default:									break;
	}
}


void Device::read_data( void )
{
	// Un seul port physique sur Vita : les autres manettes n'existent pas.
	if( m_data.m_port != 0 )
	{
		m_data.m_valid = false;
		m_plugged_in   = false;
		return;
	}

	SceCtrlData pad;
	memset( &pad, 0, sizeof( pad ));
	if( sceCtrlPeekBufferPositive( 0, &pad, 1 ) < 0 )
	{
		m_data.m_valid = false;
		m_plugged_in   = false;
		return;
	}

	// Inversion des axes choisie par le joueur (#31). Sur le MATERIEL
	// seulement, avant l'injection : "hold ly=-1" de vctl.py reste absolu.
	if( s_inv_lx ) pad.lx = inverser_axe( pad.lx );
	if( s_inv_ly ) pad.ly = inverser_axe( pad.ly );
	if( s_inv_rx ) pad.rx = inverser_axe( pad.rx );
	if( s_inv_ry ) pad.ry = inverser_axe( pad.ry );

	if( !g_vita_tactile )
		pad.buttons |= pave_arriere();
	const int tac = g_vita_tactile ? ecran_coins() : 0;

	// Touches injectees a distance : elles s'ajoutent a la manette reelle.
	const unsigned int hw  = pad.buttons;

	// --- RACCOURCIS DE DEBOGAGE -----------------------------------------
	//
	// GAUCHE tenu + une touche. Passer par un fichier depose en FTP pour des
	// questions aussi immediates que « combien de fps ? » ou « est-ce
	// l'eclairage ? » n'avait pas de sens : la manette est deja dans les mains.
	//
	// Deux precautions :
	//   - on agit sur le FRONT de la combinaison, pas sur son maintien, sinon
	//     le reglage basculerait a chaque image ;
	//   - on lit le materiel BRUT (hw) et non l'etat mele a l'injection, pour
	//     qu'une sequence rejouee ne declenche pas un raccourci.
#ifndef THUG_RELEASE	// build public : ni raccourcis de debug ni triches
	{
		static unsigned int s_avant = 0;
		const unsigned int  appui   = hw & ~s_avant;		/* fronts */

		if( hw & SCE_CTRL_LEFT )
		{
			if( appui & SCE_CTRL_SQUARE )
			{
				Nx::g_vita_lighting = !Nx::g_vita_lighting;
				VLOG( "DBG", "eclairage : %s",
				      Nx::g_vita_lighting ? "actif" : "coupe" );
			}
			if( appui & SCE_CTRL_TRIANGLE )
			{
				NxVita::g_vita_sky = !NxVita::g_vita_sky;
				VLOG( "DBG", "ciel : %s",
				      NxVita::g_vita_sky ? "dessine" : "coupe" );
			}
			if( appui & SCE_CTRL_CIRCLE )
			{
				NxVita::g_vita_cull = !NxVita::g_vita_cull;
				VLOG( "DBG", "culling : %s",
				      NxVita::g_vita_cull ? "actif" : "coupe" );
			}
			// PAS DE RACCOURCI SUR L1 / R1.
			//
			// Ces deux gachettes servent a descendre du skate : y poser le
			// reglage de zoom genait une action de jeu frequente. La commande
			// texte « zoom N.NN » (press.py) reste disponible et suffit
			// largement -- c'est un reglage qu'on touche une fois par
			// enquete, pas une fois par minute.
			if( appui & SCE_CTRL_START )
			{
				VitaForceEndRun();			/* pour les modes qui l'ecoutent */
				VitaRequestFrontend();		/* consomme par la boucle principale */
				VLOG( "DBG", "retour au menu demande" );
			}
			if( appui & SCE_CTRL_CROSS )
			{
				NxVita::g_vita_hud = !NxVita::g_vita_hud;
				VLOG( "DBG", "compteurs : %s",
				      NxVita::g_vita_hud ? "affiches" : "caches" );
			}
		}

		// --- CAPTURE D'ECRAN : SELECT SEUL --------------------------------
		//
		// Elle etait sur GAUCHE + CROIX. Une capture se demande en pleine
		// action -- au moment precis ou l'on voit le defaut -- et tenir une
		// direction pour la declencher fausse justement ce qu'on photographie :
		// le skater tourne, la camera suit, la scene n'est plus celle qu'on
		// voulait montrer.
		//
		// SELECT est libre : aucun trick du jeu ne le nomme (verifie sur
		// Scripts/game/skater/). On ne le CONSOMME pas pour autant -- le bit
		// continue vers le moteur, au cas ou un menu l'attende.
		// RACCOURCIS DE TEST DU STORY MODE (#51), a la manette de l'humain :
		// SELECT + TRIANGLE gagne la mission en cours, SELECT + CARRE passe a
		// l'etape suivante. Scripts de triche du jeu (gamemenu.qb).
		ecran_raccourcis();
		if(( hw & SCE_CTRL_SELECT ) && ( appui & ( SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE )))
		{
			const char *p_scr = ( appui & SCE_CTRL_TRIANGLE ) ? "cheats_menu_beat_current_goal"
			                                                   : "cheats_menu_advance_stage";
			Script::CStruct *p_params = new Script::CStruct;
			Script::SpawnScript( Script::GenerateCRC( p_scr ), p_params, NO_NAME, NULL, -1, 0, false, true, true );
			delete p_params;
			VLOG( "DBG", "RACCOURCI manette : %s", p_scr );
		}
		else if( appui & SCE_CTRL_SELECT )
		{
			NxVita::RequestScreenshot();
			VLOG( "DBG", "capture demandee (SELECT)" );
		}
		s_avant = hw;
	}
#endif
	// Serveur de debug TCP : commandes texte (meme analyseur qu'inject.txt),
	// puis maintien de boutons ET de sticks (« hold »), que le fichier ne
	// savait pas exprimer.
	{
		char cmd[512];
		while( vita_dbgsrv_take_command( cmd, sizeof( cmd )))
			injection_analyser( cmd );
	}
	const unsigned int inj = injection_bits();
	pad.buttons |= inj;
	vita_dbgsrv_set_busy(( s_inject_index < s_inject_count ) || s_inject_shot );
	{
		VitaDbgPad force;
		if( vita_dbgsrv_pad( &force ))
		{
			pad.buttons |= force.buttons;
			if( force.has_sticks )
			{
				pad.lx = force.lx; pad.ly = force.ly;
				pad.rx = force.rx; pad.ry = force.ry;
			}
		}
	}

	// Des entrees FANTOMES ont ete observees : 412 evenements de direction
	// apres la fin d'une sequence injectee, alors que le sondage periodique
	// (une frame sur 300) ne montrait rien. Cette trace-ci ne sort qu'aux
	// CHANGEMENTS d'etat -- muette au repos, donc sans coÃ»t -- et separe le
	// materiel de l'injection, ce que la trace periodique confondait.
	{
		static unsigned int s_prev_hw  = 0;
		static unsigned int s_prev_inj = 0;
		if(( hw != s_prev_hw ) || ( inj != s_prev_inj ))
		{
			s_prev_hw  = hw;
			s_prev_inj = inj;
			VLOG( "PAD", "changement : materiel=0x%08x injection=0x%08x lx=%d ly=%d",
			      hw, inj, (int)pad.lx, (int)pad.ly );
		}
	}

	m_data.m_valid = true;
	m_plugged_in   = true;

	m_data.m_control_data[0] = 0;					// donnees valides
	m_data.m_control_data[1] = ( 0x07 << 4 ) | 16;	// DUALSHOCK2 + longueur

	// Actif a zero : on part tout relache, puis on efface les bits presses.
	unsigned char b2 = 0xFF;
	unsigned char b3 = 0xFF;

	if( pad.buttons & SCE_CTRL_SELECT )		b2 &= ~( 1 << 0 );
	if( pad.buttons & SCE_CTRL_START )		b2 &= ~( 1 << 3 );
	if( pad.buttons & SCE_CTRL_UP )			b2 &= ~( 1 << 4 );
	if( pad.buttons & SCE_CTRL_RIGHT )		b2 &= ~( 1 << 5 );
	if( pad.buttons & SCE_CTRL_DOWN )		b2 &= ~( 1 << 6 );
	if( pad.buttons & SCE_CTRL_LEFT )		b2 &= ~( 1 << 7 );

	// GACHETTES INVERSEES (#59, #60, decision de l'humain) : sur la Vita, les
	// gachettes L/R portent L2/R2 (nollie, revert/spine transfer -- les gestes
	// qui comptent ; la descente de planche est L1+R1, voir #76) et le pave arriere L1/R1 (rotations).
	// Avant : L1/R1 aux gachettes, et les doigts poses au dos de la console
	// envoyaient L2+R2 = descente de planche intempestive. "inv 0/1".
	{
		const bool pg = ( pad.buttons & SCE_CTRL_LTRIGGER ) != 0, pd = ( pad.buttons & SCE_CTRL_RTRIGGER ) != 0;
		const bool ag = ( pad.buttons & VITA_PAD_L2 ) != 0,       ad = ( pad.buttons & VITA_PAD_R2 ) != 0;
		const bool l2 = g_vita_inv_gachettes ? pg : ag, r2 = g_vita_inv_gachettes ? pd : ad;
		const bool l1 = g_vita_inv_gachettes ? ag : pg, r1 = g_vita_inv_gachettes ? ad : pd;
		// #76 : les deux coins = L1+R1 (descente).
		if( l2 ) b3 &= ~( 1 << 0 );
		if( r2 ) b3 &= ~( 1 << 1 );
		if( l1 || ( tac & ( TAC_L1 | TAC_DESCENTE ))) b3 &= ~( 1 << 2 );
		if( r1 || ( tac & ( TAC_R1 | TAC_DESCENTE ))) b3 &= ~( 1 << 3 );
	}
	if( pad.buttons & SCE_CTRL_TRIANGLE )	b3 &= ~( 1 << 4 );
	if( pad.buttons & SCE_CTRL_CIRCLE )		b3 &= ~( 1 << 5 );
	if( pad.buttons & SCE_CTRL_CROSS )		b3 &= ~( 1 << 6 );
	if( pad.buttons & SCE_CTRL_SQUARE )		b3 &= ~( 1 << 7 );

	m_data.m_control_data[2] = b2;
	m_data.m_control_data[3] = b3;

	// Sticks. sceCtrl rend deja 0..255 avec 128 au centre, comme la PS2.
	//
	// ZONE MORTE indispensable : au repos, cette console lit 127/123 et non
	// 128/128. Le moteur peut ECRASER les evenements analogiques deduits de la
	// croix directionnelle par ceux du stick (OverrideAnalogPadWithStick,
	// inpserv.cpp:246). Un stick legerement decentre efface donc en silence
	// les appuis sur la croix -- ce qui ressemble exactement a Â« la navigation
	// ne repond pas Â».
	m_data.m_control_data[4] = zone_morte( pad.rx );
	m_data.m_control_data[5] = zone_morte( pad.ry );
	m_data.m_control_data[6] = zone_morte( pad.lx );
	m_data.m_control_data[7] = zone_morte( pad.ly );

	// Pressions analogiques et croix directionnelle analogique : non gerees.
	for( int i = 8; i <= 20; ++i )
		m_data.m_control_data[i] = 0;

	// Croix directionnelle en pression, attendue par certains menus.
	if( pad.buttons & SCE_CTRL_RIGHT )	m_data.m_control_data[8]  = 0xFF;
	if( pad.buttons & SCE_CTRL_LEFT )	m_data.m_control_data[9]  = 0xFF;
	if( pad.buttons & SCE_CTRL_UP )		m_data.m_control_data[10] = 0xFF;
	if( pad.buttons & SCE_CTRL_DOWN )	m_data.m_control_data[11] = 0xFF;

	uint32 pressed = 0xFFFF ^ (( m_data.m_control_data[2] << 8 )
	                          | m_data.m_control_data[3] );
	m_pressed = ( pressed != 0 );

	m_start_or_a_pressed = ( pad.buttons & ( SCE_CTRL_START | SCE_CTRL_CROSS )) != 0;

	if( m_pressed )
		gLastPadPressed = m_index;

	// Deux traces, pour deux questions distinctes :
	//
	//  - le battement prouve que read_data TOURNE et que sceCtrl repond. Il
	//    sort meme si personne n'appuie -- indispensable quand la console est
	//    testee a distance, sans humain devant.
	//  - la premiere pression prouve qu'une touche remonte reellement jusqu'au
	//    moteur. Celle-la, seul un humain peut la declencher.
	{
		static int s_polls = 0;
		if(( ++s_polls % 300 ) == 1 )
			VLOG( "PAD", "sondage %d : boutons=0x%08x lx=%d ly=%d rx=%d ry=%d",
			      s_polls, (unsigned)pad.buttons,
			      (int)pad.lx, (int)pad.ly, (int)pad.rx, (int)pad.ry );
	}

	static bool s_first = true;
	if( m_pressed && s_first )
	{
		s_first = false;
		VLOG( "PAD", "*** PREMIERE PRESSION *** boutons=0x%08x", (unsigned)pad.buttons );
	}
}


bool Device::IsPluggedIn( void )
{
	return m_plugged_in;
}


// --- vibration : la Vita n'en a pas -----------------------------------------

void Device::ActivateActuator( int act_num, int percent )				{}
void Device::ActivatePressureSensitiveMode( void )						{}
void Device::DeactivatePressureSensitiveMode( void )					{}
void Device::Pause( void )												{}
void Device::UnPause( void )											{}
void Device::DisableActuators( void )									{}
void Device::EnableActuators( void )									{}
void Device::ResetActuators( void )										{}
void Device::StopAllVibrationIncludingSaved( void )						{}

} // namespace SIO
