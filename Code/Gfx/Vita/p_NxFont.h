///////////////////////////////////////////////////////////////////////////////
// p_NxFont.h — polices et texte 2D
//
// Format des .fnt.xbx, releve sur DX9/NX/chars.cpp:304 (LoadFont) :
//
//   16 octets  : [1]=nombre de caracteres, [2]=hauteur par defaut,
//                [3]=ligne de base par defaut
//   4*N octets : par caractere, uint16 ligne de base + sint16 code ASCII
//                (un code NEGATIF designe un caractere special, range dans
//                 une table a part -- boutons de manette, symboles)
//   16 octets  : en-tete de texture, largeur/hauteur/profondeur en uint16
//                aux offsets 4, 6, 8
//   W*H octets : indices sur un octet, NON entrelaces (contrairement aux
//                textures : le backend d'origine ne les desentrelace pas)
//   1024 octets: palette, 256 entrees D3DCOLOR
//   4 octets   : nombre de sous-textures (ignore)
//   8*N octets : par caractere, x/y/w/h en uint16 dans la planche
//
// Le texte du moteur peut contenir des balises : « \cN » change la couleur,
// « \bN » et « \sN » inserent un glyphe special. On les interprete a minima --
// assez pour que les menus soient lisibles, pas plus.

#ifndef __GFX_VITA_P_NXFONT_H__
#define __GFX_VITA_P_NXFONT_H__

#include <gfx/NxFont.h>
#include <vitaGL.h>

namespace Nx
{

struct SVitaGlyph
{
	float	u0, v0, u1, v1;
	int		w, h;
	int		baseline;
};


class CVitaFont : public CFont
{
public:
	CVitaFont();
	virtual ~CVitaFont();

	GLuint				GetTexture() const	{ return m_texture; }
	const SVitaGlyph *	GetGlyph( unsigned char c ) const;
	const SVitaGlyph *	GetSpecialGlyph( int index ) const;
	int					GetHeight() const	{ return m_default_height; }
	int					GetCharSpacing() const	{ return m_char_spacing; }
	int					GetSpaceSpacing() const	{ return m_space_spacing; }
	// Table des couleurs \cN (LoadFont color_tab=..., NxFontMan.cpp:173).
	Image::RGBA			GetRGBATableEntry( int i ) const	{ return m_rgba_tab[i & 15]; }

	// Interprete une balise de texte, *pp pointant le caractere qui suit
	// l'antislash. Rend le glyphe a dessiner (NULL si la balise n'en produit
	// pas) et, dans *pp_font, la police qui le porte : les icones \bN et \mN
	// viennent de la police de BOUTONS, pas de celle du texte. *pp est laisse
	// sur le dernier caractere consomme.
	const SVitaGlyph *	ParseTag( const char **pp, CVitaFont **pp_font ) const;

private:
	virtual bool	plat_load( const char *filename );
	virtual void	plat_set_spacings( int charSpacing, int spaceSpacing );
	virtual void	plat_unload();
	virtual uint32	plat_get_default_height() const	{ return m_default_height; }
	virtual uint32	plat_get_default_base() const	{ return m_default_base; }
	virtual void	plat_query_string( char *String, float &width, float &height ) const;
	virtual void	plat_mark_as_button_font( bool isButton );
	virtual void	plat_set_rgba_table( Image::RGBA *pTab );

	GLuint			m_texture;
	SVitaGlyph *	mp_glyphs;
	int				m_num_glyphs;
	unsigned char	m_map[256];
	unsigned char	m_special_map[32];
	int				m_default_height;
	int				m_default_base;
	int				m_char_spacing;
	int				m_space_spacing;
	Image::RGBA		m_rgba_tab[16];
};


class CVitaText : public CText
{
public:
	CVitaText( CWindow2D *p_window = NULL );
	virtual ~CVitaText();

	bool			IsHiddenNow() const	{ return m_hidden; }
	float			GetPri() const		{ return m_priority; }
	CFont *			Font() const		{ return mp_font; }
	const char *	Str() const			{ return m_string; }
	float			PosX() const		{ return m_xpos; }
	float			PosY() const		{ return m_ypos; }
	float			ScaleX() const		{ return m_xscale; }
	float			ScaleY() const		{ return m_yscale; }
	Image::RGBA		Color() const		{ return m_rgba; }
	bool			ColorOverride() const	{ return m_color_override; }
};

} // namespace Nx


namespace NxVita
{

// Dessine tous les textes visibles. A appeler apres les sprites.
// Textes de priorite EXACTEMENT pri ; TextePriorites : priorites distinctes
// des textes inscrits, croissantes (#54).
void RenderText2D( float pri );
int  TextePriorites( float *p_out, int max );
// Issue #17 : lots de glyphes (" xgl 0/1 ") et compteurs de l'image.
extern int g_vita_lots_glyphes;
extern int g_vita_2d_glyphes;
extern int g_vita_2d_appels_txt;
void JournaliserTextes( void );

// Issue #24 : textes Xbox des scripts (disque dur, console Xbox...) reecrits
// pour la Vita. Rend une copie (malloc, a liberer par free) si au moins un
// motif a ete remplace, NULL sinon. Appele par Gfx/2D/TextElement.cpp.
char *TexteVitaSubstitue( const char *p_in );

} // namespace NxVita

#endif // __GFX_VITA_P_NXFONT_H__
