/*****************************************************************************
**  THUG-Vita — couche système                                              **
**  Code/Sys/SIO/Vita/p_sioman.cpp                                          **
**                                                                          **
**  Gestionnaire de peripheriques d'entree.                                 **
**                                                                          **
**  Remplace le singleton NEUTRE de p_singletons.cpp : celui-ci rendait un  **
**  bloc mis a zero, ce qui suffisait a ne pas planter mais garantissait     **
**  qu'aucune touche ne serait jamais lue. Le menu ne pouvait donc pas       **
**  avancer, et sans menu, pas de skater.                                   **
**                                                                          **
**  Un seul port : la Vita n'a qu'une manette integree.                      **
*****************************************************************************/

#include <core/defines.h>
#include <sys/sioman.h>
#include <sys/siodev.h>
#include <sys/mem/memman.h>

#include <gel/module.h>
#include <gel/music/music.h>

#include "vita_log.h"

namespace SIO
{

void Manager::process_devices( const Tsk::Task< Manager::DeviceList >& task )
{
	Lst::Search< Device >	sh;
	Manager::DeviceList&	device_list = task.GetData();

	Device *device = sh.FirstItem( device_list );
	while( device )
	{
		device->process();
		device = sh.NextItem();
	}
}


// Comme XBox/p_sioman.cpp:202 : lecture des manettes hors de la boucle de
// taches, pour les boucles bloquantes (lecture des films, Gel/Movies/Vita).
void Manager::ProcessDevices( void )
{
	Lst::Search< Device >	sh;
	Device *device = sh.FirstItem( m_devices );
	while( device )
	{
		device->process();
		device = sh.NextItem();
	}
}

Device *Manager::create_device( int index, int port, int slot )
{
	return new Device( index, port, slot );
}


Manager::Manager( void )
{
	m_process_devices_task = new Tsk::Task< DeviceList >( Manager::process_devices,
	                                                      m_devices );

	// [REFUTE] « un seul peripherique suffit, la Vita n'a qu'une manette ».
	// Le moteur interroge les DEUX ports et ne verifie pas le NULL :
	// Data abort dans Device::IsPluggedIn avec this=0, constate sur psp2core.
	// On cree donc vMAX_PORT peripheriques ; seul le port 0 lit vraiment le
	// materiel, les autres se declarent debranches (voir read_data).
	for( int port = 0; port < SIO::vMAX_PORT; ++port )
	{
		Device *p_device = create_device( port, port, 0 );
		m_devices.AddToTail( p_device->m_node );
		p_device->Acquire();
	}

	VLOG( "PAD", "gestionnaire de manette initialise (%d ports, seul le 0 est reel)",
	      (int)SIO::vMAX_PORT );

	// L'audio s'initialise ICI, et pas ailleurs, parce que c'est ici que le
	// jeu d'origine le fait : Sys/SIO/XBox/p_sioman.cpp:153, a la fin de
	// l'enumeration des peripheriques. Rien ne relie logiquement les manettes
	// au son -- c'est un heritage de la PS2, ou les deux passaient par l'IOP
	// (voir sioman.cpp:417, entoure de LoadIRX et sceSifInitIopHeap).
	//
	// Consequence a connaitre : le seul appel a Pcm::Init() vivait dans du
	// code PS2/Xbox jamais compile pour nous. Le backend musique pouvait donc
	// etre parfaitement fonctionnel sans qu'une seule note ne sorte, et sans
	// la moindre trace -- PCMAudio_Init n'etant jamais atteint.
	if( !Pcm::NoMusicPlease())
	{
		Pcm::Init();
	}
}


Manager::~Manager( void )
{
	Lst::Search< Device >	sh;
	Device *device = sh.FirstItem( m_devices );
	while( device )
	{
		Device *next = sh.NextItem();
		delete device;
		device = next;
	}
	delete m_process_devices_task;
}


Device *Manager::GetDevice( int port, int slot )
{
	Lst::Search< Device >	sh;
	Device *device = sh.FirstItem( m_devices );
	while( device )
	{
		if(( device->GetPort() == port ) && ( device->GetSlot() == slot ))
			return device;
		device = sh.NextItem();
	}
	return NULL;
}


Device *Manager::GetDeviceByIndex( int index )
{
	Lst::Search< Device >	sh;
	Device *device = sh.FirstItem( m_devices );
	while( device )
	{
		if( device->GetIndex() == index )
			return device;
		device = sh.NextItem();
	}
	return NULL;
}


void Manager::Pause( void )
{
}


void Manager::UnPause( void )
{
}

} // namespace SIO

// Le trio sSgltnInstance / sSgltnDelete / sp_sgltn_instance impose par
// DeclareSingletonClass. Version REELLE : elle construit le gestionnaire a la
// premiere demande, contrairement au bloc neutre de p_singletons.cpp.
DefineSingletonClass( SIO::Manager, "SIO Manager" );
