/*
 * MacroQuest2: The extension platform for EverQuest
 * Copyright (C) 2002-2019 MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "pch.h"
#include "MQ2DataTypes.h"

namespace mq {
namespace datatypes {

MQ2ZoneType::MQ2ZoneType() : MQ2Type("zone")
{
	ScopedTypeMember(ZoneMembers, Name);
	ScopedTypeMember(ZoneMembers, ShortName);
	ScopedTypeMember(ZoneMembers, ID);
	ScopedTypeMember(ZoneMembers, Address);
	ScopedTypeMember(ZoneMembers, ZoneFlags);
}

bool MQ2ZoneType::GetMember(MQVarPtr VarPtr, char* Member, char* Index, MQTypeVar& Dest)
{
	EQZoneInfo* pZone = static_cast<EQZoneInfo*>(VarPtr.Ptr);
	if (!VarPtr.Ptr)
		return false;

	MQTypeMember* pMember = MQ2ZoneType::FindMember(Member);
	if (!pMember)
		return false;

	switch (static_cast<ZoneMembers>(pMember->ID))
	{
	case ZoneMembers::Address:
		Dest.DWord = (DWORD)VarPtr.Ptr;
		Dest.Type = pIntType;
		return true;

	case ZoneMembers::Name:
		strcpy_s(DataTypeTemp, pZone->LongName);
		Dest.Ptr = &DataTypeTemp;
		Dest.Type = pStringType;
		return true;

	case ZoneMembers::ShortName:
		strcpy_s(DataTypeTemp, pZone->ShortName);
		Dest.Ptr = &DataTypeTemp[0];
		Dest.Type = pStringType;
		return true;

	case ZoneMembers::ID:
		Dest.Int = pZone->Id & 0x7FFF;
		Dest.Type = pIntType;
		return true;

	case ZoneMembers::ZoneFlags:
		Dest.UInt64 = pZone->ZoneFlags;
		Dest.Type = pInt64Type;
		return true;

	default: break;
	}

	return false;
}

bool MQ2ZoneType::ToString(MQVarPtr VarPtr, char* Destination)
{
	EQZoneInfo* pZoneInfo = static_cast<EQZoneInfo*>(VarPtr.Ptr);

	strcpy_s(Destination, MAX_STRING, pZoneInfo->LongName);
	return true;
}

bool MQ2ZoneType::FromData(MQVarPtr& VarPtr, MQTypeVar& Source)
{
	if (Source.Type == pZoneType)
	{
		VarPtr.Ptr = Source.Ptr;
		return true;
	}

	if (Source.Type == (MQ2Type*)pCurrentZoneType)
	{
		if (CHARINFO* pChar = GetCharInfo())
		{
			int zoneid = (pChar->zoneId & 0x7FFF);
			if (zoneid <= MAX_ZONES)
			{
				VarPtr.Ptr = &pWorldData->ZoneArray[zoneid];
				return true;
			}
		}
	}

	return false;
}

}} // namespace mq::datatypes