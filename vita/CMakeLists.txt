cmake_minimum_required(VERSION 3.16)

# Cha√Æne vitasdk. Doit √™tre positionn√©e AVANT project().
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "toolchain vitasdk")
  else()
    message(FATAL_ERROR "$VITASDK non defini ‚Äî installe le vitasdk (vdpm)")
  endif()
endif()

project(thug_vita CXX C ASM)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "THUG")
set(VITA_TITLEID  "THUG00001")
set(VITA_VERSION  "01.04")

# ATTRIBUTE2=12 d√©bloque les 109 Mio de RAM suppl√©mentaires (365 au lieu des
# 256 par d√©faut). Le moteur tenait dans les 32 Mo de la PS2, donc c'est de la
# marge et non un besoin ‚Äî mais autant l'avoir d√®s le d√©part plut√¥t que de
# diagnostiquer un OOM au palier 2.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

# -- Palier 0 : rien du moteur, juste le squelette. -------------------------
# Les sources du moteur (Code/**) entreront ici au palier 1, r√©pertoire par
# r√©pertoire.
set(THUG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# main_vita.cpp (point d'entr√©e du palier 0) est retir√© : le moteur apporte
# son propre main() dans Sk/Main.cpp, et deux main() ne linkent pas. Les
# r√©glages m√©moire qu'il portait sont pass√©s dans vita_runtime.cpp.
# Le fichier est conserv√© dans le d√©p√¥t : il reste le plus petit test de
# bout en bout de la cha√Æne (VPK -> boot -> vitaGL -> log).
set(PALIER0_SOURCES
  src/vita_runtime.cpp
  src/vita_log.c
  src/vita_prof.c
  src/vita_glspy.c
  src/vita_memspy.c
  src/vita_dbgsrv.c
  src/vita_sans_splash.c
  src/vita_ecran_boot.c
)

# gnu++98 + -fpermissive : le code d'origine vise MSVC/CodeWarrior d'√©poque.
# On ne le modernise pas, on rend le compilateur tol√©rant. Voir PORTING_NOTES.
# Pas d'optimisation GLOBALE : -O2 comme -O1 font planter ce moteur de 2003
# pendant l'initialisation des modules (dernier repere : ´ apres
# Mth::InitialRand ª). On optimise donc FICHIER PAR FICHIER, en commencant par
# les plus chauds, et on verifie a chaque ajout.
#
# Ce qui est en jeu : Mth::Matrix::operator[] est bien marque ´ inline ª dans
# matrix.inl, mais en -O0 gcc n'inline RIEN. Chaque m[0][c] devient donc deux
# appels de fonction -- d'ou 17,8 ms pour skinner 5000 sommets, soit ~1200
# cycles par sommet quand une cinquantaine suffirait.
#
# CMAKE_BUILD_TYPE etait vide, donc gcc compilait tout en -O0 -- y compris les
# accesseurs de Mth::Matrix, qui devenaient de vrais appels de fonction au lieu
# d'etre inlines. Mesure avant : 17,8 ms pour skinner 5000 sommets, soit ~1200
# cycles par sommet la ou une cinquantaine suffit. Le meme defaut portait sur
# les 73 ms de logique de jeu.
#
# -fno-strict-aliasing : ce moteur date de 2003 et lit reguliement une memoire
# a travers un autre type (unions, casts de tampons). Sans ce drapeau, -O2
# s'autorise des reordonnancements que ce code ne supporte pas.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++98 -fpermissive -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -std=gnu99")

# Chemins du depot remplaces par "." dans le binaire (__FILE__ des assertions,
# infos de debug) : sans cela l'eboot contenait le chemin absolu de la machine
# de build, nom d'utilisateur compris.
get_filename_component(THUG_ROOT_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_compile_options("-ffile-prefix-map=${THUG_ROOT_ABS}=.")

# Build public : cmake -DTHUG_RELEASE=ON. Pas de chien de garde (il plante le
# jeu expres apres 45 s sans log), pas de serveur de debug reseau (port 9990),
# pas de raccourcis de triche tactiles, log reduit au demarrage et aux erreurs.
option(THUG_RELEASE "Build public (outils de debug retires)" OFF)
if(THUG_RELEASE)
  add_compile_definitions(THUG_RELEASE)
endif()

add_compile_options(
  -Wall
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -mfpu=neon
  -mcpu=cortex-a9
  -ffast-math
)

add_compile_definitions(
  # __PLAT_WN32__ fait prendre aux 1598 sites #ifdef le chemin Win32/DX9, le
  # plus complet et le seul maintenu par kisak. __PLAT_VITA__ sert de garde
  # pour diverger aux points durs. Voir PORTING_NOTES.
  __PLAT_WN32__
  __PLAT_VITA__
  VITA
)

# -- Sources du moteur ------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/engine.cmake")

add_executable(${PROJECT_NAME} ${PALIER0_SOURCES} ${THUG_SOURCES})

# --- profileur par instrumentation -----------------------------------------
# Active a la demande : cmake -DTHUG_PROFILE=ON. Le drapeau n'est pose que sur
# les fichiers qu'on soupconne, jamais sur tout le moteur -- instrumenter
# 14000 fonctions fausserait la mesure autant qu'elle l'eclairerait.
#
# vita_prof.c lui-meme ne doit JAMAIS etre instrumente : il s'appellerait
# lui-meme a l'infini.
set_source_files_properties(src/vita_prof.c PROPERTIES
                            COMPILE_OPTIONS "-fno-instrument-functions")

# --- optimisation ciblee ----------------------------------------------------
# Liste a etendre UN FICHIER A LA FOIS, en testant entre chaque : c'est le seul
# moyen d'isoler celui qui ne supporte pas l'optimisation.
# -fno-fast-math : il ANNULE le -ffast-math global, et c'est indispensable ici.
#
# Sans optimisation, -ffast-math ne pouvait pas faire grand mal. Des que -O2
# arrive, il autorise le compilateur a reecrire les calculs flottants
# (associativite, suppression de comparaisons jugees redondantes). Or le
# culling compare des plans de frustum a des boites englobantes : releve sur
# console, les batiments n'apparaissaient plus que dans un rayon proche, devant
# comme derriere -- signature d'un test de visibilite fausse.
#
# Lot 1 (valide) : ˜2 sur la frame entiere, 96,5 -> 42,9 ms.
#
# p_world_render.cpp en a longtemps ete EXCLU : optimise, son culling rejetait
# beaucoup trop (plafond a 975 maillages contre 2451) et les batiments
# n'apparaissaient que dans un rayon proche, devant comme derriere.
#
# [VERIFIE 2026-09-03] le defaut a disparu, le fichier est remis en -O2. Ce qui
# l'a fait disparaitre est vraisemblablement la reecriture de la boucle de rendu
# (6c08520) : l'ancienne testait les memes boites quatre fois par image, dans
# quatre contextes que le compilateur pouvait traiter differemment.
#
# Verifie sur les nombres avant de l'etre a l'oeil : plans du tronc journalises
# (normales unitaires, plans proche et lointain opposes) et taux de rejet
# identique au -O0 -- 2379/4441 contre 2387/4441 a la meme position, 47 % dans
# les deux cas. Puis a l'ecran : le decor porte jusqu'au fond du niveau.
set(THUG_HOT_SOURCES
    "${THUG_ROOT}/Code/Gfx/Vita/p_NxModel.cpp"
    "${THUG_ROOT}/Code/Gfx/Vita/p_world_render.cpp")

# Lot 2 : les maths et la collision. Ce sont eux que ´ logique ª (27 ms) et
# ´ monde ª (10 ms) appellent en boucle, et ils passent par les memes
# accesseurs inlines.
file(GLOB THUG_MATH_SOURCES "${THUG_ROOT}/Code/Core/Math/*.cpp")
file(GLOB THUG_COLL_SOURCES "${THUG_ROOT}/Code/Gel/Collision/*.cpp")
list(APPEND THUG_HOT_SOURCES ${THUG_MATH_SOURCES} ${THUG_COLL_SOURCES})

# Lot 3 : le rendu generique et les composants d'objet. ´ logique ª (28 ms)
# est faite de leurs Update(), ´ monde ª (20 ms) de leurs parcours de scene.
# On prend la RACINE de Gfx seulement : les backends des autres consoles ne
# sont pas compiles.
file(GLOB THUG_GFX_SOURCES  "${THUG_ROOT}/Code/Gfx/*.cpp")
file(GLOB THUG_COMP_SOURCES "${THUG_ROOT}/Code/Gel/Components/*.cpp")
file(GLOB THUG_SKOBJ_SOURCES "${THUG_ROOT}/Code/Sk/Objects/*.cpp")
list(APPEND THUG_HOT_SOURCES ${THUG_GFX_SOURCES} ${THUG_COMP_SOURCES}
                             ${THUG_SKOBJ_SOURCES})

# Lot 4 : la VM de script et la gestion d'objets. Ce sont elles qui restent
# derriere les 13 ms de ´ logique ª -- le moteur interprete des milliers
# d'instructions QB par frame.
file(GLOB THUG_SCRIPT_SOURCES "${THUG_ROOT}/Code/Gel/Scripting/*.cpp")
file(GLOB THUG_OBJ_SOURCES    "${THUG_ROOT}/Code/Gel/Object/*.cpp")
file(GLOB THUG_SKCOMP_SOURCES "${THUG_ROOT}/Code/Sk/Components/*.cpp")
list(APPEND THUG_HOT_SOURCES ${THUG_SCRIPT_SOURCES} ${THUG_OBJ_SOURCES}
                             ${THUG_SKCOMP_SOURCES})

# Lot 5 : le CHARGEMENT. Mesure : 9,2 s passees a enchainer ~1400 etapes de
# 5 a 11 ms -- decompresser, analyser, televerser une piece de pieton apres
# l'autre. Aucun gros poste, que du cout unitaire repete.
#
# Le decodeur LZSS (PRE.cpp) travaille OCTET PAR OCTET : compile sans
# optimisation, c'est le pire cas possible. Le parseur de scenes et le
# chargeur de textures sont dans le meme cas.
file(GLOB THUG_FILE_SOURCES "${THUG_ROOT}/Code/Sys/File/*.cpp")
list(APPEND THUG_HOT_SOURCES ${THUG_FILE_SOURCES}
     "${THUG_ROOT}/Code/Gfx/Vita/p_scene_load.cpp"
     "${THUG_ROOT}/Code/Gfx/Vita/p_NxTexture.cpp")

# Lot 6 : AUDIO. Le mixeur d'effets tourne 200 fois par seconde et le decodeur
# ADPCM traite ~116 000 echantillons par lecture ; en -O0 cela saturait un
# coeur et faisait tomber le jeu de ~37 a 8 images par seconde. Mesure faite,
# pas supposee : trace [PERF] dans p_nx.cpp.
#
# C'est la MEME lecon que pour le moteur : verifier comment un code est
# COMPILE avant de chercher a l'optimiser.
set(THUG_AUDIO_SOURCES
    "${THUG_ROOT}/Code/Core/Vita/adpcm.cpp"
    "${THUG_ROOT}/Code/Gel/Music/Vita/p_music.cpp"
    "${THUG_ROOT}/Code/Gel/SoundFX/Vita/p_sfx.cpp"
)
list(APPEND THUG_HOT_SOURCES ${THUG_AUDIO_SOURCES})
# Lot 7 : le reste du BACKEND VITA, ecrit pour ce portage (pas de code
# d'epoque). [MESURE] p_shader_decor.cpp en -O0 : la transposition de 55
# matrices d'os prenait ~19 us par personnage, et ce meme fichier porte le
# chemin GXM des lots du decor, appele des centaines de fois par image.
list(APPEND THUG_HOT_SOURCES
     "${THUG_ROOT}/Code/Gfx/Vita/p_shader_decor.cpp"
     "${THUG_ROOT}/Code/Gfx/Vita/p_NxSprite.cpp"
     "${THUG_ROOT}/Code/Gfx/Vita/p_NxFont.cpp"
     "${THUG_ROOT}/Code/Gfx/Vita/p_NxLight.cpp"
     "${THUG_ROOT}/Code/Gfx/Vita/p_occlusion.cpp"
     "${THUG_ROOT}/Code/Gfx/Vita/p_nx.cpp"
     # Effets V2 (#45) : ombre portee (~1,5 ms mesure en -O0 sur V1), gamma.
     "${THUG_ROOT}/Code/Gfx/Vita/p_ombre.cpp"
     "${THUG_ROOT}/Code/Gfx/Vita/p_gamma.cpp"
     # Particules parametriques : calcul par particule sur CPU (le XBox le
     # faisait en vertex shader), coupees par defaut (prt 0).
     "${THUG_ROOT}/Code/Gfx/Vita/p_NxNewParticle.cpp"
     # Anciennes particules (sang, etincelles) : sommets calcules sur CPU.
     "${THUG_ROOT}/Code/Gfx/Vita/p_NxParticle.cpp")
# Serveur de debug : la reduction des captures et zlib tournent sur son thread,
# en -O0 une demi-capture coutait ~0,5 s. Module autonome, sans code d'epoque.
list(APPEND THUG_HOT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/vita_dbgsrv.c")
foreach(f ${THUG_HOT_SOURCES})
  if(EXISTS "${f}")
    set_source_files_properties("${f}" PROPERTIES
                                COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-fno-fast-math")
  endif()
endforeach()

if(THUG_PROFILE)
  # Build de profilage (repertoire de build separe, -DTHUG_PROFILE=ON) : la
  # LOGIQUE, instrumentee en gardant son -O2 (sinon on mesure du -O0). Les
  # petites fonctions inlinees des maths et des listes sont exclues : leur
  # instrumentation noierait tout et elles comptent dans leur appelant.
  file(GLOB THUG_PROF_LOGIQUE
       "${THUG_ROOT}/Code/Sk/Components/*.cpp"
       "${THUG_ROOT}/Code/Sk/Objects/*.cpp"
       "${THUG_ROOT}/Code/Sk/Modules/Skate/*.cpp"
       "${THUG_ROOT}/Code/Gel/Components/*.cpp"
       "${THUG_ROOT}/Code/Gel/Object/*.cpp"
       "${THUG_ROOT}/Code/Gel/Scripting/*.cpp")
  foreach(f ${THUG_PROF_LOGIQUE})
    set(opts "-finstrument-functions;-finstrument-functions-exclude-file-list=Core/Math,core/math,Core/List,core/list,Core/HashTable,core/hashtable,Core/support,core/support")
    list(FIND THUG_HOT_SOURCES "${f}" idx)
    if(NOT idx EQUAL -1)
      set(opts "-O2;-fno-strict-aliasing;-fno-fast-math;${opts}")
    endif()
    set_source_files_properties("${f}" PROPERTIES COMPILE_OPTIONS "${opts}")
  endforeach()
  message(STATUS "profileur : instrumentation de la logique")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${THUG_ROOT}/Code"
  "${THUG_ROOT}/Code/Sk"
  "${THUG_ROOT}/Code/Gfx/Vita"
)

# Menu VITA OPTIONS : vita/qb/vita_options.q (script QB ecrit par nous) est
# assemble par vita/tools/qbasm.py en tableau d'octets, vita_options_qb.h,
# genere dans le dossier de build (pas commite : le .q fait foi). Charge
# apres qb.prx par Sk/Scripting/Vita/vita_qb_options.cpp. Sans python3, le
# jeu se construit quand meme, sans ce menu (THUG_SANS_MENU_VITA, log [QB]).
find_package(Python3 COMPONENTS Interpreter)
set(THUG_QB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(THUG_QB_OPTIONS_CPP "${THUG_ROOT}/Code/Sk/Scripting/Vita/vita_qb_options.cpp")
if(Python3_Interpreter_FOUND)
  add_custom_command(
    OUTPUT "${THUG_QB_GEN_DIR}/vita_options_qb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${THUG_QB_GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/qbasm.py" asm
            "${CMAKE_CURRENT_SOURCE_DIR}/qb/vita_options.q"
            -o "${THUG_QB_GEN_DIR}/vita_options.qb"
            --header "${THUG_QB_GEN_DIR}/vita_options_qb.h"
            --symbol vita_options_qb
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qb/vita_options.q"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/qbasm.py"
    COMMENT "qbasm : vita_options.q -> vita_options_qb.h"
    VERBATIM)
  target_sources(${PROJECT_NAME} PRIVATE "${THUG_QB_GEN_DIR}/vita_options_qb.h")
  set_source_files_properties("${THUG_QB_OPTIONS_CPP}" PROPERTIES
                              OBJECT_DEPENDS "${THUG_QB_GEN_DIR}/vita_options_qb.h")
  target_include_directories(${PROJECT_NAME} PRIVATE "${THUG_QB_GEN_DIR}")
else()
  message(WARNING "python3 absent : menu Vita Options non embarque")
  set_source_files_properties("${THUG_QB_OPTIONS_CPP}" PROPERTIES
                              COMPILE_DEFINITIONS THUG_SANS_MENU_VITA)
endif()

# vitaGL recompile a part (options de vitesse) :
#   cmake -DTHUG_VITAGL_DIR=/chemin/vers/vitaGL ...
# Sans l'option, c'est la libvitaGL.a du vitasdk qui est liee. Issue #18.
if(THUG_VITAGL_DIR)
  target_link_directories(${PROJECT_NAME} BEFORE PRIVATE "${THUG_VITAGL_DIR}")
  message(STATUS "vitaGL : ${THUG_VITAGL_DIR}")
endif()

# Espion GL (#48) : appels du jeu qui coupent ou effacent la scene.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=glClear,--wrap=glBindFramebuffer,--wrap=glFinish,--wrap=glFlush,--wrap=glReadPixels,--wrap=glGetQueryObjectuiv,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=memalign,--wrap=sceShaccCgCompileProgram,--wrap=sceGxmShaderPatcherCreateVertexProgram")

# FMV Bink (#3) : FFmpeg minimal (demuxeur bink + decodeurs binkvideo et
# binkaudio), construit par Code/Gel/Movies/Vita/build_ffmpeg_bink.sh dans un
# prefixe A PART (le paquet vdpm ffmpeg n'a pas Bink). Absent : les films
# restent sautes, comme chez kisak. LGPL 2.1+ : lien statique a remplacer par
# un module .suprx avant toute release publique qui l'embarque.
set(THUG_FFMPEG_BINK_DIR "$ENV{HOME}/vita-build/ffmpeg-bink" CACHE PATH "prefixe FFmpeg Bink")
if(EXISTS "${THUG_FFMPEG_BINK_DIR}/lib/libavcodec.a")
  target_sources(${PROJECT_NAME} PRIVATE "${THUG_ROOT}/Code/Gel/Movies/Vita/vita_bink.c")
  target_include_directories(${PROJECT_NAME} PRIVATE "${THUG_FFMPEG_BINK_DIR}/include")
  target_link_directories(${PROJECT_NAME} PRIVATE "${THUG_FFMPEG_BINK_DIR}/lib")
  target_compile_definitions(${PROJECT_NAME} PRIVATE THUG_FMV)
  target_link_libraries(${PROJECT_NAME} avformat avcodec avutil pthread)
  message(STATUS "FMV : FFmpeg Bink ${THUG_FFMPEG_BINK_DIR}")
else()
  message(STATUS "FMV : FFmpeg Bink absent, films sautes")
endif()

target_link_libraries(${PROJECT_NAME}
  vitaGL
  vitashark
  mathneon
  # Ordre impos√© : vitashark appelle sceShaccCgExt*, qui vit dans
  # libSceShaccCgExt.a (et PAS dans SceShaccCg_stub, contrairement √† ce que le
  # nom laisse croire). Inverser les deux casse le link.
  SceShaccCgExt
  SceShaccCg_stub
  taihen_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceMotion_stub
  SceAudio_stub
  SceAudioIn_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  ScePower_stub
  SceKernelDmacMgr_stub
  SceIofilemgr_stub
  SceLibKernel_stub
  SceNet_stub
  SceNetCtl_stub
  # zlib : compression des captures du serveur de debug (vita_dbgsrv.c).
  z
  m
  c
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
# LiveArea et ecran de lancement : generes depuis le dump du jeu par
# vita/livearea/generer.py (PNG 8 bits indexes ; fichiers ignores par git). Apres une mise a jour de ces fichiers,
# reinstaller le VPK (ou VitaShell : Refresh LiveArea) -- pousser eboot.bin par
# FTP ne touche pas a la bulle, que le systeme copie dans ur0:appmeta.
# Sans ces fichiers (generes depuis les donnees du jeu), le VPK se construit
# quand meme, avec la bulle par defaut du systeme et sans ecran de lancement.
set(THUG_VPK_FICHIERS)
set(THUG_GENERES
  sce_sys/icon0.png sce_sys/pic0.png
  sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/startup.png
  boot/ecran.bin)
set(THUG_GENERES_OK TRUE)
foreach(f ${THUG_GENERES})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
    set(THUG_GENERES_OK FALSE)
  endif()
endforeach()
if(THUG_GENERES_OK)
  foreach(f ${THUG_GENERES} sce_sys/livearea/contents/template.xml)
    list(APPEND THUG_VPK_FICHIERS FILE ${f} ${f})
  endforeach()
else()
  message(WARNING "LiveArea absente (vita/livearea/generer.py) : VPK avec la bulle par defaut")
endif()
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME    ${VITA_APP_NAME}
  ${THUG_VPK_FICHIERS}
)

# -- Boucle de test : make send / make kill / make log ----------------------
# L'IP est surchargeable : cmake -DVITA_IP=192.168.1.x
# L'adresse suit le reseau. Priorite : -DVITA_IP=..., puis le fichier
# vita/tools/vita_ip.txt (le meme que les outils Python), puis le defaut.
if(NOT DEFINED VITA_IP)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/vita_ip.txt")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tools/vita_ip.txt" VITA_IP)
    string(STRIP "${VITA_IP}" VITA_IP)
  else()
    set(VITA_IP "192.168.1.100")
  endif()
endif()
message(STATUS "console visee : ${VITA_IP}")

# It√©ration rapide : on n'√©crase que eboot.bin, pas de r√©installation de VPK.
add_custom_target(send
  COMMAND echo destroy | nc -w2 ${VITA_IP} 1338 || true
  COMMAND sleep 1
  COMMAND curl -s --max-time 90 -T eboot.bin
          ftp://${VITA_IP}:1337/ux0:/app/${VITA_TITLEID}/eboot.bin
  COMMAND echo launch ${VITA_TITLEID} | nc -w2 ${VITA_IP} 1338
  DEPENDS eboot.bin
  COMMENT "push eboot + launch ${VITA_TITLEID} sur ${VITA_IP}"
)

add_custom_target(kill
  COMMAND echo destroy | nc -w2 ${VITA_IP} 1338 || true
  COMMENT "kill l'app sur la console"
)
