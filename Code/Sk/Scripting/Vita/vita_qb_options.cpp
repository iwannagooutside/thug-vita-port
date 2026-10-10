/*****************************************************************************
**  THUG-Vita -- menu VITA OPTIONS (reglages propres au port)               **
**  Code/Sk/Scripting/Vita/vita_qb_options.cpp                              **
**                                                                          **
**  Deux moities :                                                          **
**                                                                          **
**  1. VitaChargerQBOptions : charge vita/qb/vita_options.q, assemble au    **
**     build par vita/tools/qbasm.py en tableau d'octets (vita_options_qb.h **
**     genere dans le dossier de build), APRES les .qb de qb.prx            **
**     (gs_file.cpp, fin de LoadAllStartupQBFiles). LoadQBFromMemory ->     **
**     ParseQB : un symbole deja defini est remplace (parse.cpp, branche    **
**     p_existing_entry), c'est ainsi que create_controller_config_menu     **
**     recoit l'entree "Vita Options". Aucune donnee du joueur n'est        **
**     modifiee.                                                            **
**                                                                          **
**     ParseQB ne verifie le format qu'en Dbg_MsgAssert, coupes sur Vita :  **
**     un .qb mal forme planterait ou corromprait la table des symboles.    **
**     D'ou, AVANT de charger, et sans rien charger si un point echoue      **
**     (log [QB], le jeu garde ses menus d'origine) :                       **
**       - CRC32 du tableau = celui calcule a l'assemblage ;                **
**       - parcours complet des jetons (tailles de skiptoken.cpp), bornes,  **
**         fin exacte sur ENDOFFILE, script/endscript, if/endif,            **
**         switch/endswitch, begin/repeat, accolades/crochets/parentheses ; **
**       - hors script, chaque ligne est 'nom = valeur' ou 'script nom' ;   **
**       - chaque script remplace (%replaces) existe et a EXACTEMENT la     **
**         somme de contenu de la version copiee (ISO USA) : sur une autre  **
**         version du jeu, on ne remplace rien ;                            **
**       - chaque symbole du jeu appele (%needs) existe.                    **
**                                                                          **
**  2. Les C-functions appelees par ce menu, enregistrees dans              **
**     Sk/Scripting/ftables.cpp :                                           **
**       VitaOptGet name=<cle>    -> value (entier), value_text (chaine)    **
**       VitaOptToggle name=<cle> -> bascule, applique, reecrit             **
**                                   controls.txt, rend value/value_text    **
**     Cles : invert_left_x, invert_left_y, invert_right_x, invert_right_y, **
**     triggers_as_l2r2, touch_on_rear_pad, framerate. Les reglages vivent  **
**     dans Sys/SIO/Vita/p_siodev.cpp (SIO::VitaReglage*).               **
*****************************************************************************/

#include <core/defines.h>
#include <core/crc.h>
#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/symboltable.h>
#include <gel/scripting/symboltype.h>
#include <gel/scripting/tokens.h>
#include <gel/scripting/file.h>
#include <gel/scripting/checksum.h>
#include <sk/scripting/cfuncs.h>

#include <string.h>

#include "vita_log.h"

#ifndef THUG_SANS_MENU_VITA
#include "vita_options_qb.h"
#endif

// Sys/SIO/Vita/p_siodev.cpp
namespace SIO
{
int VitaReglageValeur( const char *p_cle );
int VitaReglageBasculer( const char *p_cle );
}

using namespace Script;

/*****************************************************************************
**  Verification structurelle (equivalent de qbasm.py, verify_bin et        **
**  check_structure / check_top_level)                                      **
*****************************************************************************/

static uint32 lire32( const uint8 *p )
{
	return (uint32)p[0] | ((uint32)p[1] << 8 ) | ((uint32)p[2] << 16 ) | ((uint32)p[3] << 24 );
}

// Taille du jeton en p (skiptoken.cpp), 0 si inconnu ou hors du tampon.
static uint32 taille_jeton( const uint8 *p, const uint8 *p_fin )
{
	const uint32 reste = (uint32)( p_fin - p );
	switch( *p )
	{
		case ESCRIPTTOKEN_ENDOFFILE:
		case ESCRIPTTOKEN_ENDOFLINE:
		case ESCRIPTTOKEN_EQUALS:
		case ESCRIPTTOKEN_DOT:
		case ESCRIPTTOKEN_COMMA:
		case ESCRIPTTOKEN_MINUS:
		case ESCRIPTTOKEN_ADD:
		case ESCRIPTTOKEN_DIVIDE:
		case ESCRIPTTOKEN_MULTIPLY:
		case ESCRIPTTOKEN_OPENPARENTH:
		case ESCRIPTTOKEN_CLOSEPARENTH:
		case ESCRIPTTOKEN_SAMEAS:
		case ESCRIPTTOKEN_LESSTHAN:
		case ESCRIPTTOKEN_LESSTHANEQUAL:
		case ESCRIPTTOKEN_GREATERTHAN:
		case ESCRIPTTOKEN_GREATERTHANEQUAL:
		case ESCRIPTTOKEN_STARTSTRUCT:
		case ESCRIPTTOKEN_STARTARRAY:
		case ESCRIPTTOKEN_ENDSTRUCT:
		case ESCRIPTTOKEN_ENDARRAY:
		case ESCRIPTTOKEN_KEYWORD_BEGIN:
		case ESCRIPTTOKEN_KEYWORD_REPEAT:
		case ESCRIPTTOKEN_KEYWORD_BREAK:
		case ESCRIPTTOKEN_KEYWORD_SCRIPT:
		case ESCRIPTTOKEN_KEYWORD_ENDSCRIPT:
		case ESCRIPTTOKEN_KEYWORD_IF:
		case ESCRIPTTOKEN_KEYWORD_ELSE:
		case ESCRIPTTOKEN_KEYWORD_ELSEIF:
		case ESCRIPTTOKEN_KEYWORD_ENDIF:
		case ESCRIPTTOKEN_KEYWORD_RETURN:
		case ESCRIPTTOKEN_KEYWORD_ALLARGS:
		case ESCRIPTTOKEN_ARG:
		case ESCRIPTTOKEN_OR:
		case ESCRIPTTOKEN_AND:
		case ESCRIPTTOKEN_XOR:
		case ESCRIPTTOKEN_SHIFT_LEFT:
		case ESCRIPTTOKEN_SHIFT_RIGHT:
		case ESCRIPTTOKEN_KEYWORD_RANDOM_RANGE:
		case ESCRIPTTOKEN_KEYWORD_RANDOM_RANGE2:
		case ESCRIPTTOKEN_KEYWORD_NOT:
		case ESCRIPTTOKEN_KEYWORD_AND:
		case ESCRIPTTOKEN_KEYWORD_OR:
		case ESCRIPTTOKEN_KEYWORD_SWITCH:
		case ESCRIPTTOKEN_KEYWORD_ENDSWITCH:
		case ESCRIPTTOKEN_KEYWORD_CASE:
		case ESCRIPTTOKEN_KEYWORD_DEFAULT:
		case ESCRIPTTOKEN_COLON:
			return 1;
		case ESCRIPTTOKEN_NAME:
		case ESCRIPTTOKEN_INTEGER:
		case ESCRIPTTOKEN_HEXINTEGER:
		case ESCRIPTTOKEN_FLOAT:
		case ESCRIPTTOKEN_ENDOFLINENUMBER:
		case ESCRIPTTOKEN_JUMP:
			return ( reste >= 5 ) ? 5 : 0;
		case ESCRIPTTOKEN_VECTOR:
			return ( reste >= 13 ) ? 13 : 0;
		case ESCRIPTTOKEN_PAIR:
			return ( reste >= 9 ) ? 9 : 0;
		case ESCRIPTTOKEN_STRING:
		case ESCRIPTTOKEN_LOCALSTRING:
		{
			if( reste < 5 )
				return 0;
			const uint32 n = lire32( p + 1 );
			// Terminateur inclus dans la longueur (AddString lit jusqu'au 0).
			if(( n == 0 ) || ( n > reste - 5 ) || ( p[4 + n] != 0 ))
				return 0;
			return 5 + n;
		}
		case ESCRIPTTOKEN_CHECKSUM_NAME:
		{
			if( reste < 6 )
				return 0;
			for( uint32 i = 5; i < reste; ++i )
				if( p[i] == 0 )
					return i + 1;
			return 0;
		}
		case ESCRIPTTOKEN_KEYWORD_RANDOM:
		case ESCRIPTTOKEN_KEYWORD_RANDOM2:
		case ESCRIPTTOKEN_KEYWORD_RANDOM_NO_REPEAT:
		case ESCRIPTTOKEN_KEYWORD_RANDOM_PERMUTE:
		{
			if( reste < 5 )
				return 0;
			const uint32 n = lire32( p + 1 );
			if(( n == 0 ) || ( n > ( reste - 5 ) / 6 ))
				return 0;
			return 5 + 6 * n;
		}
		default:
			return 0;
	}
}

// Rend NULL si le tampon est conforme, sinon la raison.
static const char *verifier_qb( const uint8 *p_qb, uint32 taille, uint32 *p_octet )
{
	const uint8 *p = p_qb, *p_fin = p_qb + taille;
	bool dans_script = false;
	bool debut_ligne = true;		// hors script : premier jeton de la ligne
	bool attend_nom_script = false;
	bool attend_egal = false;		// hors script : 'nom' vu, '=' attendu
	bool attend_valeur = false;		// '=' vu : fins de ligne sautees (SkipEndOfLines)
	int  prof_accolade = 0, prof_crochet = 0, prof_paren = 0;
	int  pile[64];					// 0 if, 1 switch, 2 begin
	int  n_pile = 0;

	while( p < p_fin )
	{
		*p_octet = (uint32)( p - p_qb );
		const uint8 t = *p;
		const uint32 n = taille_jeton( p, p_fin );
		if( !n )
			return "jeton inconnu ou tronque";
		if( attend_valeur && ( t != ESCRIPTTOKEN_ENDOFLINE ) && ( t != ESCRIPTTOKEN_ENDOFFILE ))
		{
			attend_valeur = false;	// premier jeton de la valeur
			debut_ligne = false;
		}
		if( t == ESCRIPTTOKEN_ENDOFFILE )
		{
			if( p + 1 != p_fin )
				return "octets apres ENDOFFILE";
			if( dans_script )
				return "script non termine";
			if( prof_accolade || prof_crochet || prof_paren )
				return "accolades/crochets/parentheses non equilibres";
			if( attend_egal || attend_valeur || attend_nom_script )
				return "definition incomplete en fin de fichier";
			return NULL;
		}

		switch( t )
		{
			case ESCRIPTTOKEN_STARTSTRUCT:	++prof_accolade; break;
			case ESCRIPTTOKEN_ENDSTRUCT:	--prof_accolade; break;
			case ESCRIPTTOKEN_STARTARRAY:	++prof_crochet;  break;
			case ESCRIPTTOKEN_ENDARRAY:		--prof_crochet;  break;
			case ESCRIPTTOKEN_OPENPARENTH:	++prof_paren;    break;
			case ESCRIPTTOKEN_CLOSEPARENTH:	--prof_paren;    break;
			default: break;
		}
		if(( prof_accolade < 0 ) || ( prof_crochet < 0 ) || ( prof_paren < 0 ))
			return "fermeture sans ouverture";

		if( dans_script )
		{
			switch( t )
			{
				case ESCRIPTTOKEN_KEYWORD_SCRIPT:
					return "script dans un script";
				case ESCRIPTTOKEN_CHECKSUM_NAME:
					return "table de noms dans un script";
				case ESCRIPTTOKEN_KEYWORD_IF:
				case ESCRIPTTOKEN_KEYWORD_SWITCH:
				case ESCRIPTTOKEN_KEYWORD_BEGIN:
					if( n_pile >= 64 )
						return "imbrication trop profonde";
					pile[n_pile++] = ( t == ESCRIPTTOKEN_KEYWORD_IF ) ? 0 :
					                 ( t == ESCRIPTTOKEN_KEYWORD_SWITCH ) ? 1 : 2;
					break;
				case ESCRIPTTOKEN_KEYWORD_ELSE:
				case ESCRIPTTOKEN_KEYWORD_ELSEIF:
					if( !n_pile || pile[n_pile - 1] != 0 )
						return "else hors if";
					break;
				case ESCRIPTTOKEN_KEYWORD_ENDIF:
					if( !n_pile || pile[--n_pile] != 0 )
						return "endif orphelin";
					break;
				case ESCRIPTTOKEN_KEYWORD_ENDSWITCH:
					if( !n_pile || pile[--n_pile] != 1 )
						return "endswitch orphelin";
					break;
				case ESCRIPTTOKEN_KEYWORD_REPEAT:
					if( !n_pile || pile[--n_pile] != 2 )
						return "repeat orphelin";
					break;
				case ESCRIPTTOKEN_KEYWORD_ENDSCRIPT:
					if( n_pile )
						return "if/switch/begin non ferme";
					if( prof_accolade || prof_crochet || prof_paren )
						return "accolades/crochets/parentheses non equilibres";
					dans_script = false;
					debut_ligne = true;
					break;
				default:
					break;
			}
		}
		else if( attend_nom_script )
		{
			if( t != ESCRIPTTOKEN_NAME )
				return "script sans nom";
			attend_nom_script = false;
			dans_script = true;
			n_pile = 0;
		}
		else if( attend_egal )
		{
			if( t == ESCRIPTTOKEN_EQUALS )
			{
				attend_egal = false;
				attend_valeur = true;
				debut_ligne = false;
			}
			else if( t != ESCRIPTTOKEN_ENDOFLINE )
				return "commande hors script (nom sans =)";
		}
		else if( attend_valeur && ( t == ESCRIPTTOKEN_ENDOFLINE ))
			;
		else if( t == ESCRIPTTOKEN_ENDOFLINE )
		{
			if( !prof_accolade && !prof_crochet && !prof_paren )
				debut_ligne = true;
		}
		else if( debut_ligne )
		{
			if( t == ESCRIPTTOKEN_KEYWORD_SCRIPT )
				attend_nom_script = true;
			else if( t == ESCRIPTTOKEN_NAME )
				attend_egal = true;
			else if( t == ESCRIPTTOKEN_CHECKSUM_NAME )
				;
			else
				return "jeton inattendu hors script";
		}
		p += n;
	}
	return "pas de ENDOFFILE";
}

/*****************************************************************************
**  Chargement                                                              **
*****************************************************************************/

void VitaChargerQBOptions( void )
{
#ifdef THUG_SANS_MENU_VITA
	VLOG( "QB", "vita_options.qb : non embarque (python3 absent au build), menu Vita Options absent" );
#else
	const uint8 *p_qb = vita_options_qb;
	const uint32 taille = vita_options_qb_size;

	const uint32 crc = Crc::UpdateCRC((const char *)p_qb, (int)taille, 0xffffffff ) ^ 0xffffffff;
	if( crc != vita_options_qb_crc32 )
	{
		VLOG( "QB", "!! vita_options.qb : crc32 0x%08x au lieu de 0x%08x, NON charge", crc, vita_options_qb_crc32 );
		return;
	}
	uint32 octet = 0;
	const char *p_err = verifier_qb( p_qb, taille, &octet );
	if( p_err )
	{
		VLOG( "QB", "!! vita_options.qb : %s (octet %u / %u), NON charge", p_err, octet, taille );
		return;
	}
	for( const SQBRequis *p_r = vita_options_qb_requis; p_r->p_nom; ++p_r )
	{
		if( !LookUpSymbol( p_r->crc_nom ))
		{
			VLOG( "QB", "!! vita_options.qb : symbole du jeu '%s' absent, NON charge", p_r->p_nom );
			return;
		}
	}
	for( const SQBRemplace *p_r = vita_options_qb_remplace; p_r->p_nom; ++p_r )
	{
		CSymbolTableEntry *p_sym = LookUpSymbol( p_r->crc_nom );
		if( !p_sym || ( p_sym->mType != ESYMBOLTYPE_QSCRIPT ) || !p_sym->mpScript )
		{
			VLOG( "QB", "!! vita_options.qb : script '%s' absent de qb.prx, NON charge", p_r->p_nom );
			return;
		}
		// Tete de mpScript : CalculateScriptContentsChecksum de l'original
		// (parse.cpp, sCreateScriptSymbol ; avec ou sans cache de scripts).
		const uint32 somme = *(const uint32 *)p_sym->mpScript;
		if( somme != p_r->somme )
		{
			VLOG( "QB", "!! vita_options.qb : '%s' a la somme 0x%08x, la copie vise 0x%08x "
			            "(autre version du jeu ?), NON charge", p_r->p_nom, somme, p_r->somme );
			return;
		}
	}

	// LoadQBFromMemory ne garde aucun pointeur dans le tampon (scripts copies,
	// chaines copiees) et n'y ecrit pas : le tableau const suffit.
	LoadQBFromMemory( "vita_options.qb", const_cast<uint8 *>( p_qb ), NO_ASSERT_IF_DUPLICATE_SYMBOLS );

	int remplaces = 0;
	for( const SQBRemplace *p_r = vita_options_qb_remplace; p_r->p_nom; ++p_r )
	{
		CSymbolTableEntry *p_sym = LookUpSymbol( p_r->crc_nom );
		if( p_sym && ( p_sym->mType == ESYMBOLTYPE_QSCRIPT ) && p_sym->mpScript &&
		    ( *(const uint32 *)p_sym->mpScript != p_r->somme ))
			++remplaces;
	}
	VLOG( "QB", "vita_options.qb charge : %u octets, %d script(s) du jeu remplace(s), "
	            "menu %s", taille, remplaces,
	      LookUpSymbol( Crc::GenerateCRCFromString( "create_vita_options_menu" )) ? "present" : "ABSENT" );
#endif
}

/*****************************************************************************
**  C-functions                                                             **
*****************************************************************************/

static const char *const s_cles[] =
{
	"invert_left_x", "invert_left_y", "invert_right_x", "invert_right_y",
	"triggers_as_l2r2", "touch_on_rear_pad", "framerate", NULL
};

static const char *cle_depuis_params( Script::CStruct *pParams )
{
	uint32 nom = 0;
	if( !pParams || !pParams->GetChecksum( Crc::GenerateCRCFromString( "name" ), &nom ))
		return NULL;
	for( int i = 0; s_cles[i]; ++i )
		if( Crc::GenerateCRCFromString( s_cles[i] ) == nom )
			return s_cles[i];
	return NULL;
}

// Texte affiche dans le menu, minuscules comme CONTROL SETUP ("on"/"off").
static const char *texte_valeur( const char *p_cle, int v )
{
	if( strcmp( p_cle, "framerate" ) == 0 )			return ( v == 30 ) ? "30" : "60";
	if( strcmp( p_cle, "triggers_as_l2r2" ) == 0 )	return v ? "L2/R2" : "L1/R1";
	if( strcmp( p_cle, "touch_on_rear_pad" ) == 0 )	return v ? "rear" : "front";
	return v ? "on" : "off";
}

static bool rendre_valeur( Script::CScript *pScript, const char *p_cle, int v )
{
	if( v < 0 )
		return false;
	Script::CStruct *p_ret = pScript->GetParams();
	p_ret->RemoveComponent( "value" );
	p_ret->RemoveComponent( "value_text" );
	p_ret->AddInteger( "value", v );
	p_ret->AddString( "value_text", texte_valeur( p_cle, v ));
	return true;
}

namespace CFuncs
{

// VitaOptGet name=<cle> : value, value_text dans les parametres du script.
// Faux si la cle est inconnue (value_text n'est alors pas pose).
bool ScriptVitaOptGet( Script::CStruct *pParams, Script::CScript *pScript )
{
	const char *p_cle = cle_depuis_params( pParams );
	if( !p_cle )
	{
		VLOG( "QB", "VitaOptGet : cle inconnue" );
		return false;
	}
	return rendre_valeur( pScript, p_cle, SIO::VitaReglageValeur( p_cle ));
}

// VitaOptToggle name=<cle> : bascule, applique, reecrit controls.txt, puis
// comme VitaOptGet.
bool ScriptVitaOptToggle( Script::CStruct *pParams, Script::CScript *pScript )
{
	const char *p_cle = cle_depuis_params( pParams );
	if( !p_cle )
	{
		VLOG( "QB", "VitaOptToggle : cle inconnue" );
		return false;
	}
	return rendre_valeur( pScript, p_cle, SIO::VitaReglageBasculer( p_cle ));
}

} // namespace CFuncs
