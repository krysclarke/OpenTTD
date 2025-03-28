/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file vehiclelist.cpp Lists of vehicles. */

#include "stdafx.h"
#include "train.h"
#include "vehicle_func.h"
#include "vehiclelist.h"
#include "vehiclelist_func.h"
#include "group.h"

#include "safeguards.h"

/**
 * Pack a VehicleListIdentifier in 32 bits so it can be used as unique WindowNumber.
 * @return The window number.
 */
WindowNumber VehicleListIdentifier::ToWindowNumber() const
{
	uint8_t c = this->company == OWNER_NONE ? 0xF : this->company.base();
	assert(c             < (1 <<  4));
	assert(this->vtype   < (1 <<  2));
	assert(this->index   < (1 << 20));
	assert(this->type    < VLT_END);
	static_assert(VLT_END <= (1 <<  3));

	return c << 28 | this->type << 23 | this->vtype << 26 | this->index;
}

/** Data for building a depot vehicle list. */
struct BuildDepotVehicleListData
{
	VehicleList *engines; ///< Pointer to list to add vehicles to.
	VehicleList *wagons; ///< Pointer to list to add wagons to (can be nullptr).
	VehicleType type; ///< Type of vehicle.
	bool individual_wagons; ///< If true add every wagon to \a wagons which is not attached to an engine. If false only add the first wagon of every row.
};

/**
 * Add vehicles to a depot vehicle list.
 * @param v The found vehicle.
 * @param data The depot vehicle list data.
 * @return Always nullptr.
 */
static Vehicle *BuildDepotVehicleListProc(Vehicle *v, void *data)
{
	auto bdvld = static_cast<BuildDepotVehicleListData *>(data);
	if (v->type != bdvld->type || !v->IsInDepot()) return nullptr;

	if (bdvld->type == VEH_TRAIN) {
		const Train *t = Train::From(v);
		if (t->IsArticulatedPart() || t->IsRearDualheaded()) return nullptr;
		if (bdvld->wagons != nullptr && t->First()->IsFreeWagon()) {
			if (bdvld->individual_wagons || t->IsFreeWagon()) bdvld->wagons->push_back(t);
			return nullptr;
		}
	}

	if (v->IsPrimaryVehicle()) bdvld->engines->push_back(v);
	return nullptr;
};

/**
 * Generate a list of vehicles inside a depot.
 * @param type    Type of vehicle
 * @param tile    The tile the depot is located on
 * @param engines Pointer to list to add vehicles to
 * @param wagons  Pointer to list to add wagons to (can be nullptr)
 * @param individual_wagons If true add every wagon to \a wagons which is not attached to an engine. If false only add the first wagon of every row.
 */
void BuildDepotVehicleList(VehicleType type, TileIndex tile, VehicleList *engines, VehicleList *wagons, bool individual_wagons)
{
	engines->clear();
	if (wagons != nullptr && wagons != engines) wagons->clear();

	BuildDepotVehicleListData bdvld{engines, wagons, type, individual_wagons};
	FindVehicleOnPos(tile, &bdvld, BuildDepotVehicleListProc);
}

/**
 * Generate a list of vehicles based on window type.
 * @param list Pointer to list to add vehicles to
 * @param vli  The identifier of this vehicle list.
 * @return false if invalid list is requested
 */
bool GenerateVehicleSortList(VehicleList *list, const VehicleListIdentifier &vli)
{
	list->clear();

	switch (vli.type) {
		case VL_STATION_LIST:
			FindVehiclesWithOrder(
				[&vli](const Vehicle *v) { return v->type == vli.vtype; },
				[&vli](const Order *order) { return (order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_WAYPOINT) || order->IsType(OT_IMPLICIT)) && order->GetDestination() == vli.ToStationID(); },
				[&list](const Vehicle *v) { list->push_back(v); }
			);
			break;

		case VL_SHARED_ORDERS: {
			/* Add all vehicles from this vehicle's shared order list */
			const Vehicle *v = Vehicle::GetIfValid(vli.ToVehicleID());
			if (v == nullptr || v->type != vli.vtype || !v->IsPrimaryVehicle()) return false;

			for (; v != nullptr; v = v->NextShared()) {
				list->push_back(v);
			}
			break;
		}

		case VL_GROUP_LIST:
			if (vli.ToGroupID() != ALL_GROUP) {
				for (const Vehicle *v : Vehicle::Iterate()) {
					if (v->type == vli.vtype && v->IsPrimaryVehicle() &&
							v->owner == vli.company && GroupIsInGroup(v->group_id, vli.ToGroupID())) {
						list->push_back(v);
					}
				}
				break;
			}
			[[fallthrough]];

		case VL_STANDARD:
			for (const Vehicle *v : Vehicle::Iterate()) {
				if (v->type == vli.vtype && v->owner == vli.company && v->IsPrimaryVehicle()) {
					list->push_back(v);
				}
			}
			break;

		case VL_DEPOT_LIST:
			FindVehiclesWithOrder(
				[&vli](const Vehicle *v) { return v->type == vli.vtype; },
				[&vli](const Order *order) { return order->IsType(OT_GOTO_DEPOT) && !(order->GetDepotActionType() & ODATFB_NEAREST_DEPOT) && order->GetDestination() == vli.ToDestinationID(); },
				[&list](const Vehicle *v) { list->push_back(v); }
			);
			break;

		default: return false;
	}

	return true;
}
