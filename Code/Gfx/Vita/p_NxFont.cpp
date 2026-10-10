/*****************************************************************************
**  THUG-Vita â€” backend graphique                                            **
**  Code/Gfx/Vita/p_NxFont.cpp                                              **
**                                                                          **
**  Polices et texte 2D. Sans lui les menus sont vides : THUG affiche        **
**  presque tout son front-end en texte, et l'ecran-titre reste un decor     **
**  sans un seul element selectionnable.                                     **
**                                                                          **
**  Voir p_NxFont.h pour la disposition du .fnt.xbx.                        **
*****************************************************************************/

#include <core/defines.h>
#include <sys/file/filesys.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "vita_log.h"
#include "p_NxFont.h"
#include <gfx/NxFontMan.h>

#define SCREEN_W	960.0f
#define SCREEN_H	544.0f
// 640 x 480 avec 16 lignes de marge, comme les sprites -- voir l'explication
// detaillee en tete de p_NxSprite.cpp (DX9/NX/nx_init.cpp:453). Texte et
// sprites DOIVENT partager exactement le meme mapping, sinon les libelles
// glissent par rapport aux cadres qu'ils sont censes remplir.
#define ENGINE_2D_W			640.0f
#define ENGINE_2D_H			480.0f
#define ENGINE_2D_Y_MARGIN	16.0f

namespace NxVita { extern int g_vita_mx2; }	// p_NxSprite.cpp (#52)
namespace NxVita
{

static Nx::CVitaText **sp_texts  = NULL;
static int             s_num     = 0;
static int             s_cap     = 0;

void TextRegister( Nx::CVitaText *p )
{
	if( s_num == s_cap )
	{
		int cap = s_cap ? ( s_cap * 2 ) : 128;
		Nx::CVitaText **p_new =
			(Nx::CVitaText **)realloc( sp_texts, cap * sizeof( Nx::CVitaText * ));
		if( !p_new )
			return;
		sp_texts = p_new;
		s_cap    = cap;
	}
	sp_texts[s_num++] = p;
}

void TextUnregister( Nx::CVitaText *p )
{
	for( int i = 0; i < s_num; ++i )
	{
		if( sp_texts[i] == p )
		{
			sp_texts[i] = sp_texts[--s_num];
			return;
		}
	}
}


// --- Textes propres a la Xbox (issue #24) ------------------------------------
//
// Les scripts suivent la branche « case xbox » (Sys/Config/Vita/p_config.cpp
// pose HARDWARE_XBOX, GetPlatform rend xbox) : messages de sauvegarde
// « Saving %f to hard disk ...\nPlease do not turn off your Xbox console. »
// (memcardmessages.q), « Save trick to Hard Drive » (catmenu.q:520), manette
// debranchee (controller_pulling.q:123), « System Link Play » (menu principal).
// On ne livre pas de scripts modifies : les joueurs fournissent leur ISO. Le
// texte est donc reecrit au moment ou il entre dans un element d'ecran
// (CTextElement::SetText, CTextBlockElement::SetText) -- APRES FormatText, d'ou
// des sous-chaines plutot que des chaines entieres (%f, %s, %i deja remplaces).
//
// Les remplacements ne contiennent aucun motif de la table : appliquer la
// table deux fois (bloc puis ligne) ne change rien de plus. Sensible a la
// casse. Les credits (« Manager, Xbox Testing: ») ne sont pas touches.
// Le texte des elements n'est jamais une cle : les scripts comparent leurs
// parametres, pas l'affichage.
struct STexteVita { const char *p_avant; const char *p_apres; };

static const STexteVita s_textes_vita[] =
{
	// « Checking/Saving/Overwriting/Loading/Deleting ... hard disk ... »,
	// « No THUG %n present on hard disk. », Create-A-Skater (memcardmessages.q)
	{ "hard disk",					"memory card" },
	// « Save trick to Hard Drive » (catmenu.q:520, trick slots pleins)
	{ "Hard Drive",					"Memory Card" },
	// « Please do not turn off your Xbox console. »
	{ "your Xbox console",			"your PS Vita system" },
	// « Your Xbox does not have enough free blocks to save ... »
	{ "Your Xbox",					"Your memory card" },
	// « Unable to load %s. Press A to continue. » -> icone de l'action Go
	// (\m0 -> \b3 = croix, meta_button_map_xbox, gamemenu.q:105)
	{ "Press A to continue",		"Press \\m0 to continue" },
	// « Please reconnect the controller to port %i and press START ... »
	{ "the controller to port ",	"controller " },
	// Jeu en reseau local Xbox : menu principal, titre du skateshop,
	// fichier de reglages, messages de cable (skateshop.q:1754, netmessages.q)
	{ "System Link",				"LAN" },
	{ "SYSTEM LINK",				"LAN" },
	{ "system link",				"LAN" },
};

static const int NUM_TEXTES_VITA = sizeof( s_textes_vita ) / sizeof( s_textes_vita[0] );

// Premier motif present a partir de p, et sa position ; NULL si aucun.
static const STexteVita *motif_suivant( const char *p, const char **pp_pos )
{
	const STexteVita *p_best = NULL;
	const char *p_best_pos = NULL;
	for( int i = 0; i < NUM_TEXTES_VITA; ++i )
	{
		const char *q = strstr( p, s_textes_vita[i].p_avant );
		if( q && ( !p_best_pos || ( q < p_best_pos )))
		{
			p_best     = &s_textes_vita[i];
			p_best_pos = q;
		}
	}
	*pp_pos = p_best_pos;
	return p_best;
}

char *TexteVitaSubstitue( const char *p_in )
{
	if( !p_in || !*p_in )
		return NULL;

	const char *p_pos;
	if( !motif_suivant( p_in, &p_pos ))
		return NULL;					// cas courant : rien a faire

	// Passe 1 : longueur du resultat.
	size_t len = 0;
	const char *p = p_in;
	const STexteVita *m;
	while(( m = motif_suivant( p, &p_pos )) != NULL )
	{
		len += ( p_pos - p ) + strlen( m->p_apres );
		p = p_pos + strlen( m->p_avant );
	}
	len += strlen( p );

	char *p_out = (char *)malloc( len + 1 );
	if( !p_out )
		return NULL;

	// Passe 2 : copie.
	char *o = p_out;
	p = p_in;
	while(( m = motif_suivant( p, &p_pos )) != NULL )
	{
		memcpy( o, p, p_pos - p );				o += p_pos - p;
		size_t n = strlen( m->p_apres );
		memcpy( o, m->p_apres, n );				o += n;
		p = p_pos + strlen( m->p_avant );
	}
	strcpy( o, p );

	VLOG( "TXT", "texte Vita : \"%.80s\" -> \"%.80s\"", p_in, p_out );
	return p_out;
}

} // namespace NxVita


namespace Nx
{

// --- CVitaFont -------------------------------------------------------------

// Police des icones de manette. Le moteur la designe au chargement
// (ScriptLoadFont, flag buttons_font) ; c'est ButtonsPs2 chez nous, faute de
// __PLAT_XBOX__ (NxFontMan.cpp:146) -- et c'est tant mieux : ce sont les
// icones PlayStation.
//
// [SOURCE] XBox/NX/chars.cpp:617 et :628 : \b et \m basculent sur pButtonsFont
// pour UN caractere. La premiere version de ce fichier cherchait ces glyphes
// dans la police du TEXTE, dont la table speciale contient tout autre chose :
// d'ou des drapeaux verts a la place des icones et des "A/A = Select" a la
// place des fleches (issues #10 et #27).
static CVitaFont *sp_buttons_font = NULL;

// Str::DehexifyDigit sans son assertion : 0-9 puis a-v, soit 32 valeurs.
static int dehex( char c )
{
	if(( c >= '0' ) && ( c <= '9' ))	return c - '0';
	if(( c >= 'a' ) && ( c <= 'v' ))	return c - 'a' + 10;
	if(( c >= 'A' ) && ( c <= 'V' ))	return c - 'A' + 10;
	return -1;
}

void CVitaFont::plat_mark_as_button_font( bool isButton )
{
	// Le moteur appelle cette fonction pour CHAQUE police chargee, avec false
	// pour les polices ordinaires. La Xbox remettait alors pButtonsFont a NULL
	// (XBox/p_NxFont.cpp:98) ; on ne retient que le true, pour ne pas dependre
	// de l'ordre de chargement.
	if( isButton )
		sp_buttons_font = this;
}

const SVitaGlyph *CVitaFont::ParseTag( const char **pp, CVitaFont **pp_font ) const
{
	const char *q = *pp;
	const SVitaGlyph *g = NULL;
	*pp_font = const_cast< CVitaFont * >( this );

	switch( *q )
	{
		case '\\':
			g = GetGlyph( '\\' );
			break;

		// \cN : changement de couleur, applique par RenderText2D (qui lit le
		// chiffre avant cet appel) -- ici on saute seulement le chiffre.
		case 'c': case 'C':
			if( q[1] ) ++q;
			break;

		// \sN : glyphe special de la police COURANTE.
		case 's': case 'S':
			if( q[1] )
			{
				++q;
				g = GetSpecialGlyph( dehex( *q ));
			}
			break;

		// \bN : icone N de la police de boutons. \mN : action N, traduite en
		// icone par la table meta_button_map (gamemenu.q:105).
		case 'b': case 'B':
		case 'm': case 'M':
			if( q[1] )
			{
				const bool meta = ( *q == 'm' ) || ( *q == 'M' );
				++q;
				char d = meta ? Nx::CFontManager::sMapMetaCharacterToButton( q ) : *q;
				if( sp_buttons_font )
				{
					*pp_font = sp_buttons_font;
					g = sp_buttons_font->GetSpecialGlyph( dehex( d ));
				}
			}
			break;

		default:
			break;
	}

	*pp = q;
	return g;
}

CVitaFont::CVitaFont()
	: m_texture( 0 ), mp_glyphs( NULL ), m_num_glyphs( 0 ),
	  m_default_height( 16 ), m_default_base( 12 ),
	  m_char_spacing( 0 ), m_space_spacing( 8 )
{
	memset( m_map, 0, sizeof( m_map ));
	memset( m_special_map, 0, sizeof( m_special_map ));
	for( int i = 0; i < 16; ++i )
		m_rgba_tab[i] = Image::RGBA( 128, 128, 128, 128 );
}

// [SOURCE] XBox/p_NxFont.cpp:85 : simple copie des 16 entrees. Le moteur
// l'appelle a chaque LoadFont avec color_tab (themes.q:1042 recharge "small"
// et "dialog" avec <theme>_FONT_COLORS a chaque changement de theme).
void CVitaFont::plat_set_rgba_table( Image::RGBA *pTab )
{
	for( int i = 0; i < 16; ++i )
		m_rgba_tab[i] = pTab[i];
}

CVitaFont::~CVitaFont()
{
	plat_unload();
}

void CVitaFont::plat_unload()
{
	if( sp_buttons_font == this )
		sp_buttons_font = NULL;
	if( m_texture )
	{
		glDeleteTextures( 1, &m_texture );
		m_texture = 0;
	}
	free( mp_glyphs );
	mp_glyphs    = NULL;
	m_num_glyphs = 0;
}

void CVitaFont::plat_set_spacings( int charSpacing, int spaceSpacing )
{
	m_char_spacing  = charSpacing;
	m_space_spacing = spaceSpacing;
}

const SVitaGlyph *CVitaFont::GetGlyph( unsigned char c ) const
{
	if( !mp_glyphs )
		return NULL;
	int i = m_map[c];
	if(( i < 0 ) || ( i >= m_num_glyphs ))
		return NULL;
	return &mp_glyphs[i];
}

const SVitaGlyph *CVitaFont::GetSpecialGlyph( int index ) const
{
	if( !mp_glyphs || ( index < 0 ) || ( index >= 32 ))
		return NULL;
	int i = m_special_map[index];
	if(( i < 0 ) || ( i >= m_num_glyphs ))
		return NULL;
	return &mp_glyphs[i];
}

static void ps2_recolor_clut( unsigned char *clut )
{
	static const struct { float from, shift; } map[] = {
		{   0.0f,  -35.0f },
		{  55.0f,  165.0f },
		{  64.0f,   70.0f },
		{ 225.0f,  135.0f },
	};

	for( int i = 0; i < 256; ++i )
	{
		unsigned char *c = clut + i * 4;
		float r = c[2] / 255.0f, g = c[1] / 255.0f, b = c[0] / 255.0f;
		float mx = fmaxf( r, fmaxf( g, b )), mn = fminf( r, fminf( g, b ));
		float d = mx - mn;
		if(( mx < 0.05f ) || ( d / mx < 0.25f ))
			continue;

		float h;
		if( mx == r )		h = 60.0f * fmodf(( g - b ) / d, 6.0f );
		else if( mx == g )	h = 60.0f * (( b - r ) / d + 2.0f );
		else				h = 60.0f * (( r - g ) / d + 4.0f );
		if( h < 0.0f ) h += 360.0f;

		int best = 0; float best_d = 999.0f;
		for( int k = 0; k < 4; ++k )
		{
			float dd = fabsf( h - map[k].from );
			if( dd > 180.0f ) dd = 360.0f - dd;
			if( dd < best_d ) { best_d = dd; best = k; }
		}
		h = fmodf( h + map[best].shift + 360.0f, 360.0f );

		float s = d / mx, v = mx;
		float hh = h / 60.0f;
		int   hi = (int)hh % 6;
		float f  = hh - floorf( hh );
		float p = v * ( 1.0f - s ), q = v * ( 1.0f - s * f ), t = v * ( 1.0f - s * ( 1.0f - f ));
		float R, G, B;
		switch( hi )
		{
			case 0: R = v; G = t; B = p; break;
			case 1: R = q; G = v; B = p; break;
			case 2: R = p; G = v; B = t; break;
			case 3: R = p; G = q; B = v; break;
			case 4: R = t; G = p; B = v; break;
			default: R = v; G = p; B = q; break;
		}
		c[2] = (unsigned char)( R * 255.0f + 0.5f );
		c[1] = (unsigned char)( G * 255.0f + 0.5f );
		c[0] = (unsigned char)( B * 255.0f + 0.5f );
	}
}


bool CVitaFont::plat_load( const char *filename )
{
	char path[256];
	snprintf( path, sizeof( path ), "fonts/%s.fnt.xbx", filename );

	void *p_file = File::Open( path, "rb" );
	if( !p_file )
	{
		VLOG( "FNT", "introuvable : '%s'", path );
		return false;
	}

	unsigned char hdr[16];
	if( File::Read( hdr, 16, 1, p_file ) != 16 )
	{
		File::Close( p_file );
		return false;
	}

	int num_chars    = *(int *)( hdr + 4 );
	m_default_height = *(int *)( hdr + 8 );
	m_default_base   = *(int *)( hdr + 12 );

	if(( num_chars <= 0 ) || ( num_chars > 4096 ))
	{
		VLOG( "FNT", "!! '%s' : %d caracteres, hors bornes", path, num_chars );
		File::Close( p_file );
		return false;
	}

	mp_glyphs = (SVitaGlyph *)calloc( num_chars, sizeof( SVitaGlyph ));
	if( !mp_glyphs )
	{
		File::Close( p_file );
		return false;
	}
	m_num_glyphs = num_chars;

	// Table des caracteres : ligne de base + code ASCII. Un code NEGATIF
	// designe un glyphe special (boutons de manette), range a part.
	unsigned char *p_tab = (unsigned char *)malloc( num_chars * 4 );
	if( !p_tab )
	{
		File::Close( p_file );
		return false;
	}
	File::Read( p_tab, num_chars * 4, 1, p_file );

	for( int i = 0; i < num_chars; ++i )
	{
		mp_glyphs[i].baseline = *(unsigned short *)( p_tab + i * 4 );
		short ascii           = *(short *)( p_tab + i * 4 + 2 );
		if( ascii >= 0 )
			m_map[(unsigned char)ascii] = (unsigned char)i;
		else if( ascii >= -32 )
			m_special_map[(unsigned char)( -ascii - 1 )] = (unsigned char)i;
	}
	free( p_tab );

	// En-tete de texture : largeur, hauteur, profondeur en uint16.
	unsigned char thdr[16];
	File::Read( thdr, 16, 1, p_file );
	int tw    = *(unsigned short *)( thdr + 4 );
	int th    = *(unsigned short *)( thdr + 6 );
	int depth = *(unsigned short *)( thdr + 8 );

	if(( tw <= 0 ) || ( th <= 0 ) || ( tw > 2048 ) || ( th > 2048 ) || ( depth != 8 ))
	{
		VLOG( "FNT", "!! '%s' : planche %dx%d profondeur %d inattendue",
		      path, tw, th, depth );
		File::Close( p_file );
		return false;
	}

	int num_bytes = ( tw * th + 3 ) & ~3;
	unsigned char *p_idx = (unsigned char *)malloc( num_bytes );
	unsigned char  clut[1024];
	if( !p_idx )
	{
		File::Close( p_file );
		return false;
	}
	File::Read( p_idx, num_bytes, 1, p_file );
	File::Read( clut, 1024, 1, p_file );
	if( strncmp( filename, "Buttons", 7 ) == 0 )
		ps2_recolor_clut( clut );

	// Palette : octets R, G, B, A dans le fichier, et alpha double. Les alphas
	// du moteur plafonnent a 0x80 (heritage PS2) ; sans ce doublement, tout le
	// texte serait a demi transparent.
	//
	// [SOURCE] XBox/NX/chars.cpp:456 « Switch from RGBA to BGRA format
	// palette » : la palette du .fnt.xbx est en RGBA (rouge dans l'octet de
	// poids faible), que la Xbox convertit en D3DCOLOR. La version precedente
	// la lisait deja comme un D3DCOLOR, donc rouge et bleu echanges -- sans
	// effet visible sur le texte blanc, mais les icones de ButtonsPs2
	// sortaient en croix jaune, rond bleu, triangle olive (issue #23). Lues
	// correctement : croix bleue, rond rouge, carre rose, triangle vert.
	unsigned char *p_rgba = (unsigned char *)malloc( tw * th * 4 );
	if( !p_rgba )
	{
		free( p_idx );
		File::Close( p_file );
		return false;
	}
	for( int i = 0; i < tw * th; ++i )
	{
		const unsigned char *p_c = clut + p_idx[i] * 4;
		unsigned int a = p_c[3];
		a = ( a >= 0x80 ) ? 0xFF : ( a * 2 );
		p_rgba[i * 4 + 0] = p_c[0];		// R
		p_rgba[i * 4 + 1] = p_c[1];		// G
		p_rgba[i * 4 + 2] = p_c[2];		// B
		p_rgba[i * 4 + 3] = (unsigned char)a;
	}
	free( p_idx );

	glGenTextures( 1, &m_texture );
	glBindTexture( GL_TEXTURE_2D, m_texture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE, p_rgba );
	glBindTexture( GL_TEXTURE_2D, 0 );
	free( p_rgba );

	// Table des sous-textures, precedee d'un compte qu'on ignore.
	unsigned char *p_sub = (unsigned char *)malloc( num_chars * 8 + 4 );
	if( p_sub )
	{
		File::Read( p_sub, num_chars * 8 + 4, 1, p_file );
		for( int i = 0; i < num_chars; ++i )
		{
			const unsigned short *p = (const unsigned short *)( p_sub + 4 + i * 8 );
			int x = p[0], y = p[1], w = p[2], h = p[3];
			mp_glyphs[i].w  = w;
			mp_glyphs[i].h  = h;
			// UV rentrees d'un DEMI-TEXEL (issue #24). Celles de la Xbox
			// (chars.cpp:489) tombent pile sur le bord du glyphe : en
			// GL_LINEAR, agrandi x1,5 vers 960x544, l'echantillon du bord
			// prend moitie du texel VOISIN de l'atlas -- d'ou les traits
			// verticaux a cote de certaines fleches.
			mp_glyphs[i].u0 = ( (float)x + 0.5f ) / (float)tw;
			mp_glyphs[i].v0 = ( (float)y + 0.5f ) / (float)th;
			mp_glyphs[i].u1 = ( (float)( x + w ) - 0.5f ) / (float)tw;
			mp_glyphs[i].v1 = ( (float)( y + h ) - 0.5f ) / (float)th;
		}
		free( p_sub );
	}

	File::Close( p_file );

	// Espacement par defaut : la largeur du Â« I Â», comme le moteur d'origine.
	const SVitaGlyph *p_i = GetGlyph( 'I' );
	if( p_i && p_i->w )
		m_space_spacing = p_i->w;

	static int s_traced = 0;
	if( s_traced < 20 )
	{
		++s_traced;
		VLOG( "FNT", "'%s' : %d glyphes, planche %dx%d, hauteur %d",
		      filename, num_chars, tw, th, m_default_height );
	}
	return true;
}


void CVitaFont::plat_query_string( char *String, float &width, float &height ) const
{
	width  = 0.0f;
	height = (float)m_default_height;
	if( !String )
		return;

	for( const char *p = String; *p; ++p )
	{
		// Memes regles que XBox/NX/chars.cpp:574 (QueryString) : une balise
		// qui produit un glyphe compte sa largeur, l'espace compte
		// mSpaceSpacing, et chaque caractere est suivi de mCharSpacing.
		//
		// [CORRIGE] l'ancienne version mesurait \b3 comme le chiffre 3, et
		// l'espace comme le premier glyphe de la police (la table rend 0 pour
		// un caractere absent) : les textes centres en etaient decales.
		const SVitaGlyph *g = NULL;
		if( *p == '\\' )
		{
			++p;
			if( !*p ) break;
			CVitaFont *p_gfont;
			g = ParseTag( &p, &p_gfont );
			if( !g )
				continue;
		}
		else if( *p == ' ' )
		{
			width += (float)( m_space_spacing + m_char_spacing );
			continue;
		}
		else
			g = GetGlyph( (unsigned char)*p );
		if( g )
			width += (float)( g->w + m_char_spacing );
	}
}


// --- CVitaText -------------------------------------------------------------

CVitaText::CVitaText( CWindow2D *p_window )
	: CText( p_window )
{
	NxVita::TextRegister( this );
}

CVitaText::~CVitaText()
{
	NxVita::TextUnregister( this );
}

} // namespace Nx


namespace NxVita
{

// "txt" : journalise les textes VISIBLES (alpha > 0) de l'image, pour que les
// scripts de test lisent l'ecran (boites de dialogue du Story Mode, #51).
void JournaliserTextes( void )
{
	int n = 0;
	for( int i = 0; i < s_num; ++i )
	{
		Nx::CVitaText *p = sp_texts[i];
		if( p->IsHiddenNow() || !p->Str() || !*p->Str() || ( p->Color().a == 0 ))
			continue;
		VLOG( "TXT", "visible \"%.120s\" (%.0f,%.0f) a=%d rgb=%d,%d,%d", p->Str(), p->PosX(), p->PosY(), (int)p->Color().a,
		      (int)p->Color().r, (int)p->Color().g, (int)p->Color().b );
		++n;
	}
	VLOG( "TXT", "fin des textes visibles : %d", n );
}

int TextePriorites( float *p_out, int max )
{
	int n = 0;
	for( int i = 0; i < s_num; ++i )
	{
		if( sp_texts[i]->IsHiddenNow())
			continue;
		const float pr = sp_texts[i]->GetPri();
		int j = 0;
		while(( j < n ) && ( p_out[j] < pr ))
			++j;
		if(( j < n ) && ( p_out[j] == pr ))
			continue;
		if( n == max )
			continue;	// au-dela : non dessine (journalise par l'appelant)
		for( int k = n; k > j; --k )
			p_out[k] = p_out[k - 1];
		p_out[j] = pr;
		++n;
	}
	return n;
}

// --- Lot de glyphes (issue #17) ---------------------------------------------
//
// Un glDrawArrays PAR CARACTERE : vitaGL recopie a chaque appel les tableaux
// cote client dans sa memoire temporaire et repasse par la selection du
// shader du pipeline fixe. L'ecran View Stats du menu pause (stats.qb,
// create_stats_menu / build_stat_goals_menu) affiche des centaines de glyphes
// (10 statistiques, description en textBlockElement, liste des objectifs de
// la statistique en police dialog) : autant d'appels par image.
//
// Les glyphes consecutifs de meme planche et de meme couleur partent
// maintenant en UN glDrawArrays( GL_TRIANGLES ), y compris d'un texte au
// suivant dans la meme tranche de priorite. Le lot est vide avant tout
// changement de texture ou de couleur, donc l'ordre de dessin ne change pas.
// " xgl 0/1 " : lots (1, defaut) ou un appel par glyphe (0), pour l'A/B.
int g_vita_lots_glyphes = 1;
int g_vita_2d_glyphes = 0;		// glyphes et appels de l'image (bilan [2D], p_nx.cpp)
int g_vita_2d_appels_txt = 0;

#define LOT_GLYPHES_MAX	128
static float  s_lot_xyz[LOT_GLYPHES_MAX * 6 * 3];
static float  s_lot_uv[LOT_GLYPHES_MAX * 6 * 2];
static int    s_lot_n = 0;
static GLuint s_lot_tex = 0;		// planche liee (0 : inconnue, a lier)
static float  s_lot_rgba[4] = { -1.0f, -1.0f, -1.0f, -1.0f };

static void lot_vider( void )
{
	if( !s_lot_n )
		return;
	glVertexPointer( 3, GL_FLOAT, 0, s_lot_xyz );
	glTexCoordPointer( 2, GL_FLOAT, 0, s_lot_uv );
	glDrawArrays( GL_TRIANGLES, 0, s_lot_n * 6 );
	++g_vita_2d_appels_txt;
	s_lot_n = 0;
}

static void lot_texture( GLuint tex )
{
	if( tex == s_lot_tex )
		return;
	lot_vider();
	glBindTexture( GL_TEXTURE_2D, tex );
	s_lot_tex = tex;
}

static void lot_glyphe( float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1 )
{
	float *p = s_lot_xyz + s_lot_n * 18;
	float *t = s_lot_uv + s_lot_n * 12;
	// Deux triangles (x0,y0)(x1,y0)(x1,y1) et (x0,y0)(x1,y1)(x0,y1) : le
	// meme quad que l'eventail d'avant.
	const float xyz[18] = { x0, y0, 0.0f, x1, y0, 0.0f, x1, y1, 0.0f,
	                        x0, y0, 0.0f, x1, y1, 0.0f, x0, y1, 0.0f };
	const float uv[12]  = { u0, v0, u1, v0, u1, v1,
	                        u0, v0, u1, v1, u0, v1 };
	memcpy( p, xyz, sizeof( xyz ));
	memcpy( t, uv, sizeof( uv ));
	++g_vita_2d_glyphes;
	if(( ++s_lot_n == LOT_GLYPHES_MAX ) || !g_vita_lots_glyphes )
		lot_vider();
}

// Couleur courante du texte : echelle PS2 (128 = 1,0), comme m_rgba.
static void couleur_texte( const Image::RGBA &c, float kc )
{
	float cr = (float)c.r / kc, cg = (float)c.g / kc;
	float cb = (float)c.b / kc, ca = (float)c.a / kc;
	if( cr > 1.0f ) cr = 1.0f;
	if( cg > 1.0f ) cg = 1.0f;
	if( cb > 1.0f ) cb = 1.0f;
	if( ca > 1.0f ) ca = 1.0f;
	if(( cr == s_lot_rgba[0] ) && ( cg == s_lot_rgba[1] ) && ( cb == s_lot_rgba[2] ) && ( ca == s_lot_rgba[3] ))
		return;
	lot_vider();
	s_lot_rgba[0] = cr; s_lot_rgba[1] = cg; s_lot_rgba[2] = cb; s_lot_rgba[3] = ca;
	glColor4f( cr, cg, cb, ca );
}

void RenderText2D( float pri )
{
	if( s_num == 0 )
		return;

	// Tri par priorite, comme les sprites.
	for( int i = 1; i < s_num; ++i )
	{
		Nx::CVitaText *p = sp_texts[i];
		int j = i - 1;
		while(( j >= 0 ) && ( sp_texts[j]->GetPri() > p->GetPri()))
		{
			sp_texts[j + 1] = sp_texts[j];
			--j;
		}
		sp_texts[j + 1] = p;
	}

	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0.0f, SCREEN_W, SCREEN_H, 0.0f, -1.0f, 1.0f );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glEnable( GL_BLEND );
	if( g_vita_mx2 )
	{
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
	glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
	glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE );
	glTexEnvi( GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE );
	glTexEnvi( GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR );
	glTexEnvi( GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_TEXTURE );
	glTexEnvi( GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_PRIMARY_COLOR );
	{ const GLfloat deux = 2.0f;
	  glTexEnvfv( GL_TEXTURE_ENV, GL_RGB_SCALE, &deux );
	  glTexEnvfv( GL_TEXTURE_ENV, GL_ALPHA_SCALE, &deux ); }
	}
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glEnable( GL_TEXTURE_2D );

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	const float sx = SCREEN_W / ENGINE_2D_W;
	const float sy = SCREEN_H / ENGINE_2D_H;

	int drawn = 0;

	// Les sprites ont lie d'autres textures et pose d'autres couleurs depuis
	// la tranche precedente : etat du lot inconnu.
	s_lot_n = 0;
	s_lot_tex = 0;
	s_lot_rgba[0] = s_lot_rgba[1] = s_lot_rgba[2] = s_lot_rgba[3] = -1.0f;

	for( int i = 0; i < s_num; ++i )
	{
		Nx::CVitaText *p = sp_texts[i];
		if( p->GetPri() != pri )
			continue;
		if( p->IsHiddenNow())
			continue;

		const char *p_str = p->Str();
		if( !p_str || !*p_str )
			continue;

		Nx::CVitaFont *p_font = (Nx::CVitaFont *)p->Font();
		if( !p_font || !p_font->GetTexture())
			continue;

		lot_texture( p_font->GetTexture());

		Image::RGBA c = p->Color();
		const float kc = g_vita_mx2 ? 255.0f : 128.0f;	// mx2 (#52)
		couleur_texte( c, kc );

		float pen  = p->PosX();
		float base = p->PosY();
		int   spacing = p_font->GetCharSpacing();

		for( const char *q = p_str; *q; ++q )
		{
			const Nx::SVitaGlyph *g = NULL;
			// Police qui porte le glyphe : celle du texte, sauf pour une icone.
			Nx::CVitaFont *p_gfont = p_font;

			if( *q == '\\' )
			{
				++q;
				if( !*q ) break;
				// \cN : [SOURCE] XBox/NX/chars.cpp:739. N = 0 ou texte en
				// override_encoded_rgba -> couleur de l'element ; sinon entree
				// N-1 de la table de la police, alpha compris (memcard.q:1381
				// '\c3STORY: ...' = bleu "unhighlight" des themes, #49).
				if((( *q == 'c' ) || ( *q == 'C' )) && q[1] )
				{
					const int d = Nx::dehex( q[1] );
					if(( d <= 0 ) || ( d > 16 ) || p->ColorOverride())
						couleur_texte( c, kc );
					else
						couleur_texte( p_font->GetRGBATableEntry( d - 1 ), kc );
				}
				g = p_font->ParseTag( &q, &p_gfont );
				if( !g ) continue;
			}
			else if( *q == ' ' )
			{
				// L'espace n'a PAS de glyphe dans la police : la table de
				// correspondance rend 0, c'est-a-dire le premier glyphe --
				// un Â« A Â». D'ou Â« STORYAMODE Â» au lieu de Â« STORY MODE Â».
				// On le traite avant toute recherche.
				// Espace PLUS espacement de caractere, comme la Xbox
				// (chars.cpp:815) et comme plat_query_string : sinon la largeur
				// mesuree pour centrer un texte n'est pas celle dessinee.
				pen += (float)( p_font->GetSpaceSpacing() + spacing ) * p->ScaleX();
				continue;
			}
			else
			{
				g = p_font->GetGlyph( (unsigned char)*q );
			}

			if( !g || !g->w || !g->h )
			{
				if( *q == ' ' )
					pen += (float)p_font->GetSpaceSpacing();
				continue;
			}

			float w = (float)g->w * p->ScaleX();
			float h = (float)g->h * p->ScaleY();

			// Placement vertical : ( DefaultBase - baseline ), comme la
			// reference (DX9/NX/chars.cpp:858, identique cote XBox).
			//
			// [CORRIGE] la formule precedente omettait DefaultBase et ne
			// gardait que ï¿½ - baseline ï¿½. L'ecart est une CONSTANTE, donc
			// invisible sur le texte seul : tout le texte remontait ensemble.
			// Cela se voyait en revanche par rapport aux elements voisins,
			// soulignement et icones, qui paraissaient ï¿½ en dessous ï¿½ alors
			// que c'est le texte qui etait trop haut.
			//
			// DefaultBase est la ligne de base commune de la fonte ; baseline
			// celle du glyphe. Leur difference place chaque glyphe par rapport
			// a cette ligne commune, ce qu'aucune des deux valeurs ne fait
			// seule.
			float x0 = pen * sx;
			float y0 = ( base
			             + (float)( (int)p_gfont->GetDefaultBase() - g->baseline )
			               * p->ScaleY()
			             + ENGINE_2D_Y_MARGIN ) * sy;
			float x1 = x0 + w * sx;
			float y1 = y0 + h * sy;

			// AUCUNE inversion ici, contrairement aux sprites.
			//
			// [VERIFIE a l'ecran, par elimination] Les deux formats ne rangent
			// pas leurs lignes dans le meme sens :
			//   .img (sprites) : de bas en haut  -> il faut inverser V
			//   .fnt (polices) : de haut en bas  -> il ne faut pas
			// Les deux autres formules ont ete essayees et donnent, l'une des
			// lettres a l'envers, l'autre des morceaux de planche pris au
			// hasard. Â« Convention D3D contre OpenGL Â» n'est donc pas une
			// regle globale : elle se decide FORMAT PAR FORMAT.
			// Une icone vient d'une autre planche : on la lie le temps d'un
			// glyphe, comme la Xbox change de police pour un caractere
			// (chars.cpp:866). lot_texture vide le lot a chaque changement.
			lot_texture( p_gfont->GetTexture());
			lot_glyphe( x0, y0, x1, y1, g->u0, g->v0, g->u1, g->v1 );
			if( p_gfont != p_font )
				lot_texture( p_font->GetTexture());

			pen += (float)( g->w + spacing ) * p->ScaleX();
			++drawn;
		}
	}

	lot_vider();
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );
	if( g_vita_mx2 )
	{
	{ const GLfloat un = 1.0f;
	  glTexEnvfv( GL_TEXTURE_ENV, GL_RGB_SCALE, &un );
	  glTexEnvfv( GL_TEXTURE_ENV, GL_ALPHA_SCALE, &un ); }
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	}
	glDisable( GL_BLEND );
	glDisable( GL_TEXTURE_2D );

	{
		static int s_f = 0;
		if(( ++s_f % 120 ) == 1 )
		{
			int hidden = 0, no_font = 0, empty = 0;
			for( int i = 0; i < s_num; ++i )
			{
				if( sp_texts[i]->IsHiddenNow())			++hidden;
				else if( !sp_texts[i]->Font())			++no_font;
				else if( !sp_texts[i]->Str()
				         || !*sp_texts[i]->Str())		++empty;
			}
			VLOG( "TXT", "textes : %d inscrits, %d caches, %d sans police, "
			             "%d vides, %d glyphes dessines",
			      s_num, hidden, no_font, empty, drawn );
		}
	}
}

} // namespace NxVita
