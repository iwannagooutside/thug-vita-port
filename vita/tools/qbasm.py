#!/usr/bin/env python3
"""Assembleur / desassembleur de scripts QB (THUG, format .qb de l'ISO Xbox).

    python3 vita/tools/qbasm.py dis <archive.prx> <membre.qb> [-o out.q]
    python3 vita/tools/qbasm.py dis <fichier.qb> [-o out.q]
    python3 vita/tools/qbasm.py asm <source.q> -o <out.qb> [--header out.h --symbol nom]
    python3 vita/tools/qbasm.py roundtrip <qb.prx> [membre ...]
    python3 vita/tools/qbasm.py sum <qb.prx> <membre.qb> <script>

Jetons : Code/Gel/Scripting/tokens.h (valeurs), skiptoken.cpp (tailles),
parse.cpp (ParseQB : symboles 'nom = valeur' et 'script ... endscript', table
des noms ESCRIPTTOKEN_CHECKSUM_NAME en fin de fichier, puis ENDOFFILE).
CRC des noms : zlib.crc32(nom.lower()) ^ 0xFFFFFFFF (Crc::GenerateCRCFromString).

Syntaxe du texte (sortie de 'dis', entree de 'asm') :
  - un saut de ligne = un jeton ENDOFLINE ;
  - noms : identifiant nu, #"nom bizarre", ou #0x1234abcd (checksum sans nom) ;
  - <nom> = ARG + NAME, <...> = ALLARGS ;
  - "texte" = STRING, 'texte' = LOCALSTRING ; echappements \\" \\' \\\\ \\xHH,
    tout autre antislash reste litteral (\\n des textes du jeu) ;
  - 12 / -3 = INTEGER, 0x1F = HEXINTEGER, 1.5 / -0.25 / 1e-05 = FLOAT ;
    un '-' colle a un chiffre est un litteral negatif, sinon c'est MINUS ;
  - (x, y) = PAIR, (x, y, z) = VECTOR (litteraux numeriques seulement) ;
  - operateurs : = == < <= > >= + - * / . , : ( ) { } [ ] || && ;
  - mots-cles (casse indifferente) : script endscript if else elseif endif
    begin repeat break return switch endswitch case default not
    randomrange randomrange2 ;
  - formes brutes pour l'aller-retour : $N (jeton d'un octet N), $arg,
    $jump(off), $line(n), $float(0xHHHHHHHH), $random(tok:w,w:o,o) ;
  - '//' ou ';' commente jusqu'a la fin de ligne ;
  - lignes '%name 0xCRC texte' : table des noms explicite (sinon generee) ;
  - '%replaces script 0xSOMME' : script de qb.prx remplace ; la copie privee
    des blocs '; VITA {' ... '; VITA }' doit redonner cette somme de contenu
    (CalculateScriptContentsChecksum, voir 'sum') -- verifie a l'assemblage,
    reverifiee sur l'original par le C++ avant chargement ;
  - '%needs nom ...' : symboles du jeu qui doivent exister (verifie en C++).

Mode source (asm par defaut) : lignes vides et commentaires ne produisent
aucun ENDOFLINE. Mode --raw (utilise par roundtrip) : chaque ligne compte.

Verifications d'asm : jetons connus, accolades/crochets/parentheses
equilibres par ligne logique, script ... endscript apparies, if/endif et
switch/endswitch et begin/repeat apparies dans chaque script, pas de
commande hors script. Le C++ (vita_qb_options.cpp) refait une verification
structurelle avant de charger.
"""
import argparse, os, re, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def crc(s):
    # Crc::GenerateCRCFromString : minuscules ASCII, '/' -> '\\'.
    if isinstance(s, str):
        s = s.encode('latin-1')
    return (zlib.crc32(s.lower().replace(b'/', b'\\')) ^ 0xFFFFFFFF) & 0xFFFFFFFF


# --- jetons (tokens.h) ------------------------------------------------------
EOF_, EOL, LINENUM, SSTRUCT, ESTRUCT, SARRAY, EARRAY, EQUALS = 0, 1, 2, 3, 4, 5, 6, 7
NAME, INTEGER, HEXINTEGER, FLOAT, STRING, LSTRING = 22, 23, 24, 26, 27, 28
VECTOR, PAIR = 30, 31
K_BEGIN, K_REPEAT, K_BREAK, K_SCRIPT, K_ENDSCRIPT = 32, 33, 34, 35, 36
K_IF, K_ELSE, K_ELSEIF, K_ENDIF, K_RETURN = 37, 38, 39, 40, 41
CHECKSUM_NAME, ALLARGS, ARG, JUMP = 43, 44, 45, 46
RANDOMS = (47, 55, 64, 65)
K_SWITCH, K_ENDSWITCH, K_CASE, K_DEFAULT = 60, 61, 62, 63

# Jetons d'un octet (skiptoken.cpp, branche ++p_token).
UN_OCTET = {1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21,
            32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 44, 45, 48, 50, 51, 52, 53,
            54, 56, 57, 58, 59, 60, 61, 62, 63, 66}

OPS = {3: '{', 4: '}', 5: '[', 6: ']', 7: '=', 8: '.', 9: ',', 10: '-', 11: '+',
       12: '/', 13: '*', 14: '(', 15: ')', 17: '==', 18: '<', 19: '<=', 20: '>',
       21: '>=', 44: '<...>', 50: '||', 51: '&&', 66: ':'}
KEYWORDS = {32: 'begin', 33: 'repeat', 34: 'break', 35: 'script', 36: 'endscript',
            37: 'if', 38: 'else', 39: 'elseif', 40: 'endif', 41: 'return',
            48: 'randomrange', 56: 'randomrange2', 57: 'not', 60: 'switch',
            61: 'endswitch', 62: 'case', 63: 'default'}
KW_BY_TEXT = {v: k for k, v in KEYWORDS.items()}
IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*\Z')


# --- lecture des jetons -----------------------------------------------------
def tokenize_bin(d):
    """Liste de (jeton, valeur, octets bruts). S'arrete sur ENDOFFILE (inclus)."""
    i, out = 0, []
    while True:
        if i >= len(d):
            raise ValueError('pas de ENDOFFILE')
        t = d[i]
        s = i
        if t == EOF_:
            out.append((t, None, d[i:i + 1]))
            return out, i + 1
        if t in UN_OCTET:
            v = None; i += 1
        elif t in (LINENUM, NAME, INTEGER, HEXINTEGER, FLOAT, JUMP):
            fmt = {INTEGER: '<i', FLOAT: '<f'}.get(t, '<I')
            v = struct.unpack_from(fmt, d, i + 1)[0]; i += 5
        elif t == VECTOR:
            v = struct.unpack_from('<3f', d, i + 1); i += 13
        elif t == PAIR:
            v = struct.unpack_from('<2f', d, i + 1); i += 9
        elif t in (STRING, LSTRING):
            n = struct.unpack_from('<I', d, i + 1)[0]
            v = d[i + 5:i + 5 + n]; i += 5 + n
        elif t == CHECKSUM_NAME:
            c = struct.unpack_from('<I', d, i + 1)[0]
            j = d.index(0, i + 5)
            v = (c, d[i + 5:j]); i = j + 1
        elif t in RANDOMS:
            n = struct.unpack_from('<I', d, i + 1)[0]
            w = struct.unpack_from('<%dh' % n, d, i + 5)
            o = struct.unpack_from('<%di' % n, d, i + 5 + 2 * n)
            v = (n, w, o); i += 5 + 6 * n
        else:
            raise ValueError('jeton %d inconnu a l\'octet %d' % (t, i))
        out.append((t, v, d[s:i]))


# --- desassemblage ----------------------------------------------------------
def fmt_float(f):
    if f != f or f in (float('inf'), float('-inf')):
        return None
    s = repr(f)
    if '.' not in s and 'e' not in s and 'n' not in s:
        s += '.0'
    if struct.pack('<f', float(s)) != struct.pack('<f', f):
        return None
    return s


def fmt_string(b, q):
    out = [q]
    for c in b:
        ch = chr(c)
        if ch == '\\':
            out.append('\\\\')
        elif ch == q:
            out.append('\\' + q)
        elif 32 <= c < 127:
            out.append(ch)
        else:
            out.append('\\x%02x' % c)
    out.append(q)
    return ''.join(out)


def fmt_name(c, noms):
    n = noms.get(c)
    if n is not None and crc(n) == c:
        try:
            ns = n.decode('latin-1') if isinstance(n, bytes) else n
        except Exception:
            ns = None
        if ns is not None:
            if IDENT.match(ns) and ns.lower() not in KW_BY_TEXT:
                return ns
            if '"' not in ns and '\\' not in ns and all(32 <= ord(ch) < 127 for ch in ns):
                return '#"%s"' % ns
    return '#0x%08x' % c


def disassemble(d, noms_globaux=None):
    toks, fin = tokenize_bin(d)
    if fin != len(d):
        raise ValueError('%d octets apres ENDOFFILE' % (len(d) - fin))
    noms = dict(noms_globaux or {})
    table = [v for t, v, _ in toks if t == CHECKSUM_NAME]
    for c, n in table:
        noms[c] = n.decode('latin-1')
    lignes, cur = [], []
    k = 0
    corps = [x for x in toks if x[0] not in (CHECKSUM_NAME, EOF_)]
    # Les CHECKSUM_NAME sont tous en fin de fichier (verifie sur qb.prx) ;
    # s'ils etaient intercales, l'aller-retour le detecterait.
    while k < len(corps):
        t, v, raw = corps[k]
        if t == EOL:
            lignes.append(' '.join(cur)); cur = []
        elif t == NAME:
            cur.append(fmt_name(v, noms))
        elif t == ARG:
            if k + 1 < len(corps) and corps[k + 1][0] == NAME:
                cur.append('<%s>' % fmt_name(corps[k + 1][1], noms)); k += 1
            else:
                cur.append('$arg')
        elif t == INTEGER:
            cur.append(str(v))
        elif t == HEXINTEGER:
            cur.append('0x%x' % v)
        elif t == FLOAT:
            s = fmt_float(v)
            cur.append(s if s else '$float(0x%08x)' % struct.unpack('<I', raw[1:5])[0])
        elif t in (VECTOR, PAIR):
            parts = [fmt_float(x) for x in v]
            if None in parts:
                raise ValueError('vecteur non representable')
            cur.append('(%s)' % ', '.join(parts))
        elif t in (STRING, LSTRING):
            if not v or v[-1] != 0 or 0 in v[:-1]:
                raise ValueError('chaine sans terminateur unique')
            cur.append(fmt_string(v[:-1], '"' if t == STRING else "'"))
        elif t == JUMP:
            cur.append('$jump(%d)' % struct.unpack('<i', raw[1:5])[0])
        elif t == LINENUM:
            cur.append('$line(%d)' % v)
        elif t in RANDOMS:
            n, w, o = v
            cur.append('$random(%d:%s:%s)' % (t, ','.join(map(str, w)), ','.join(map(str, o))))
        elif t in KEYWORDS:
            cur.append(KEYWORDS[t])
        elif t in OPS:
            # '(' suivi de nombre , nombre ) serait relu comme un PAIR.
            cur.append(OPS[t])
        else:
            cur.append('$%d' % t)
        k += 1
    texte = ''.join(l + '\n' for l in lignes) + ' '.join(cur) + '\n'
    texte += ''.join('%%name 0x%08x %s\n' % (c, n.decode('latin-1')) for c, n in table)
    return texte


# --- assemblage -------------------------------------------------------------
class AsmError(Exception):
    pass


NUM = r'-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?'
RE_PAIRVEC = re.compile(r'\(\s*(%s)\s*,\s*(%s)\s*(?:,\s*(%s)\s*)?\)' % (NUM, NUM, NUM))
RE_NUM = re.compile(r'-?0x[0-9A-Fa-f]+|' + NUM)
RE_NAMEREF = re.compile(r'#"([^"]*)"|#0x([0-9A-Fa-f]{1,8})|([A-Za-z_][A-Za-z0-9_]*)')


def parse_string(src, i, q):
    out = bytearray()
    i += 1
    while True:
        if i >= len(src):
            raise AsmError('chaine non terminee')
        ch = src[i]
        if ch == q:
            return bytes(out), i + 1
        if ch == '\\' and i + 1 < len(src):
            nx = src[i + 1]
            if nx in (q, '\\'):
                out.append(ord(nx)); i += 2; continue
            if nx == 'x' and re.match(r'[0-9A-Fa-f]{2}', src[i + 2:i + 4]):
                out.append(int(src[i + 2:i + 4], 16)); i += 4; continue
        out += ch.encode('latin-1')
        i += 1


def lex_line(src, noms_utilises):
    """Jetons d'une ligne (sans le ENDOFLINE). Rend une liste de (t, v)."""
    out, i, n = [], 0, len(src)

    def name_tok(m):
        if m.group(1) is not None:
            nm = m.group(1); c = crc(nm); noms_utilises.setdefault(c, nm)
        elif m.group(2) is not None:
            c = int(m.group(2), 16)
        else:
            nm = m.group(3); c = crc(nm); noms_utilises.setdefault(c, nm)
        return c

    while i < n:
        ch = src[i]
        if ch in ' \t\r':
            i += 1; continue
        if src.startswith('//', i) or ch == ';':
            break
        if ch in '"\'':
            b, i = parse_string(src, i, ch)
            out.append((STRING if ch == '"' else LSTRING, b + b'\0')); continue
        if ch == '(':
            m = RE_PAIRVEC.match(src, i)
            if m:
                vals = [float(x) for x in m.groups() if x is not None]
                out.append((VECTOR if len(vals) == 3 else PAIR, tuple(vals)))
                i = m.end(); continue
            out.append((14, None)); i += 1; continue
        if ch == '<':
            if src.startswith('<...>', i):
                out.append((ALLARGS, None)); i += 5; continue
            m = RE_NAMEREF.match(src, i + 1)
            if m and src.startswith('>', m.end()):
                out.append((ARG, None)); out.append((NAME, name_tok(m)))
                i = m.end() + 1; continue
            if src.startswith('<=', i):
                out.append((19, None)); i += 2; continue
            out.append((18, None)); i += 1; continue
        if ch == '>':
            if src.startswith('>=', i):
                out.append((21, None)); i += 2; continue
            out.append((20, None)); i += 1; continue
        if ch == '=':
            if src.startswith('==', i):
                out.append((17, None)); i += 2; continue
            out.append((EQUALS, None)); i += 1; continue
        if src.startswith('||', i):
            out.append((50, None)); i += 2; continue
        if src.startswith('&&', i):
            out.append((51, None)); i += 2; continue
        if ch.isdigit() or (ch in '-.' and i + 1 < n and (src[i + 1].isdigit() or
                                                         (src[i + 1] == '.' and ch == '-'))):
            m = RE_NUM.match(src, i)
            if m:
                s = m.group(0)
                if '0x' in s.lower():
                    out.append((HEXINTEGER, int(s, 16) & 0xFFFFFFFF))
                elif re.search(r'[.eE]', s):
                    out.append((FLOAT, float(s)))
                else:
                    v = int(s)
                    if not -2**31 <= v < 2**31:
                        raise AsmError('entier hors limites : %s' % s)
                    out.append((INTEGER, v))
                i = m.end(); continue
        if ch == '$':
            m = re.compile(r'\$(arg|jump|line|float|random|\d+)(?:\(([^)]*)\))?').match(src, i)
            if not m:
                raise AsmError('forme brute invalide : %s' % src[i:i + 20])
            kind, arg = m.group(1), m.group(2)
            if kind == 'arg':
                out.append((ARG, None))
            elif kind == 'jump':
                out.append((JUMP, int(arg)))
            elif kind == 'line':
                out.append((LINENUM, int(arg)))
            elif kind == 'float':
                out.append((FLOAT, ('raw', int(arg, 16))))
            elif kind == 'random':
                tk, w, o = arg.split(':')
                w = [int(x) for x in w.split(',')] if w else []
                o = [int(x) for x in o.split(',')] if o else []
                out.append((int(tk), (len(w), w, o)))
            else:
                t = int(kind)
                if t not in UN_OCTET:
                    raise AsmError('$%d n\'est pas un jeton d\'un octet' % t)
                out.append((t, None))
            i = m.end(); continue
        if ch == '#' or ch.isalpha() or ch == '_':
            m = RE_NAMEREF.match(src, i)
            if not m:
                raise AsmError('nom invalide : %s' % src[i:i + 20])
            if m.group(3) is not None and m.group(3).lower() in KW_BY_TEXT:
                out.append((KW_BY_TEXT[m.group(3).lower()], None))
            else:
                out.append((NAME, name_tok(m)))
            i = m.end(); continue
        for txt, t in (('{', 3), ('}', 4), ('[', 5), (']', 6), ('.', 8), (',', 9),
                       ('-', 10), ('+', 11), ('/', 12), ('*', 13), (')', 15), (':', 66)):
            if ch == txt:
                out.append((t, None)); i += 1; break
        else:
            raise AsmError('caractere inattendu %r' % ch)
    return out


def encode(t, v):
    if t in UN_OCTET:
        return bytes([t])
    if t == INTEGER:
        return bytes([t]) + struct.pack('<i', v)
    if t == FLOAT:
        if isinstance(v, tuple):
            return bytes([t]) + struct.pack('<I', v[1])
        return bytes([t]) + struct.pack('<f', v)
    if t in (NAME, HEXINTEGER, LINENUM):
        return bytes([t]) + struct.pack('<I', v)
    if t == JUMP:
        return bytes([t]) + struct.pack('<i', v)
    if t == VECTOR:
        return bytes([t]) + struct.pack('<3f', *v)
    if t == PAIR:
        return bytes([t]) + struct.pack('<2f', *v)
    if t in (STRING, LSTRING):
        return bytes([t]) + struct.pack('<I', len(v)) + v
    if t in RANDOMS:
        n, w, o = v
        return bytes([t]) + struct.pack('<I', n) + struct.pack('<%dh' % n, *w) + struct.pack('<%di' % n, *o)
    raise AsmError('jeton %d non encodable' % t)


def check_structure(toks):
    """Verifications que ParseQB ne fait qu'en assert (absents en release)."""
    k, n = 0, len(toks)
    prof = [0, 0, 0]  # {} [] ()
    in_script = False
    pile = []
    debut_ligne = True
    for (t, v), ligne in toks:
        if t == SSTRUCT: prof[0] += 1
        elif t == ESTRUCT: prof[0] -= 1
        elif t == SARRAY: prof[1] += 1
        elif t == EARRAY: prof[1] -= 1
        elif t == 14: prof[2] += 1
        elif t == 15: prof[2] -= 1
        if min(prof) < 0:
            raise AsmError('ligne %d : fermeture sans ouverture' % ligne)
        if t == K_SCRIPT:
            if in_script:
                raise AsmError('ligne %d : script dans un script' % ligne)
            in_script = True; pile = []
        elif t == K_ENDSCRIPT:
            if not in_script:
                raise AsmError('ligne %d : endscript sans script' % ligne)
            if pile:
                raise AsmError('ligne %d : %s non ferme' % (ligne, pile[-1]))
            if any(prof):
                raise AsmError('ligne %d : accolades/crochets/parentheses non equilibres' % ligne)
            in_script = False
        elif in_script:
            if t == K_IF: pile.append('if')
            elif t in (K_ELSE, K_ELSEIF):
                if not pile or pile[-1] != 'if':
                    raise AsmError('ligne %d : else hors if' % ligne)
            elif t == K_ENDIF:
                if not pile or pile.pop() != 'if':
                    raise AsmError('ligne %d : endif orphelin' % ligne)
            elif t == K_SWITCH: pile.append('switch')
            elif t == K_ENDSWITCH:
                if not pile or pile.pop() != 'switch':
                    raise AsmError('ligne %d : endswitch orphelin' % ligne)
            elif t == K_BEGIN: pile.append('begin')
            elif t == K_REPEAT:
                if not pile or pile.pop() != 'begin':
                    raise AsmError('ligne %d : repeat orphelin' % ligne)
    if in_script:
        raise AsmError('script non termine')
    if any(prof):
        raise AsmError('accolades/crochets/parentheses non equilibres en fin de fichier')


def check_top_level(toks):
    """Hors script, chaque ligne logique doit etre 'nom = valeur' (ParseQB)."""
    i, n = 0, len(toks)
    while i < n:
        t = toks[i][0][0]
        if t == EOL:
            i += 1; continue
        if t == K_SCRIPT:
            if i + 1 >= n or toks[i + 1][0][0] != NAME:
                raise AsmError('ligne %d : script sans nom' % toks[i][1])
            while toks[i][0][0] != K_ENDSCRIPT:
                i += 1
            i += 1; continue
        if t == NAME:
            j = i + 1
            while j < n and toks[j][0][0] == EOL:
                j += 1
            if j >= n or toks[j][0][0] != EQUALS:
                raise AsmError('ligne %d : commande hors script (nom sans =)' % toks[i][1])
            # saute la valeur jusqu'a la fin de ligne logique
            prof = 0; j += 1
            while j < n and toks[j][0][0] == EOL:
                j += 1
            while j < n:
                tt = toks[j][0][0]
                if tt in (3, 5, 14): prof += 1
                elif tt in (4, 6, 15): prof -= 1
                elif tt == EOL and prof == 0:
                    break
                j += 1
            i = j; continue
        raise AsmError('ligne %d : jeton %d inattendu hors script' % (toks[i][1], t))


def assemble(texte, raw=False, want_meta=False, copies=True):
    noms_utilises = {}
    table = None
    meta = {'replaces': [], 'needs': []}
    corps = []  # ((t, v), numero de ligne)
    lignes = texte.split('\n')
    sortie_lignes = []
    for num, l in enumerate(lignes, 1):
        if l.startswith('%name '):
            if table is None:
                table = []
            _, c, nm = l.split(' ', 2)
            table.append((int(c, 16), nm))
            continue
        if l.startswith('%replaces '):
            _, nm, somme = l.split()[:3]
            meta['replaces'].append((nm, int(somme, 16)))
            continue
        if l.startswith('%needs '):
            meta['needs'] += l.split()[1:]
            continue
        if l.startswith('%'):
            raise AsmError('ligne %d : directive inconnue %s' % (num, l.split()[0]))
        sortie_lignes.append((num, l))
    # En mode raw, le texte de dis se termine par : <ligne finale sans EOL>
    # puis les %name. On retire la ligne vide finale due au '\n' terminal.
    if raw and len(sortie_lignes) > 1 and sortie_lignes[-1][1] == '':
        sortie_lignes.pop()  # artefact du '\n' terminal
    for idx, (num, l) in enumerate(sortie_lignes):
        try:
            tk = lex_line(l, noms_utilises)
        except AsmError as e:
            raise AsmError('ligne %d : %s' % (num, e))
        dernier = idx == len(sortie_lignes) - 1
        if raw:
            corps += [(x, num) for x in tk]
            if not dernier:
                corps.append(((EOL, None), num))
        else:
            if not tk:
                continue
            corps += [(x, num) for x in tk]
            corps.append(((EOL, None), num))
    if not raw:
        check_structure(corps)
        check_top_level(corps)
        if copies and meta['replaces']:
            check_copies(texte, meta['replaces'])
    out = bytearray()
    for (t, v), _ in corps:
        out += encode(t, v)
    if table is None:
        table = sorted(noms_utilises.items(), key=lambda cv: (cv[0] & 0xFF, cv[0]))
    for c, nm in table:
        out += bytes([CHECKSUM_NAME]) + struct.pack('<I', c) + nm.encode('latin-1') + b'\0'
    out.append(EOF_)
    if want_meta:
        return bytes(out), meta
    return bytes(out)


RE_BLOC_VITA = re.compile(r'^[ \t]*;[ \t]*VITA[ \t]*\{.*?^[ \t]*;[ \t]*VITA[ \t]*\}[^\n]*\n', re.M | re.S)


def check_copies(texte, replaces):
    """Un script %replaces est une copie de l'original plus des blocs encadres
    par '; VITA {' et '; VITA }'. Sans ces blocs, il doit redonner exactement
    la somme de contenu de l'original : toute retouche hors bloc est refusee."""
    d = assemble(RE_BLOC_VITA.sub('', texte), copies=False)
    for nm, somme in replaces:
        try:
            s = contents_checksum(d, nm)
        except KeyError:
            raise AsmError('%%replaces %s : script absent du source' % nm)
        if s != somme:
            raise AsmError('%s : la copie (hors blocs VITA) a la somme 0x%08x, '
                           'l\'original 0x%08x -- copie modifiee hors des blocs' % (nm, s, somme))


def contents_checksum(d, script):
    """CalculateScriptContentsChecksum (parse.cpp:1882) du script nomme : CRC
    (Crc::UpdateCRC, depart 0xFFFFFFFF, sans inversion finale) des jetons qui
    suivent le nom jusqu'a endscript exclu, ENDOFLINE et numeros de ligne
    exclus. C'est la valeur rangee en tete de mpScript."""
    toks, _ = tokenize_bin(d)
    c = crc(script)
    for k in range(len(toks) - 1):
        if toks[k][0] == K_SCRIPT and toks[k + 1][0] == NAME and toks[k + 1][1] == c:
            buf = bytearray()
            j = k + 2
            while toks[j][0] != K_ENDSCRIPT:
                if toks[j][0] not in (EOL, LINENUM):
                    buf += toks[j][2]
                j += 1
            return (zlib.crc32(bytes(buf)) ^ 0xFFFFFFFF) & 0xFFFFFFFF
    raise KeyError(script)


def verify_bin(d):
    """Equivalent Python de la verification faite par le C++ avant chargement."""
    toks, fin = tokenize_bin(d)
    if fin != len(d):
        raise AsmError('octets apres ENDOFFILE')
    return toks


# --- sortie C ---------------------------------------------------------------
def write_header(d, path, symbol, source, meta=None):
    meta = meta or {'replaces': [], 'needs': []}
    lignes = ['// GENERE par vita/tools/qbasm.py depuis %s -- ne pas editer.' % source,
              '// %d octets, crc32 0x%08x.' % (len(d), zlib.crc32(d) & 0xFFFFFFFF),
              '#pragma once', '',
              'static const unsigned int %s_size = %du;' % (symbol, len(d)),
              'static const unsigned int %s_crc32 = 0x%08xu;' % (symbol, zlib.crc32(d) & 0xFFFFFFFF),
              'static const unsigned char %s[%d] __attribute__((aligned(4))) = {' % (symbol, len(d))]
    for i in range(0, len(d), 16):
        lignes.append('\t' + ''.join('0x%02x,' % b for b in d[i:i + 16]))
    lignes.append('};')
    lignes += ['',
               '// Scripts de qb.prx remplaces : charges seulement si la somme de contenu',
               '// de l\'original (tete de mpScript) est celle de la version lue (ISO USA).',
               'struct SQBRemplace { const char *p_nom; unsigned int crc_nom; unsigned int somme; };',
               'static const SQBRemplace %s_remplace[] = {' % symbol]
    for nm, somme in meta['replaces']:
        lignes.append('\t{ "%s", 0x%08xu, 0x%08xu },' % (nm, crc(nm), somme))
    lignes += ['\t{ 0, 0u, 0u }', '};', '',
               '// Symboles du jeu dont le .qb depend : tous doivent exister avant chargement.',
               'struct SQBRequis { const char *p_nom; unsigned int crc_nom; };',
               'static const SQBRequis %s_requis[] = {' % symbol]
    for nm in meta['needs']:
        lignes.append('\t{ "%s", 0x%08xu },' % (nm, crc(nm)))
    lignes += ['\t{ 0, 0u }', '};']
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(lignes) + '\n')


# --- outils -----------------------------------------------------------------
def charger(chemin, membre=None):
    if membre:
        from pre_scan import read_pre
        r = read_pre(chemin)
        for k, d in r.items():
            if k.lower() == membre.lower() or k.lower().endswith('\\' + membre.lower()):
                return d, r
        raise SystemExit('membre %s absent' % membre)
    return open(chemin, 'rb').read(), None


def noms_archive(r):
    noms = {}
    for k, d in (r or {}).items():
        if not k.lower().endswith('.qb'):
            continue
        try:
            toks, _ = tokenize_bin(d)
        except Exception:
            continue
        for t, v, _ in toks:
            if t == CHECKSUM_NAME:
                noms.setdefault(v[0], v[1].decode('latin-1'))
    return noms


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    a = sp.add_parser('dis'); a.add_argument('src'); a.add_argument('membre', nargs='?')
    a.add_argument('-o')
    a.add_argument('--script', help='ne sortir que ce script (lecture seule)')
    b = sp.add_parser('asm'); b.add_argument('src'); b.add_argument('-o', required=True)
    b.add_argument('--raw', action='store_true'); b.add_argument('--header')
    b.add_argument('--symbol', default='qb_data')
    e = sp.add_parser('sum', help='somme de contenu d\'un script (garde %replaces)')
    e.add_argument('src'); e.add_argument('membre', nargs='?'); e.add_argument('script')
    c = sp.add_parser('roundtrip'); c.add_argument('prx'); c.add_argument('membres', nargs='*')
    args = ap.parse_args()

    if args.cmd == 'dis':
        d, r = charger(args.src, args.membre)
        txt = disassemble(d, noms_archive(r))
        if args.script:
            m = re.search(r'^script %s\b.*?^endscript$' % re.escape(args.script), txt,
                          re.M | re.S | re.I)
            txt = m.group(0) + '\n' if m else ''
        if args.o:
            open(args.o, 'w', encoding='latin-1', newline='\n').write(txt)
        else:
            sys.stdout.write(txt)
    elif args.cmd == 'asm':
        txt = open(args.src, encoding='latin-1').read()
        try:
            d, meta = assemble(txt, raw=args.raw, want_meta=True)
            verify_bin(d)
        except AsmError as e:
            raise SystemExit('%s: %s' % (args.src, e))
        open(args.o, 'wb').write(d)
        if args.header:
            write_header(d, args.header, args.symbol, os.path.basename(args.src), meta)
        print('%s : %d octets' % (args.o, len(d)))
    elif args.cmd == 'sum':
        d, _ = charger(args.src, args.membre)
        print('0x%08x' % contents_checksum(d, args.script))
    elif args.cmd == 'roundtrip':
        from pre_scan import read_pre
        r = read_pre(args.prx)
        noms = noms_archive(r)
        ok = ko = 0
        for k, d in r.items():
            if not k.lower().endswith('.qb'):
                continue
            if args.membres and not any(m.lower() in k.lower() for m in args.membres):
                continue
            try:
                txt = disassemble(d, noms)
                d2 = assemble(txt, raw=True)
            except Exception as e:
                print('KO %s : %s' % (k, e)); ko += 1; continue
            if d2 == d:
                ok += 1
            else:
                n = next((i for i in range(min(len(d), len(d2))) if d[i] != d2[i]), min(len(d), len(d2)))
                print('KO %s : differe a l\'octet %d (%d / %d octets)' % (k, n, len(d), len(d2)))
                ko += 1
        print('aller-retour : %d identiques, %d differents' % (ok, ko))
        sys.exit(1 if ko else 0)


if __name__ == '__main__':
    main()
