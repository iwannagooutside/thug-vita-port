; vita_options.q -- menu VITA OPTIONS du port PS Vita (reglages propres au port).
;
; Assemble par vita/tools/qbasm.py au build (vita/CMakeLists.txt), embarque
; dans l'eboot et charge APRES qb.prx par Script::LoadQBFromMemory
; (Code/Sk/Scripting/Vita/vita_qb_options.cpp, appele en fin de
; SkateScript::LoadAllStartupQBFiles). Un symbole deja defini est remplace
; (parse.cpp, ParseQB). On ne livre aucun script du jeu modifie : les joueurs
; gardent leur ISO.
;
; Syntaxe : voir l'en-tete de qbasm.py. Un saut de ligne = ENDOFLINE ; les
; lignes vides et les commentaires (';' ou '//') ne produisent rien.
;
; Chemin dans le jeu :
;   menu principal : OPTIONS > Control Setup > Vita Options
;   menu pause     : OPTIONS > Control Setup (Player 1) > Vita Options
;
; Garde (verifiee par le C++ avant de charger quoi que ce soit) :
;   %replaces : le script remplace doit etre EXACTEMENT celui de l'ISO USA
;               (somme de contenu = CalculateScriptContentsChecksum, valeur
;               donnee par 'qbasm.py sum <qb.prx> gamemenu.qb <script>').
;   %needs    : scripts et globales du jeu appeles ici, qui doivent exister.

%replaces create_controller_config_menu 0x5882e7f4
%needs make_new_themed_sub_menu theme_menu_add_item finish_themed_sub_menu
%needs create_helper_text kill_start_key_binding generic_menu_pad_choose
%needs generic_menu_pad_back controller_config_focus controller_config_unfocus
%needs THEME_PREFIXES THEME_COLOR_PREFIXES current_theme_prefix

; ---------------------------------------------------------------------------
; create_controller_config_menu : COPIE CONFORME de gamemenu.qb (ISO USA, meme
; texte que Scripts/game/menu/gamemenu.q:6943), plus les deux blocs encadres
; par "; VITA {" et "; VITA }" (une entree "Vita Options" avant chaque "Done"
; des branches Player 1 et liste des manettes). qbasm.py VERIFIE au build que
; la copie privee de ces blocs redonne la somme %replaces de l'original : ne
; rien modifier hors des blocs.
; ---------------------------------------------------------------------------
script create_controller_config_menu
	if ObjectExists id = current_menu_anchor
		DestroyScreenElement id = current_menu_anchor
	endif
	if GotParam controller_number
		if ( <controller_number> = -1 )
			FormatText textname = menu_title "COMMON" n = <controller_number>
		else
			FormatText textname = menu_title "PLAYER %n" n = <controller_number>
		endif
		helper_text = { helper_text_elements = [ { text = "\b7/\b4 = Select" }
				{ text = "\b6/\b5 = Adjust" }
				{ text = "\m1 = Back" }
				{ text = "\m0 = Accept" }
			]
		}
	else
		menu_title = "CONTROL SETUP"
		helper_text = { helper_text_elements = [ { text = "\b7/\b4 = Select" }
				{ text = "\m1 = Back" }
				{ text = "\m0 = Accept" }
			]
		}
	endif
	FormatText ChecksumName = title_icon "%i_control" i = ( THEME_PREFIXES [ current_theme_prefix ] )
	make_new_themed_sub_menu title = <menu_title> title_icon = <title_icon>
	if LevelIs load_skateshop
		build_top_and_bottom_blocks
		make_mainmenu_3d_plane
		change joystick_pushed = 0
		control_options_graphic
		SetScreenElementProps { id = sub_vmenu event_handlers = [
				{ pad_up animate_joystick params = { dir = up } }
				{ pad_down animate_joystick params = { dir = down } }
			]
		}
	endif
	kill_start_key_binding
	if isngc
		<vibration_text> = "Rumble"
	else
		<vibration_text> = "Vibration"
	endif
	if GotParam from_options
		<from_options> = from_options
	endif
	if GotParam controller_number
		if GotParam from_options
			SetScreenElementProps { id = sub_menu
				event_handlers = [
					{ pad_back generic_menu_pad_back params = { callback = controller_config_exit from_options = <from_options> } }
				]
			}
		else
			SetScreenElementProps { id = sub_menu
				event_handlers = [
					{ pad_back generic_menu_pad_back params = { callback = create_controller_config_menu from_options = <from_options> } }
				]
			}
		endif
		if not ( <controller_number> = -1 )
			theme_menu_add_item { text = <vibration_text>
				id = menu_vibration
				focus_script = controller_config_focus
				unfocus_script = controller_config_unfocus
				pad_choose_script = nullscript
			}
			theme_menu_add_item { text = "Autokick"
				id = menu_autokick
				focus_script = controller_config_focus
				unfocus_script = controller_config_unfocus
				pad_choose_script = nullscript
			}
			theme_menu_add_item { text = "180 Spin Taps"
				id = menu_spintaps
				focus_script = controller_config_focus
				unfocus_script = controller_config_unfocus
				pad_choose_script = nullscript
			}
		endif
		if ( <controller_number> = 1 )
			if not LevelIs load_skateshop
				if ( ( InNetGame ) && ( GetGlobalFlag flag = FLAG_G_EXPERT_MODE_NO_REVERTS ) )
					theme_menu_add_item { text = "No Reverts"
						id = menu_reverts
						focus_script = controller_config_focus
						unfocus_script = controller_config_unfocus
						pad_choose_script = nullscript
						not_focusable = not_focusable
					}
				else
					theme_menu_add_item { text = "No Reverts"
						id = menu_reverts
						focus_script = controller_config_focus
						unfocus_script = controller_config_unfocus
						pad_choose_script = nullscript
					}
				endif
				if ( ( InNetGame ) && ( GetGlobalFlag flag = FLAG_G_EXPERT_MODE_NO_MANUALS ) )
					theme_menu_add_item { text = "No Manuals"
						id = menu_manuals
						focus_script = controller_config_focus
						unfocus_script = controller_config_unfocus
						pad_choose_script = nullscript
						not_focusable = not_focusable
					}
				else
					theme_menu_add_item { text = "No Manuals"
						id = menu_manuals
						focus_script = controller_config_focus
						unfocus_script = controller_config_unfocus
						pad_choose_script = nullscript
					}
				endif
				if ( ( InNetGame ) && ( GetGlobalFlag flag = FLAG_G_EXPERT_MODE_NO_WALKING ) )
					theme_menu_add_item { text = "No Walking"
						id = menu_walking
						focus_script = controller_config_focus
						unfocus_script = controller_config_unfocus
						pad_choose_script = nullscript
						not_focusable = not_focusable
					}
				else
					theme_menu_add_item { text = "No Walking"
						id = menu_walking
						focus_script = controller_config_focus
						unfocus_script = controller_config_unfocus
						pad_choose_script = nullscript
					}
				endif
			endif
		endif
		if ( <controller_number> = -1 )
			if ( ( InNetGame ) && ( GetGlobalFlag flag = FLAG_G_EXPERT_MODE_NO_REVERTS ) )
				theme_menu_add_item { text = "No Reverts"
					id = menu_reverts
					focus_script = controller_config_focus
					unfocus_script = controller_config_unfocus
					pad_choose_script = nullscript
					not_focusable = not_focusable
				}
			else
				theme_menu_add_item { text = "No Reverts"
					id = menu_reverts
					focus_script = controller_config_focus
					unfocus_script = controller_config_unfocus
					pad_choose_script = nullscript
				}
			endif
			if ( ( InNetGame ) && ( GetGlobalFlag flag = FLAG_G_EXPERT_MODE_NO_MANUALS ) )
				theme_menu_add_item { text = "No Manuals"
					id = menu_manuals
					focus_script = controller_config_focus
					unfocus_script = controller_config_unfocus
					pad_choose_script = nullscript
					not_focusable = not_focusable
				}
			else
				theme_menu_add_item { text = "No Manuals"
					id = menu_manuals
					focus_script = controller_config_focus
					unfocus_script = controller_config_unfocus
					pad_choose_script = nullscript
				}
			endif
			if ( ( InNetGame ) && ( GetGlobalFlag flag = FLAG_G_EXPERT_MODE_NO_WALKING ) )
				theme_menu_add_item { text = "No Walking"
					id = menu_walking
					focus_script = controller_config_focus
					unfocus_script = controller_config_unfocus
					pad_choose_script = nullscript
					not_focusable = not_focusable
				}
			else
				theme_menu_add_item { text = "No Walking"
					id = menu_walking
					focus_script = controller_config_focus
					unfocus_script = controller_config_unfocus
					pad_choose_script = nullscript
				}
			endif
		endif
		; VITA { entree du menu des reglages du port (Player 1 / menu pause)
		if ( <controller_number> = 1 )
			theme_menu_add_item { text = "Vita Options"
				id = menu_vita_options
				pad_choose_script = generic_menu_pad_choose
				pad_choose_params = { callback = create_vita_options_menu controller_number = <controller_number> from_options = <from_options> }
			}
		endif
		; VITA }
		if GotParam from_options
			theme_menu_add_item { text = "Done"
				id = menu_done
				pad_choose_script = generic_menu_pad_choose
				pad_choose_params = { callback = controller_config_exit from_options = <from_options> }
				last_menu_item = last_menu_item
			}
		else
			theme_menu_add_item { text = "Done"
				id = menu_done
				pad_choose_script = generic_menu_pad_choose
				pad_choose_params = { callback = create_controller_config_menu from_options = <from_options> }
				last_menu_item = last_menu_item
			}
		endif
		control_config_show_values controller_number = <controller_number>
	else
		SetScreenElementProps { id = sub_menu
			event_handlers = [
				{ pad_back generic_menu_pad_back params = { callback = controller_config_exit from_options = <from_options> } }
			]
		}
		theme_menu_add_item { text = "Player 1"
			id = menu_controller_1
			pad_choose_script = generic_menu_pad_choose
			pad_choose_params = { callback = create_controller_config_menu controller_number = 1 from_options = <from_options> }
			centered
		}
		theme_menu_add_item { text = "Player 2"
			id = menu_controller_2
			pad_choose_script = generic_menu_pad_choose
			pad_choose_params = { callback = create_controller_config_menu controller_number = 2 from_options = <from_options> }
			centered
		}
		theme_menu_add_item { text = "Common"
			id = menu_controller_x
			pad_choose_script = generic_menu_pad_choose
			pad_choose_params = { callback = create_controller_config_menu controller_number = -1 from_options = <from_options> }
			centered
		}
		; VITA { entree du menu des reglages du port (menu principal)
		theme_menu_add_item { text = "Vita Options"
			id = menu_vita_options
			pad_choose_script = generic_menu_pad_choose
			pad_choose_params = { callback = create_vita_options_menu from_options = <from_options> }
			centered
		}
		; VITA }
		theme_menu_add_item { text = "Done"
			id = menu_done
			pad_choose_script = generic_menu_pad_choose
			pad_choose_params = { callback = controller_config_exit from_options = <from_options> }
			last_menu_item = last_menu_item
			centered
		}
	endif
	finish_themed_sub_menu
	create_helper_text <helper_text>
endscript

; ---------------------------------------------------------------------------
; Menu VITA OPTIONS. Presentation calquee sur CONTROL SETUP > Player 1
; (control_config_show_values) : valeur au centre (enfant 4 de l'element),
; fleches gauche/droite (enfants 5 et 6) allumees par controller_config_focus.
; Gauche, droite et CROIX basculent la valeur ; le C++ applique tout de suite
; et reecrit ux0:data/thug/controls.txt.
; ---------------------------------------------------------------------------
script create_vita_options_menu
	if ObjectExists id = current_menu_anchor
		DestroyScreenElement id = current_menu_anchor
	endif
	helper_text = { helper_text_elements = [ { text = "\b7/\b4 = Select" }
			{ text = "\b6/\b5 = Adjust" }
			{ text = "\m1 = Back" }
			{ text = "\m0 = Accept" }
		]
	}
	FormatText ChecksumName = title_icon "%i_control" i = ( THEME_PREFIXES [ current_theme_prefix ] )
	make_new_themed_sub_menu title = "VITA OPTIONS" title_icon = <title_icon>
	; Meme decor que CONTROL SETUP au menu principal : les scripts de focus du
	; theme y touchent ces elements (sans eux : SetScreenElementProps sur un
	; element absent, plantage a l'ouverture constate le 2026-10-10).
	if LevelIs load_skateshop
		build_top_and_bottom_blocks
		make_mainmenu_3d_plane
		change joystick_pushed = 0
		control_options_graphic
		SetScreenElementProps { id = sub_vmenu event_handlers = [
				{ pad_up animate_joystick params = { dir = up } }
				{ pad_down animate_joystick params = { dir = down } }
			]
		}
	endif
	kill_start_key_binding
	if GotParam from_options
		<from_options> = from_options
	endif
	SetScreenElementProps { id = sub_menu
		event_handlers = [
			{ pad_back generic_menu_pad_back params = { callback = create_controller_config_menu controller_number = <controller_number> from_options = <from_options> } }
		]
	}
	vita_options_add_item text = "Invert Left X" id = menu_vita_inv_lx name = invert_left_x
	vita_options_add_item text = "Invert Left Y" id = menu_vita_inv_ly name = invert_left_y
	vita_options_add_item text = "Invert Camera X" id = menu_vita_inv_rx name = invert_right_x
	vita_options_add_item text = "Invert Camera Y" id = menu_vita_inv_ry name = invert_right_y
	vita_options_add_item text = "L/R Buttons" id = menu_vita_lr name = triggers_as_l2r2
	vita_options_add_item text = "Touch Buttons" id = menu_vita_touch name = touch_on_rear_pad
	vita_options_add_item text = "Frame Rate" id = menu_vita_fps name = framerate
	theme_menu_add_item { text = "Done"
		id = menu_done
		pad_choose_script = generic_menu_pad_choose
		pad_choose_params = { callback = create_controller_config_menu controller_number = <controller_number> from_options = <from_options> }
		last_menu_item = last_menu_item
	}
	finish_themed_sub_menu
	create_helper_text <helper_text>
endscript

; Une ligne de reglage : libelle, valeur courante (VitaOptGet rend value et
; value_text), fleches, et la bascule sur gauche / droite / croix.
script vita_options_add_item
	theme_menu_add_item { text = <text>
		id = <id>
		focus_script = controller_config_focus
		unfocus_script = controller_config_unfocus
		pad_choose_script = vita_options_change
		pad_choose_params = { id = <id> name = <name> }
	}
	VitaOptGet name = <name>
	FormatText ChecksumName = text_color "%i_UNHIGHLIGHTED_TEXT_COLOR" i = ( THEME_COLOR_PREFIXES [ current_theme_prefix ] )
	CreateScreenElement {
		type = TextElement
		parent = <id>
		font = small
		just = [ center top ]
		pos = (142.0, -17.0)
		text = <value_text>
		rgba = <text_color>
	}
	CreateScreenElement {
		type = SpriteElement
		parent = <id>
		texture = left_arrow
		rgba = [ 128 128 128 0 ]
		pos = (122.0, -17.0)
		just = [ right top ]
		scale = 0.75
	}
	CreateScreenElement {
		type = SpriteElement
		parent = <id>
		texture = right_arrow
		rgba = [ 128 128 128 0 ]
		pos = (162.0, -17.0)
		just = [ left top ]
		scale = 0.75
	}
	SetScreenElementProps {
		id = <id>
		event_handlers = [ { pad_left vita_options_change params = { id = <id> name = <name> } }
			{ pad_right vita_options_change params = { id = <id> name = <name> } }
		]
		replace_handlers
	}
endscript

; Bascule le reglage (deux valeurs chacun), applique et enregistre (C++),
; puis affiche la nouvelle valeur.
script vita_options_change
	VitaOptToggle name = <name>
	SetScreenElementProps id = { <id> child = 4 } text = <value_text>
endscript
