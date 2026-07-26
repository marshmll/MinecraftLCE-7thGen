#include "stdafx.h"
#include <iostream>
#include "ArrayWithLength.h"
#include "InputOutputStream.h"
#include "PacketListener.h"
#include "RemoveEntitiesPacket.h"

RemoveEntitiesPacket::RemoveEntitiesPacket()
{
}

RemoveEntitiesPacket::RemoveEntitiesPacket(intArray ids) 
{
	this->ids = ids;
}

RemoveEntitiesPacket::~RemoveEntitiesPacket()
{
	delete ids.data;
}

void RemoveEntitiesPacket::read(DataInputStream *dis) //throws IOException 
{
	ids = intArray(dis->readByte());
	for(unsigned int i = 0; i < ids.length; ++i)
	{
		ids[i] = dis->readInt();
	}
}

void RemoveEntitiesPacket::write(DataOutputStream *dos) //throws IOException 
{
	dos->writeByte(ids.length);
	for(unsigned int i = 0; i < ids.length; ++i)
	{
		dos->writeInt(ids[i]);
	}
}

void RemoveEntitiesPacket::handle(PacketListener *listener)
{
	listener->handleRemoveEntity(shared_from_this());
}

int RemoveEntitiesPacket::getEstimatedSize()
{
	return 1 + (ids.length * 4);
}

/*
	4J: These are necesary on the PS3.
		(and 4).
*/
// GCC (Linux) enforces the same strict ODR rule these consoles' toolchains
// do - widened rather than left PS3/Orbis/PSVita-only (see Tile.cpp's
// identical fix for the same underlying gap).
#if (defined __PS3__ || defined __ORBIS__ || defined __PSVITA__ || defined _LINUX64)
const int RemoveEntitiesPacket::MAX_PER_PACKET;
#endif
