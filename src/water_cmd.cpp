/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file water_cmd.cpp Handling of water tiles. */

#include "stdafx.h"
#include "landscape.h"
#include "viewport_func.h"
#include "command_func.h"
#include "town.h"
#include "news_func.h"
#include "depot_base.h"
#include "depot_func.h"
#include "water.h"
#include "industry_map.h"
#include "newgrf_canal.h"
#include "strings_func.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "company_func.h"
#include "clear_map.h"
#include "tree_map.h"
#include "aircraft.h"
#include "effectvehicle_func.h"
#include "tunnelbridge_map.h"
#include "station_base.h"
#include "ai/ai.hpp"
#include "game/game.hpp"
#include "core/random_func.hpp"
#include "core/backup_type.hpp"
#include "timer/timer_game_calendar.h"
#include "company_base.h"
#include "company_gui.h"
#include "newgrf_generic.h"
#include "industry.h"
#include "water_cmd.h"
#include "landscape_cmd.h"
#include "pathfinder/water_regions.h"

#include "table/strings.h"

#include "safeguards.h"

/**
 * Describes from which directions a specific slope can be flooded (if the tile is floodable at all).
 */
static const Directions _flood_from_dirs[] = {
	{DIR_NW, DIR_SW, DIR_SE, DIR_NE}, // SLOPE_FLAT
	{DIR_NE, DIR_SE},                 // SLOPE_W
	{DIR_NW, DIR_NE},                 // SLOPE_S
	{DIR_NE},                         // SLOPE_SW
	{DIR_NW, DIR_SW},                 // SLOPE_E
	{},                               // SLOPE_EW
	{DIR_NW},                         // SLOPE_SE
	{DIR_N, DIR_NW, DIR_NE},          // SLOPE_WSE, SLOPE_STEEP_S
	{DIR_SW, DIR_SE},                 // SLOPE_N
	{DIR_SE},                         // SLOPE_NW
	{},                               // SLOPE_NS
	{DIR_E, DIR_NE, DIR_SE},          // SLOPE_NWS, SLOPE_STEEP_W
	{DIR_SW},                         // SLOPE_NE
	{DIR_S, DIR_SW, DIR_SE},          // SLOPE_ENW, SLOPE_STEEP_N
	{DIR_W, DIR_SW, DIR_NW},          // SLOPE_SEN, SLOPE_STEEP_E
};

/**
 * Marks tile dirty if it is a canal or river tile.
 * Called to avoid glitches when flooding tiles next to canal tile.
 *
 * @param tile tile to check
 */
static inline void MarkTileDirtyIfCanalOrRiver(TileIndex tile)
{
	if (IsValidTile(tile) && IsTileType(tile, MP_WATER) && (IsCanal(tile) || IsRiver(tile))) MarkTileDirtyByTile(tile);
}

/**
 * Marks the tiles around a tile as dirty, if they are canals or rivers.
 *
 * @param tile The center of the tile where all other tiles are marked as dirty
 * @ingroup dirty
 */
static void MarkCanalsAndRiversAroundDirty(TileIndex tile)
{
	for (Direction dir = DIR_BEGIN; dir < DIR_END; dir++) {
		MarkTileDirtyIfCanalOrRiver(tile + TileOffsByDir(dir));
	}
}

/**
 * Clear non-flooding state of the tiles around a tile.
 * @param tile The centre of the tile where other tiles' non-flooding state is cleared.
 */
void ClearNeighbourNonFloodingStates(TileIndex tile)
{
	for (Direction dir = DIR_BEGIN; dir != DIR_END; dir++) {
		TileIndex dest = tile + TileOffsByDir(dir);
		if (IsValidTile(dest) && IsTileType(dest, MP_WATER)) SetNonFloodingWaterTile(dest, false);
	}
}

/**
 * Build a ship depot.
 * @param flags type of operation
 * @param tile tile where ship depot is built
 * @param axis depot orientation (Axis)
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildShipDepot(DoCommandFlags flags, TileIndex tile, Axis axis)
{
	if (!IsValidAxis(axis)) return CMD_ERROR;
	TileIndex tile2 = tile + TileOffsByAxis(axis);

	if (!HasTileWaterGround(tile) || !HasTileWaterGround(tile2)) {
		return CommandCost(STR_ERROR_MUST_BE_BUILT_ON_WATER);
	}

	if (IsBridgeAbove(tile) || IsBridgeAbove(tile2)) return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);

	if (!IsTileFlat(tile) || !IsTileFlat(tile2)) {
		/* Prevent depots on rapids */
		return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}

	if (!Depot::CanAllocateItem()) return CMD_ERROR;

	WaterClass wc1 = GetWaterClass(tile);
	WaterClass wc2 = GetWaterClass(tile2);
	CommandCost cost = CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_DEPOT_SHIP]);

	bool add_cost = !IsWaterTile(tile);
	CommandCost ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags | DoCommandFlag::Auto, tile);
	if (ret.Failed()) return ret;
	if (add_cost) {
		cost.AddCost(ret.GetCost());
	}
	add_cost = !IsWaterTile(tile2);
	ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags | DoCommandFlag::Auto, tile2);
	if (ret.Failed()) return ret;
	if (add_cost) {
		cost.AddCost(ret.GetCost());
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		Depot *depot = new Depot(tile);

		uint new_water_infra = 2 * LOCK_DEPOT_TILE_FACTOR;
		/* Update infrastructure counts after the tile clears earlier.
		 * Clearing object tiles may result in water tiles which are already accounted for in the water infrastructure total.
		 * See: MakeWaterKeepingClass() */
		if (wc1 == WATER_CLASS_CANAL && !(HasTileWaterClass(tile) && GetWaterClass(tile) == WATER_CLASS_CANAL && IsTileOwner(tile, _current_company))) new_water_infra++;
		if (wc2 == WATER_CLASS_CANAL && !(HasTileWaterClass(tile2) && GetWaterClass(tile2) == WATER_CLASS_CANAL && IsTileOwner(tile2, _current_company))) new_water_infra++;

		Company::Get(_current_company)->infrastructure.water += new_water_infra;
		DirtyCompanyInfrastructureWindows(_current_company);

		MakeShipDepot(tile,  _current_company, depot->index, DEPOT_PART_NORTH, axis, wc1);
		MakeShipDepot(tile2, _current_company, depot->index, DEPOT_PART_SOUTH, axis, wc2);
		CheckForDockingTile(tile);
		CheckForDockingTile(tile2);
		MarkTileDirtyByTile(tile);
		MarkTileDirtyByTile(tile2);
		MakeDefaultName(depot);
	}

	return cost;
}

bool IsPossibleDockingTile(Tile t)
{
	assert(IsValidTile(t));
	switch (GetTileType(t)) {
		case MP_WATER:
			if (IsLock(t) && GetLockPart(t) == LOCK_PART_MIDDLE) return false;
			[[fallthrough]];
		case MP_RAILWAY:
		case MP_STATION:
		case MP_TUNNELBRIDGE:
			return TrackStatusToTrackBits(GetTileTrackStatus(t, TRANSPORT_WATER, 0)) != TRACK_BIT_NONE;

		default:
			return false;
	}
}

/**
 * Mark the supplied tile as a docking tile if it is suitable for docking.
 * Tiles surrounding the tile are tested to be docks with correct orientation.
 * @param t Tile to test.
 */
void CheckForDockingTile(TileIndex t)
{
	for (DiagDirection d = DIAGDIR_BEGIN; d != DIAGDIR_END; d++) {
		TileIndex tile = t + TileOffsByDiagDir(d);
		if (!IsValidTile(tile)) continue;

		if (IsDockTile(tile) && IsDockWaterPart(tile)) {
			Station::GetByTile(tile)->docking_station.Add(t);
			SetDockingTile(t, true);
		}
		if (IsTileType(tile, MP_INDUSTRY)) {
			Station *st = Industry::GetByTile(tile)->neutral_station;
			if (st != nullptr) {
				st->docking_station.Add(t);
				SetDockingTile(t, true);
			}
		}
		if (IsTileType(tile, MP_STATION) && IsOilRig(tile)) {
			Station::GetByTile(tile)->docking_station.Add(t);
			SetDockingTile(t, true);
		}
	}
}

void MakeWaterKeepingClass(TileIndex tile, Owner o)
{
	WaterClass wc = GetWaterClass(tile);

	/* Autoslope might turn an originally canal or river tile into land */
	auto [slope, z] = GetTileSlopeZ(tile);

	if (slope != SLOPE_FLAT) {
		if (wc == WATER_CLASS_CANAL) {
			/* If we clear the canal, we have to remove it from the infrastructure count as well. */
			Company *c = Company::GetIfValid(o);
			if (c != nullptr) {
				c->infrastructure.water--;
				DirtyCompanyInfrastructureWindows(c->index);
			}
			/* Sloped canals are locks and no natural water remains whatever the slope direction */
			wc = WATER_CLASS_INVALID;
		}

		/* Only river water should be restored on appropriate slopes. Other water would be invalid on slopes */
		if (wc != WATER_CLASS_RIVER || GetInclinedSlopeDirection(slope) == INVALID_DIAGDIR) {
			wc = WATER_CLASS_INVALID;
		}
	}

	if (wc == WATER_CLASS_SEA && z > 0) {
		/* Update company infrastructure count. */
		Company *c = Company::GetIfValid(o);
		if (c != nullptr) {
			c->infrastructure.water++;
			DirtyCompanyInfrastructureWindows(c->index);
		}

		wc = WATER_CLASS_CANAL;
	}

	/* Zero map array and terminate animation */
	DoClearSquare(tile);

	/* Maybe change to water */
	switch (wc) {
		case WATER_CLASS_SEA:   MakeSea(tile);                break;
		case WATER_CLASS_CANAL: MakeCanal(tile, o, Random()); break;
		case WATER_CLASS_RIVER: MakeRiver(tile, Random());    break;
		default: break;
	}

	if (wc != WATER_CLASS_INVALID) CheckForDockingTile(tile);
	MarkTileDirtyByTile(tile);
}

static CommandCost RemoveShipDepot(TileIndex tile, DoCommandFlags flags)
{
	if (!IsShipDepot(tile)) return CMD_ERROR;

	CommandCost ret = CheckTileOwnership(tile);
	if (ret.Failed()) return ret;

	TileIndex tile2 = GetOtherShipDepotTile(tile);

	/* do not check for ship on tile when company goes bankrupt */
	if (!flags.Test(DoCommandFlag::Bankrupt)) {
		ret = EnsureNoVehicleOnGround(tile);
		if (ret.Succeeded()) ret = EnsureNoVehicleOnGround(tile2);
		if (ret.Failed()) return ret;
	}

	bool do_clear = flags.Test(DoCommandFlag::ForceClearTile);

	if (flags.Test(DoCommandFlag::Execute)) {
		delete Depot::GetByTile(tile);

		Company *c = Company::GetIfValid(GetTileOwner(tile));
		if (c != nullptr) {
			c->infrastructure.water -= 2 * LOCK_DEPOT_TILE_FACTOR;
			if (do_clear && GetWaterClass(tile) == WATER_CLASS_CANAL) c->infrastructure.water--;
			DirtyCompanyInfrastructureWindows(c->index);
		}

		if (!do_clear) MakeWaterKeepingClass(tile,  GetTileOwner(tile));
		MakeWaterKeepingClass(tile2, GetTileOwner(tile2));
	}

	return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_CLEAR_DEPOT_SHIP]);
}

/**
 * Builds a lock.
 * @param tile Central tile of the lock.
 * @param dir Uphill direction.
 * @param flags Operation to perform.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost DoBuildLock(TileIndex tile, DiagDirection dir, DoCommandFlags flags)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);

	TileIndexDiff delta = TileOffsByDiagDir(dir);
	CommandCost ret = EnsureNoVehicleOnGround(tile);
	if (ret.Succeeded()) ret = EnsureNoVehicleOnGround(tile + delta);
	if (ret.Succeeded()) ret = EnsureNoVehicleOnGround(tile - delta);
	if (ret.Failed()) return ret;

	/* middle tile */
	WaterClass wc_middle = HasTileWaterGround(tile) ? GetWaterClass(tile) : WATER_CLASS_CANAL;
	ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile);
	if (ret.Failed()) return ret;
	cost.AddCost(ret.GetCost());

	/* lower tile */
	if (!IsWaterTile(tile - delta)) {
		ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile - delta);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());
		cost.AddCost(_price[PR_BUILD_CANAL]);
	}
	if (!IsTileFlat(tile - delta)) {
		return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
	}
	WaterClass wc_lower = IsWaterTile(tile - delta) ? GetWaterClass(tile - delta) : WATER_CLASS_CANAL;

	/* upper tile */
	if (!IsWaterTile(tile + delta)) {
		ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile + delta);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());
		cost.AddCost(_price[PR_BUILD_CANAL]);
	}
	if (!IsTileFlat(tile + delta)) {
		return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
	}
	WaterClass wc_upper = IsWaterTile(tile + delta) ? GetWaterClass(tile + delta) : WATER_CLASS_CANAL;

	if (IsBridgeAbove(tile) || IsBridgeAbove(tile - delta) || IsBridgeAbove(tile + delta)) {
		return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Update company infrastructure counts. */
		Company *c = Company::GetIfValid(_current_company);
		if (c != nullptr) {
			/* Counts for the water. */
			if (!IsWaterTile(tile - delta)) c->infrastructure.water++;
			if (!IsWaterTile(tile + delta)) c->infrastructure.water++;
			/* Count for the lock itself. */
			c->infrastructure.water += 3 * LOCK_DEPOT_TILE_FACTOR; // Lock is three tiles.
			DirtyCompanyInfrastructureWindows(_current_company);
		}

		MakeLock(tile, _current_company, dir, wc_lower, wc_upper, wc_middle);
		CheckForDockingTile(tile - delta);
		CheckForDockingTile(tile + delta);
		MarkTileDirtyByTile(tile);
		MarkTileDirtyByTile(tile - delta);
		MarkTileDirtyByTile(tile + delta);
		MarkCanalsAndRiversAroundDirty(tile - delta);
		MarkCanalsAndRiversAroundDirty(tile + delta);
		InvalidateWaterRegion(tile - delta);
		InvalidateWaterRegion(tile + delta);
	}
	cost.AddCost(_price[PR_BUILD_LOCK]);

	return cost;
}

/**
 * Remove a lock.
 * @param tile Central tile of the lock.
 * @param flags Operation to perform.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost RemoveLock(TileIndex tile, DoCommandFlags flags)
{
	if (GetTileOwner(tile) != OWNER_NONE) {
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) return ret;
	}

	TileIndexDiff delta = TileOffsByDiagDir(GetLockDirection(tile));

	/* make sure no vehicle is on the tile. */
	CommandCost ret = EnsureNoVehicleOnGround(tile);
	if (ret.Succeeded()) ret = EnsureNoVehicleOnGround(tile + delta);
	if (ret.Succeeded()) ret = EnsureNoVehicleOnGround(tile - delta);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Remove middle part from company infrastructure count. */
		Company *c = Company::GetIfValid(GetTileOwner(tile));
		if (c != nullptr) {
			c->infrastructure.water -= 3 * LOCK_DEPOT_TILE_FACTOR; // three parts of the lock.
			DirtyCompanyInfrastructureWindows(c->index);
		}

		if (GetWaterClass(tile) == WATER_CLASS_RIVER) {
			MakeRiver(tile, Random());
		} else {
			DoClearSquare(tile);
			ClearNeighbourNonFloodingStates(tile);
		}
		MakeWaterKeepingClass(tile + delta, GetTileOwner(tile + delta));
		MakeWaterKeepingClass(tile - delta, GetTileOwner(tile - delta));
		MarkCanalsAndRiversAroundDirty(tile);
		MarkCanalsAndRiversAroundDirty(tile - delta);
		MarkCanalsAndRiversAroundDirty(tile + delta);
	}

	return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_CLEAR_LOCK]);
}

/**
 * Builds a lock.
 * @param flags type of operation
 * @param tile tile where to place the lock
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildLock(DoCommandFlags flags, TileIndex tile)
{
	DiagDirection dir = GetInclinedSlopeDirection(GetTileSlope(tile));
	if (dir == INVALID_DIAGDIR) return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);

	return DoBuildLock(tile, dir, flags);
}

/**
 * Make a river tile and remove desert directly around it.
 * @param tile The tile to change into river and create non-desert around
 */
void MakeRiverAndModifyDesertZoneAround(TileIndex tile)
{
	MakeRiver(tile, Random());
	MarkTileDirtyByTile(tile);

	/* Remove desert directly around the river tile. */
	for (auto t : SpiralTileSequence(tile, RIVER_OFFSET_DESERT_DISTANCE)) {
		if (GetTropicZone(t) == TROPICZONE_DESERT) SetTropicZone(t, TROPICZONE_NORMAL);
	}
}

/**
 * Build a piece of canal.
 * @param flags type of operation
 * @param tile end tile of stretch-dragging
 * @param start_tile start tile of stretch-dragging
 * @param wc waterclass to build. sea and river can only be built in scenario editor
 * @param diagonal Whether to use the Orthogonal (0) or Diagonal (1) iterator.
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildCanal(DoCommandFlags flags, TileIndex tile, TileIndex start_tile, WaterClass wc, bool diagonal)
{
	if (start_tile >= Map::Size() || !IsValidWaterClass(wc)) return CMD_ERROR;

	/* Outside of the editor you can only build canals, not oceans */
	if (wc != WATER_CLASS_CANAL && _game_mode != GM_EDITOR) return CMD_ERROR;

	CommandCost cost(EXPENSES_CONSTRUCTION);

	std::unique_ptr<TileIterator> iter = TileIterator::Create(tile, start_tile, diagonal);
	for (; *iter != INVALID_TILE; ++(*iter)) {
		TileIndex current_tile = *iter;
		CommandCost ret;

		Slope slope = GetTileSlope(current_tile);
		if (slope != SLOPE_FLAT && (wc != WATER_CLASS_RIVER || !IsInclinedSlope(slope))) {
			return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		}

		bool water = IsWaterTile(current_tile);

		/* Outside the editor, prevent building canals over your own or OWNER_NONE owned canals */
		if (water && IsCanal(current_tile) && _game_mode != GM_EDITOR && (IsTileOwner(current_tile, _current_company) || IsTileOwner(current_tile, OWNER_NONE))) continue;

		ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, current_tile);
		if (ret.Failed()) return ret;

		if (!water) cost.AddCost(ret.GetCost());

		if (flags.Test(DoCommandFlag::Execute)) {
			if (IsTileType(current_tile, MP_WATER) && IsCanal(current_tile)) {
				Owner owner = GetTileOwner(current_tile);
				if (Company::IsValidID(owner)) {
					Company::Get(owner)->infrastructure.water--;
					DirtyCompanyInfrastructureWindows(owner);
				}
			}

			switch (wc) {
				case WATER_CLASS_RIVER:
					MakeRiver(current_tile, Random());
					if (_game_mode == GM_EDITOR) {
						/* Remove desert directly around the river tile. */
						for (auto t : SpiralTileSequence(current_tile, RIVER_OFFSET_DESERT_DISTANCE)) {
							if (GetTropicZone(t) == TROPICZONE_DESERT) SetTropicZone(t, TROPICZONE_NORMAL);
						}
					}
					break;

				case WATER_CLASS_SEA:
					if (TileHeight(current_tile) == 0) {
						MakeSea(current_tile);
						break;
					}
					[[fallthrough]];

				default:
					MakeCanal(current_tile, _current_company, Random());
					if (Company::IsValidID(_current_company)) {
						Company::Get(_current_company)->infrastructure.water++;
						DirtyCompanyInfrastructureWindows(_current_company);
					}
					break;
			}
			MarkTileDirtyByTile(current_tile);
			MarkCanalsAndRiversAroundDirty(current_tile);
			CheckForDockingTile(current_tile);
		}

		cost.AddCost(_price[PR_BUILD_CANAL]);
	}

	if (cost.GetCost() == 0) {
		return CommandCost(STR_ERROR_ALREADY_BUILT);
	} else {
		return cost;
	}
}


static CommandCost ClearTile_Water(TileIndex tile, DoCommandFlags flags)
{
	switch (GetWaterTileType(tile)) {
		case WATER_TILE_CLEAR: {
			if (flags.Test(DoCommandFlag::NoWater)) return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);

			Money base_cost = IsCanal(tile) ? _price[PR_CLEAR_CANAL] : _price[PR_CLEAR_WATER];
			/* Make sure freeform edges are allowed or it's not an edge tile. */
			if (!_settings_game.construction.freeform_edges && (!IsInsideMM(TileX(tile), 1, Map::MaxX() - 1) ||
					!IsInsideMM(TileY(tile), 1, Map::MaxY() - 1))) {
				return CommandCost(STR_ERROR_TOO_CLOSE_TO_EDGE_OF_MAP);
			}

			/* Make sure no vehicle is on the tile */
			CommandCost ret = EnsureNoVehicleOnGround(tile);
			if (ret.Failed()) return ret;

			Owner owner = GetTileOwner(tile);
			if (owner != OWNER_WATER && owner != OWNER_NONE) {
				ret = CheckTileOwnership(tile);
				if (ret.Failed()) return ret;
			}

			if (flags.Test(DoCommandFlag::Execute)) {
				if (IsCanal(tile) && Company::IsValidID(owner)) {
					Company::Get(owner)->infrastructure.water--;
					DirtyCompanyInfrastructureWindows(owner);
				}
				DoClearSquare(tile);
				MarkCanalsAndRiversAroundDirty(tile);
				ClearNeighbourNonFloodingStates(tile);
			}

			return CommandCost(EXPENSES_CONSTRUCTION, base_cost);
		}

		case WATER_TILE_COAST: {
			Slope slope = GetTileSlope(tile);

			/* Make sure no vehicle is on the tile */
			CommandCost ret = EnsureNoVehicleOnGround(tile);
			if (ret.Failed()) return ret;

			if (flags.Test(DoCommandFlag::Execute)) {
				DoClearSquare(tile);
				MarkCanalsAndRiversAroundDirty(tile);
				ClearNeighbourNonFloodingStates(tile);
			}
			if (IsSlopeWithOneCornerRaised(slope)) {
				return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_CLEAR_WATER]);
			} else {
				return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_CLEAR_ROUGH]);
			}
		}

		case WATER_TILE_LOCK: {
			static const TileIndexDiffC _lock_tomiddle_offs[][DIAGDIR_END] = {
				/*   NE       SE        SW      NW       */
				{ { 0,  0}, {0,  0}, { 0, 0}, {0,  0} }, // LOCK_PART_MIDDLE
				{ {-1,  0}, {0,  1}, { 1, 0}, {0, -1} }, // LOCK_PART_LOWER
				{ { 1,  0}, {0, -1}, {-1, 0}, {0,  1} }, // LOCK_PART_UPPER
			};

			if (flags.Test(DoCommandFlag::Auto)) return CommandCost(STR_ERROR_BUILDING_MUST_BE_DEMOLISHED);
			if (_current_company == OWNER_WATER) return CMD_ERROR;
			/* move to the middle tile.. */
			return RemoveLock(tile + ToTileIndexDiff(_lock_tomiddle_offs[GetLockPart(tile)][GetLockDirection(tile)]), flags);
		}

		case WATER_TILE_DEPOT:
			if (flags.Test(DoCommandFlag::Auto)) return CommandCost(STR_ERROR_BUILDING_MUST_BE_DEMOLISHED);
			return RemoveShipDepot(tile, flags);

		default:
			NOT_REACHED();
	}
}

/**
 * return true if a tile is a water tile wrt. a certain direction.
 *
 * @param tile The tile of interest.
 * @param from The direction of interest.
 * @return true iff the tile is water in the view of 'from'.
 *
 */
bool IsWateredTile(TileIndex tile, Direction from)
{
	switch (GetTileType(tile)) {
		case MP_WATER:
			switch (GetWaterTileType(tile)) {
				default: NOT_REACHED();
				case WATER_TILE_DEPOT: case WATER_TILE_CLEAR: return true;
				case WATER_TILE_LOCK: return DiagDirToAxis(GetLockDirection(tile)) == DiagDirToAxis(DirToDiagDir(from));

				case WATER_TILE_COAST:
					switch (GetTileSlope(tile)) {
						case SLOPE_W: return (from == DIR_SE) || (from == DIR_E) || (from == DIR_NE);
						case SLOPE_S: return (from == DIR_NE) || (from == DIR_N) || (from == DIR_NW);
						case SLOPE_E: return (from == DIR_NW) || (from == DIR_W) || (from == DIR_SW);
						case SLOPE_N: return (from == DIR_SW) || (from == DIR_S) || (from == DIR_SE);
						default: return false;
					}
			}

		case MP_RAILWAY:
			if (GetRailGroundType(tile) == RAIL_GROUND_WATER) {
				assert(IsPlainRail(tile));
				switch (GetTileSlope(tile)) {
					case SLOPE_W: return (from == DIR_SE) || (from == DIR_E) || (from == DIR_NE);
					case SLOPE_S: return (from == DIR_NE) || (from == DIR_N) || (from == DIR_NW);
					case SLOPE_E: return (from == DIR_NW) || (from == DIR_W) || (from == DIR_SW);
					case SLOPE_N: return (from == DIR_SW) || (from == DIR_S) || (from == DIR_SE);
					default: return false;
				}
			}
			return false;

		case MP_STATION:
			if (IsOilRig(tile)) {
				/* Do not draw waterborders inside of industries.
				 * Note: There is no easy way to detect the industry of an oilrig tile. */
				TileIndex src_tile = tile + TileOffsByDir(from);
				if ((IsTileType(src_tile, MP_STATION) && IsOilRig(src_tile)) ||
				    (IsTileType(src_tile, MP_INDUSTRY))) return true;

				return IsTileOnWater(tile);
			}
			return (IsDock(tile) && IsTileFlat(tile)) || IsBuoy(tile);

		case MP_INDUSTRY: {
			/* Do not draw waterborders inside of industries.
			 * Note: There is no easy way to detect the industry of an oilrig tile. */
			TileIndex src_tile = tile + TileOffsByDir(from);
			if ((IsTileType(src_tile, MP_STATION) && IsOilRig(src_tile)) ||
			    (IsTileType(src_tile, MP_INDUSTRY) && GetIndustryIndex(src_tile) == GetIndustryIndex(tile))) return true;

			return IsTileOnWater(tile);
		}

		case MP_OBJECT: return IsTileOnWater(tile);

		case MP_TUNNELBRIDGE: return GetTunnelBridgeTransportType(tile) == TRANSPORT_WATER && ReverseDiagDir(GetTunnelBridgeDirection(tile)) == DirToDiagDir(from);

		case MP_VOID: return true; // consider map border as water, esp. for rivers

		default:          return false;
	}
}

/**
 * Draw a water sprite, potentially with a NewGRF-modified sprite offset.
 * @param base    Sprite base.
 * @param offset  Sprite offset.
 * @param feature The type of sprite that is drawn.
 * @param tile    Tile index to draw.
 */
static void DrawWaterSprite(SpriteID base, uint offset, CanalFeature feature, TileIndex tile)
{
	if (base != SPR_FLAT_WATER_TILE) {
		/* Only call offset callback if the sprite is NewGRF-provided. */
		offset = GetCanalSpriteOffset(feature, tile, offset);
	}
	DrawGroundSprite(base + offset, PAL_NONE);
}

/**
 * Draw canal or river edges.
 * @param canal  True if canal edges should be drawn, false for river edges.
 * @param offset Sprite offset.
 * @param tile   Tile to draw.
 */
static void DrawWaterEdges(bool canal, uint offset, TileIndex tile)
{
	CanalFeature feature;
	SpriteID base = 0;
	if (canal) {
		feature = CF_DIKES;
		base = GetCanalSprite(CF_DIKES, tile);
		if (base == 0) base = SPR_CANAL_DIKES_BASE;
	} else {
		feature = CF_RIVER_EDGE;
		base = GetCanalSprite(CF_RIVER_EDGE, tile);
		if (base == 0) return; // Don't draw if no sprites provided.
	}

	uint wa;

	/* determine the edges around with water. */
	wa  = IsWateredTile(TileAddXY(tile, -1,  0), DIR_SW) << 0;
	wa += IsWateredTile(TileAddXY(tile,  0,  1), DIR_NW) << 1;
	wa += IsWateredTile(TileAddXY(tile,  1,  0), DIR_NE) << 2;
	wa += IsWateredTile(TileAddXY(tile,  0, -1), DIR_SE) << 3;

	if (!(wa & 1)) DrawWaterSprite(base, offset,     feature, tile);
	if (!(wa & 2)) DrawWaterSprite(base, offset + 1, feature, tile);
	if (!(wa & 4)) DrawWaterSprite(base, offset + 2, feature, tile);
	if (!(wa & 8)) DrawWaterSprite(base, offset + 3, feature, tile);

	/* right corner */
	switch (wa & 0x03) {
		case 0: DrawWaterSprite(base, offset + 4, feature, tile); break;
		case 3: if (!IsWateredTile(TileAddXY(tile, -1, 1), DIR_W)) DrawWaterSprite(base, offset + 8, feature, tile); break;
	}

	/* bottom corner */
	switch (wa & 0x06) {
		case 0: DrawWaterSprite(base, offset + 5, feature, tile); break;
		case 6: if (!IsWateredTile(TileAddXY(tile, 1, 1), DIR_N)) DrawWaterSprite(base, offset + 9, feature, tile); break;
	}

	/* left corner */
	switch (wa & 0x0C) {
		case  0: DrawWaterSprite(base, offset + 6, feature, tile); break;
		case 12: if (!IsWateredTile(TileAddXY(tile, 1, -1), DIR_E)) DrawWaterSprite(base, offset + 10, feature, tile); break;
	}

	/* upper corner */
	switch (wa & 0x09) {
		case 0: DrawWaterSprite(base, offset + 7, feature, tile); break;
		case 9: if (!IsWateredTile(TileAddXY(tile, -1, -1), DIR_S)) DrawWaterSprite(base, offset + 11, feature, tile); break;
	}
}

/** Draw a plain sea water tile with no edges */
static void DrawSeaWater(TileIndex)
{
	DrawGroundSprite(SPR_FLAT_WATER_TILE, PAL_NONE);
}

/** draw a canal styled water tile with dikes around */
static void DrawCanalWater(TileIndex tile)
{
	SpriteID image = SPR_FLAT_WATER_TILE;
	if (HasBit(_water_feature[CF_WATERSLOPE].flags, CFF_HAS_FLAT_SPRITE)) {
		/* First water slope sprite is flat water. */
		image = GetCanalSprite(CF_WATERSLOPE, tile);
		if (image == 0) image = SPR_FLAT_WATER_TILE;
	}
	DrawWaterSprite(image, 0, CF_WATERSLOPE, tile);

	DrawWaterEdges(true, 0, tile);
}

#include "table/water_land.h"

/**
 * Draw a build sprite sequence for water tiles.
 * If buildings are invisible, nothing will be drawn.
 * @param ti      Tile info.
 * @param seq Sprite sequence to draw.
 * @param base    Base sprite.
 * @param offset  Additional sprite offset.
 * @param palette Palette to use.
 */
static void DrawWaterTileStruct(const TileInfo *ti, std::span<const DrawTileSeqStruct> seq, SpriteID base, uint offset, PaletteID palette, CanalFeature feature)
{
	/* Don't draw if buildings are invisible. */
	if (IsInvisibilitySet(TO_BUILDINGS)) return;

	for (const DrawTileSeqStruct &dtss : seq) {
		uint tile_offs = offset + dtss.image.sprite;
		if (feature < CF_END) tile_offs = GetCanalSpriteOffset(feature, ti->tile, tile_offs);
		AddSortableSpriteToDraw(base + tile_offs, palette, *ti, dtss, IsTransparencySet(TO_BUILDINGS));
	}
}

/** Draw a lock tile. */
static void DrawWaterLock(const TileInfo *ti)
{
	int part = GetLockPart(ti->tile);
	const DrawTileSprites &dts = _lock_display_data[part][GetLockDirection(ti->tile)];

	/* Draw ground sprite. */
	SpriteID image = dts.ground.sprite;

	SpriteID water_base = GetCanalSprite(CF_WATERSLOPE, ti->tile);
	if (water_base == 0) {
		/* Use default sprites. */
		water_base = SPR_CANALS_BASE;
	} else if (HasBit(_water_feature[CF_WATERSLOPE].flags, CFF_HAS_FLAT_SPRITE)) {
		/* NewGRF supplies a flat sprite as first sprite. */
		if (image == SPR_FLAT_WATER_TILE) {
			image = water_base;
		} else {
			image++;
		}
	}

	if (image < 5) image += water_base;
	DrawGroundSprite(image, PAL_NONE);

	/* Draw structures. */
	uint     zoffs = 0;
	SpriteID base  = GetCanalSprite(CF_LOCKS, ti->tile);

	if (base == 0) {
		/* If no custom graphics, use defaults. */
		base = SPR_LOCK_BASE;
		uint8_t z_threshold = part == LOCK_PART_UPPER ? 8 : 0;
		zoffs = ti->z > z_threshold ? 24 : 0;
	}

	DrawWaterTileStruct(ti, dts.GetSequence(), base, zoffs, PAL_NONE, CF_LOCKS);
}

/** Draw a ship depot tile. */
static void DrawWaterDepot(const TileInfo *ti)
{
	DrawWaterClassGround(ti);
	DrawWaterTileStruct(ti, _shipdepot_display_data[GetShipDepotAxis(ti->tile)][GetShipDepotPart(ti->tile)].seq, 0, 0, GetCompanyPalette(GetTileOwner(ti->tile)), CF_END);
}

static void DrawRiverWater(const TileInfo *ti)
{
	SpriteID image = SPR_FLAT_WATER_TILE;
	uint     offset = 0;
	uint     edges_offset = 0;

	if (ti->tileh != SLOPE_FLAT || HasBit(_water_feature[CF_RIVER_SLOPE].flags, CFF_HAS_FLAT_SPRITE)) {
		image = GetCanalSprite(CF_RIVER_SLOPE, ti->tile);
		if (image == 0) {
			switch (ti->tileh) {
				case SLOPE_NW: image = SPR_WATER_SLOPE_Y_DOWN; break;
				case SLOPE_SW: image = SPR_WATER_SLOPE_X_UP;   break;
				case SLOPE_SE: image = SPR_WATER_SLOPE_Y_UP;   break;
				case SLOPE_NE: image = SPR_WATER_SLOPE_X_DOWN; break;
				default:       image = SPR_FLAT_WATER_TILE;    break;
			}
		} else {
			/* Flag bit 0 indicates that the first sprite is flat water. */
			offset = HasBit(_water_feature[CF_RIVER_SLOPE].flags, CFF_HAS_FLAT_SPRITE) ? 1 : 0;

			switch (ti->tileh) {
				case SLOPE_SE:              edges_offset += 12; break;
				case SLOPE_NE: offset += 1; edges_offset += 24; break;
				case SLOPE_SW: offset += 2; edges_offset += 36; break;
				case SLOPE_NW: offset += 3; edges_offset += 48; break;
				default:       offset  = 0; break;
			}

			offset = GetCanalSpriteOffset(CF_RIVER_SLOPE, ti->tile, offset);
		}
	}

	DrawGroundSprite(image + offset, PAL_NONE);

	/* Draw river edges if available. */
	DrawWaterEdges(false, edges_offset, ti->tile);
}

void DrawShoreTile(Slope tileh)
{
	/* Converts the enum Slope into an offset based on SPR_SHORE_BASE.
	 * This allows to calculate the proper sprite to display for this Slope */
	static const uint8_t tileh_to_shoresprite[32] = {
		0, 1, 2, 3, 4, 16, 6, 7, 8, 9, 17, 11, 12, 13, 14, 0,
		0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  5,  0, 10, 15, 0,
	};

	assert(!IsHalftileSlope(tileh)); // Halftile slopes need to get handled earlier.
	assert(tileh != SLOPE_FLAT);     // Shore is never flat

	assert((tileh != SLOPE_EW) && (tileh != SLOPE_NS)); // No suitable sprites for current flooding behaviour

	DrawGroundSprite(SPR_SHORE_BASE + tileh_to_shoresprite[tileh], PAL_NONE);
}

void DrawWaterClassGround(const TileInfo *ti)
{
	switch (GetWaterClass(ti->tile)) {
		case WATER_CLASS_SEA:   DrawSeaWater(ti->tile); break;
		case WATER_CLASS_CANAL: DrawCanalWater(ti->tile); break;
		case WATER_CLASS_RIVER: DrawRiverWater(ti); break;
		default: NOT_REACHED();
	}
}

static void DrawTile_Water(TileInfo *ti)
{
	switch (GetWaterTileType(ti->tile)) {
		case WATER_TILE_CLEAR:
			DrawWaterClassGround(ti);
			DrawBridgeMiddle(ti);
			break;

		case WATER_TILE_COAST: {
			DrawShoreTile(ti->tileh);
			DrawBridgeMiddle(ti);
			break;
		}

		case WATER_TILE_LOCK:
			DrawWaterLock(ti);
			break;

		case WATER_TILE_DEPOT:
			DrawWaterDepot(ti);
			break;
	}
}

void DrawShipDepotSprite(int x, int y, Axis axis, DepotPart part)
{
	const DrawTileSprites &dts = _shipdepot_display_data[axis][part];

	DrawSprite(dts.ground.sprite, dts.ground.pal, x, y);
	DrawOrigTileSeqInGUI(x, y, &dts, GetCompanyPalette(_local_company));
}


static int GetSlopePixelZ_Water(TileIndex tile, uint x, uint y, bool)
{
	auto [tileh, z] = GetTilePixelSlope(tile);

	return z + GetPartialPixelZ(x & 0xF, y & 0xF, tileh);
}

static Foundation GetFoundation_Water(TileIndex, Slope)
{
	return FOUNDATION_NONE;
}

static void GetTileDesc_Water(TileIndex tile, TileDesc &td)
{
	switch (GetWaterTileType(tile)) {
		case WATER_TILE_CLEAR:
			switch (GetWaterClass(tile)) {
				case WATER_CLASS_SEA:   td.str = STR_LAI_WATER_DESCRIPTION_WATER; break;
				case WATER_CLASS_CANAL: td.str = STR_LAI_WATER_DESCRIPTION_CANAL; break;
				case WATER_CLASS_RIVER: td.str = STR_LAI_WATER_DESCRIPTION_RIVER; break;
				default: NOT_REACHED();
			}
			break;
		case WATER_TILE_COAST: td.str = STR_LAI_WATER_DESCRIPTION_COAST_OR_RIVERBANK; break;
		case WATER_TILE_LOCK : td.str = STR_LAI_WATER_DESCRIPTION_LOCK;               break;
		case WATER_TILE_DEPOT:
			td.str = STR_LAI_WATER_DESCRIPTION_SHIP_DEPOT;
			td.build_date = Depot::GetByTile(tile)->build_date;
			break;
		default: NOT_REACHED();
	}

	td.owner[0] = GetTileOwner(tile);
}

/**
 * Handle the flooding of a vehicle. This sets the vehicle state to crashed,
 * creates a newsitem and dirties the necessary windows.
 * @param v The vehicle to flood.
 */
static void FloodVehicle(Vehicle *v)
{
	uint victims = v->Crash(true);

	AI::NewEvent(v->owner, new ScriptEventVehicleCrashed(v->index, v->tile, ScriptEventVehicleCrashed::CRASH_FLOODED, victims, v->owner));
	Game::NewEvent(new ScriptEventVehicleCrashed(v->index, v->tile, ScriptEventVehicleCrashed::CRASH_FLOODED, victims, v->owner));
	AddTileNewsItem(GetEncodedString(STR_NEWS_DISASTER_FLOOD_VEHICLE, victims), NewsType::Accident, v->tile);
	CreateEffectVehicleRel(v, 4, 4, 8, EV_EXPLOSION_LARGE);
	if (_settings_client.sound.disaster) SndPlayVehicleFx(SND_12_EXPLOSION, v);
}

/**
 * Flood a vehicle if we are allowed to flood it, i.e. when it is on the ground.
 * @param v    The vehicle to test for flooding.
 * @param z    The z of level to flood.
 */
static void FloodVehicleProc(Vehicle *v, int z)
{
	if (v->vehstatus.Test(VehState::Crashed)) return;

	switch (v->type) {
		default: break;

		case VEH_AIRCRAFT: {
			if (!IsAirportTile(v->tile) || GetTileMaxZ(v->tile) != 0) break;
			if (v->subtype == AIR_SHADOW) break;

			/* We compare v->z_pos against delta_z + 1 because the shadow
			 * is at delta_z and the actual aircraft at delta_z + 1. */
			const Station *st = Station::GetByTile(v->tile);
			const AirportFTAClass *airport = st->airport.GetFTA();
			if (v->z_pos != airport->delta_z + 1) break;

			FloodVehicle(v);
			break;
		}

		case VEH_TRAIN:
		case VEH_ROAD: {
			if (v->z_pos > z) break;
			FloodVehicle(v->First());
			break;
		}
	}
}

static void FloodVehiclesOnTile(TileIndex tile, int z)
{
	for (Vehicle *v : VehiclesOnTile(tile)) {
		FloodVehicleProc(v, z);
	}
}

/**
 * Finds a vehicle to flood.
 * It does not find vehicles that are already crashed on bridges, i.e. flooded.
 * @param tile the tile where to find a vehicle to flood
 */
static void FloodVehicles(TileIndex tile)
{
	if (IsAirportTile(tile)) {
		const Station *st = Station::GetByTile(tile);
		for (TileIndex airport_tile : st->airport) {
			if (st->TileBelongsToAirport(airport_tile)) FloodVehiclesOnTile(airport_tile, 0);
		}

		/* No vehicle could be flooded on this airport anymore */
		return;
	}

	if (!IsBridgeTile(tile)) {
		FloodVehiclesOnTile(tile, 0);
		return;
	}

	TileIndex end = GetOtherBridgeEnd(tile);
	int z = GetBridgePixelHeight(tile);

	FloodVehiclesOnTile(tile, z);
	FloodVehiclesOnTile(end, z);
}

/**
 * Returns the behaviour of a tile during flooding.
 *
 * @return Behaviour of the tile
 */
FloodingBehaviour GetFloodingBehaviour(TileIndex tile)
{
	/* FLOOD_ACTIVE:  'single-corner-raised'-coast, sea, sea-shipdepots, sea-buoys, sea-docks (water part), rail with flooded halftile, sea-water-industries, sea-oilrigs
	 * FLOOD_DRYUP:   coast with more than one corner raised, coast with rail-track, coast with trees
	 * FLOOD_PASSIVE: (not used)
	 * FLOOD_NONE:    canals, rivers, everything else
	 */
	switch (GetTileType(tile)) {
		case MP_WATER:
			if (IsCoast(tile)) {
				Slope tileh = GetTileSlope(tile);
				return (IsSlopeWithOneCornerRaised(tileh) ? FLOOD_ACTIVE : FLOOD_DRYUP);
			}
			[[fallthrough]];
		case MP_STATION:
		case MP_INDUSTRY:
		case MP_OBJECT:
			return (GetWaterClass(tile) == WATER_CLASS_SEA) ? FLOOD_ACTIVE : FLOOD_NONE;

		case MP_RAILWAY:
			if (GetRailGroundType(tile) == RAIL_GROUND_WATER) {
				return (IsSlopeWithOneCornerRaised(GetTileSlope(tile)) ? FLOOD_ACTIVE : FLOOD_DRYUP);
			}
			return FLOOD_NONE;

		case MP_TREES:
			return (GetTreeGround(tile) == TREE_GROUND_SHORE ? FLOOD_DRYUP : FLOOD_NONE);

		case MP_VOID:
			return FLOOD_ACTIVE;

		default:
			return FLOOD_NONE;
	}
}

/**
 * Floods a tile.
 */
static void DoFloodTile(TileIndex target)
{
	assert(!IsTileType(target, MP_WATER));

	bool flooded = false; // Will be set to true if something is changed.

	Backup<CompanyID> cur_company(_current_company, OWNER_WATER);

	Slope tileh = GetTileSlope(target);
	if (tileh != SLOPE_FLAT) {
		/* make coast.. */
		switch (GetTileType(target)) {
			case MP_RAILWAY: {
				if (!IsPlainRail(target)) break;
				FloodVehicles(target);
				flooded = FloodHalftile(target);
				break;
			}

			case MP_TREES:
				if (!IsSlopeWithOneCornerRaised(tileh)) {
					SetTreeGroundDensity(target, TREE_GROUND_SHORE, 3);
					MarkTileDirtyByTile(target);
					flooded = true;
					break;
				}
				[[fallthrough]];

			case MP_CLEAR:
				if (Command<CMD_LANDSCAPE_CLEAR>::Do(DoCommandFlag::Execute, target).Succeeded()) {
					MakeShore(target);
					MarkTileDirtyByTile(target);
					flooded = true;
				}
				break;

			default:
				break;
		}
	} else {
		/* Flood vehicles */
		FloodVehicles(target);

		/* flood flat tile */
		if (Command<CMD_LANDSCAPE_CLEAR>::Do(DoCommandFlag::Execute, target).Succeeded()) {
			MakeSea(target);
			MarkTileDirtyByTile(target);
			flooded = true;
		}
	}

	if (flooded) {
		/* Mark surrounding canal tiles dirty too to avoid glitches */
		MarkCanalsAndRiversAroundDirty(target);

		/* update signals if needed */
		UpdateSignalsInBuffer();

		if (IsPossibleDockingTile(target)) CheckForDockingTile(target);
		InvalidateWaterRegion(target);
	}

	cur_company.Restore();
}

/**
 * Drys a tile up.
 */
static void DoDryUp(TileIndex tile)
{
	Backup<CompanyID> cur_company(_current_company, OWNER_WATER);

	switch (GetTileType(tile)) {
		case MP_RAILWAY:
			assert(IsPlainRail(tile));
			assert(GetRailGroundType(tile) == RAIL_GROUND_WATER);

			RailGroundType new_ground;
			switch (GetTrackBits(tile)) {
				case TRACK_BIT_UPPER: new_ground = RAIL_GROUND_FENCE_HORIZ1; break;
				case TRACK_BIT_LOWER: new_ground = RAIL_GROUND_FENCE_HORIZ2; break;
				case TRACK_BIT_LEFT:  new_ground = RAIL_GROUND_FENCE_VERT1;  break;
				case TRACK_BIT_RIGHT: new_ground = RAIL_GROUND_FENCE_VERT2;  break;
				default: NOT_REACHED();
			}
			SetRailGroundType(tile, new_ground);
			MarkTileDirtyByTile(tile);
			break;

		case MP_TREES:
			SetTreeGroundDensity(tile, TREE_GROUND_GRASS, 3);
			MarkTileDirtyByTile(tile);
			break;

		case MP_WATER:
			assert(IsCoast(tile));

			if (Command<CMD_LANDSCAPE_CLEAR>::Do(DoCommandFlag::Execute, tile).Succeeded()) {
				MakeClear(tile, CLEAR_GRASS, 3);
				MarkTileDirtyByTile(tile);
			}
			break;

		default: NOT_REACHED();
	}

	cur_company.Restore();
}

/**
 * Let a water tile floods its diagonal adjoining tiles
 * called from tunnelbridge_cmd, and by TileLoop_Industry() and TileLoop_Track()
 *
 * @param tile the water/shore tile that floods
 */
void TileLoop_Water(TileIndex tile)
{
	if (IsTileType(tile, MP_WATER)) {
		AmbientSoundEffect(tile);
		if (IsNonFloodingWaterTile(tile)) return;
	}

	switch (GetFloodingBehaviour(tile)) {
		case FLOOD_ACTIVE: {
			bool continue_flooding = false;
			for (Direction dir = DIR_BEGIN; dir < DIR_END; dir++) {
				TileIndex dest = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDir(dir));
				/* Contrary to drying up, flooding does not consider MP_VOID tiles. */
				if (!IsValidTile(dest)) continue;
				/* do not try to flood water tiles - increases performance a lot */
				if (IsTileType(dest, MP_WATER)) continue;

				/* Buoys and docks cannot be flooded, and when removed turn into flooding water. */
				if (IsTileType(dest, MP_STATION) && (IsBuoy(dest) || IsDock(dest))) continue;

				/* This neighbour tile might be floodable later if the tile is cleared, so allow flooding to continue. */
				continue_flooding = true;

				/* TREE_GROUND_SHORE is the sign of a previous flood. */
				if (IsTileType(dest, MP_TREES) && GetTreeGround(dest) == TREE_GROUND_SHORE) continue;

				auto [slope_dest, z_dest] = GetFoundationSlope(dest);
				if (z_dest > 0) continue;

				if (!_flood_from_dirs[slope_dest & ~SLOPE_HALFTILE_MASK & ~SLOPE_STEEP].Test(ReverseDir(dir))) continue;

				DoFloodTile(dest);
			}
			if (!continue_flooding && IsTileType(tile, MP_WATER)) SetNonFloodingWaterTile(tile, true);
			break;
		}

		case FLOOD_DRYUP: {
			Slope slope_here = std::get<0>(GetFoundationSlope(tile)) & ~SLOPE_HALFTILE_MASK & ~SLOPE_STEEP;
			for (Direction dir : _flood_from_dirs[slope_here]) {
				TileIndex dest = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDir(dir));
				/* Contrary to flooding, drying up does consider MP_VOID tiles. */
				if (dest == INVALID_TILE) continue;

				FloodingBehaviour dest_behaviour = GetFloodingBehaviour(dest);
				if ((dest_behaviour == FLOOD_ACTIVE) || (dest_behaviour == FLOOD_PASSIVE)) return;
			}
			DoDryUp(tile);
			break;
		}

		default: return;
	}
}

void ConvertGroundTilesIntoWaterTiles()
{
	for (const auto tile : Map::Iterate()) {
		auto [slope, z] = GetTileSlopeZ(tile);
		if (IsTileType(tile, MP_CLEAR) && z == 0) {
			/* Make both water for tiles at level 0
			 * and make shore, as that looks much better
			 * during the generation. */
			switch (slope) {
				case SLOPE_FLAT:
					MakeSea(tile);
					break;

				case SLOPE_N:
				case SLOPE_E:
				case SLOPE_S:
				case SLOPE_W:
					MakeShore(tile);
					break;

				default:
					for (Direction dir : _flood_from_dirs[slope & ~SLOPE_STEEP]) {
						TileIndex dest = TileAddByDir(tile, dir);
						Slope slope_dest = GetTileSlope(dest) & ~SLOPE_STEEP;
						if (slope_dest == SLOPE_FLAT || IsSlopeWithOneCornerRaised(slope_dest) || IsTileType(dest, MP_VOID)) {
							MakeShore(tile);
							break;
						}
					}
					break;
			}
		}
	}
}

static TrackStatus GetTileTrackStatus_Water(TileIndex tile, TransportType mode, uint, DiagDirection)
{
	static const TrackBits coast_tracks[] = {TRACK_BIT_NONE, TRACK_BIT_RIGHT, TRACK_BIT_UPPER, TRACK_BIT_NONE, TRACK_BIT_LEFT, TRACK_BIT_NONE, TRACK_BIT_NONE,
		TRACK_BIT_NONE, TRACK_BIT_LOWER, TRACK_BIT_NONE, TRACK_BIT_NONE, TRACK_BIT_NONE, TRACK_BIT_NONE, TRACK_BIT_NONE, TRACK_BIT_NONE, TRACK_BIT_NONE};

	TrackBits ts;

	if (mode != TRANSPORT_WATER) return 0;

	switch (GetWaterTileType(tile)) {
		case WATER_TILE_CLEAR: ts = IsTileFlat(tile) ? TRACK_BIT_ALL : TRACK_BIT_NONE; break;
		case WATER_TILE_COAST: ts = coast_tracks[GetTileSlope(tile) & 0xF]; break;
		case WATER_TILE_LOCK:  ts = DiagDirToDiagTrackBits(GetLockDirection(tile)); break;
		case WATER_TILE_DEPOT: ts = AxisToTrackBits(GetShipDepotAxis(tile)); break;
		default: return 0;
	}
	if (TileX(tile) == 0) {
		/* NE border: remove tracks that connects NE tile edge */
		ts &= ~(TRACK_BIT_X | TRACK_BIT_UPPER | TRACK_BIT_RIGHT);
	}
	if (TileY(tile) == 0) {
		/* NW border: remove tracks that connects NW tile edge */
		ts &= ~(TRACK_BIT_Y | TRACK_BIT_LEFT | TRACK_BIT_UPPER);
	}
	return CombineTrackStatus(TrackBitsToTrackdirBits(ts), TRACKDIR_BIT_NONE);
}

static bool ClickTile_Water(TileIndex tile)
{
	if (GetWaterTileType(tile) == WATER_TILE_DEPOT) {
		ShowDepotWindow(GetShipDepotNorthTile(tile), VEH_SHIP);
		return true;
	}
	return false;
}

static void ChangeTileOwner_Water(TileIndex tile, Owner old_owner, Owner new_owner)
{
	if (!IsTileOwner(tile, old_owner)) return;

	bool is_lock_middle = IsLock(tile) && GetLockPart(tile) == LOCK_PART_MIDDLE;

	/* No need to dirty company windows here, we'll redraw the whole screen anyway. */
	if (is_lock_middle) Company::Get(old_owner)->infrastructure.water -= 3 * LOCK_DEPOT_TILE_FACTOR; // Lock has three parts.
	if (new_owner != INVALID_OWNER) {
		if (is_lock_middle) Company::Get(new_owner)->infrastructure.water += 3 * LOCK_DEPOT_TILE_FACTOR; // Lock has three parts.
		/* Only subtract from the old owner here if the new owner is valid,
		 * otherwise we clear ship depots and canal water below. */
		if (GetWaterClass(tile) == WATER_CLASS_CANAL && !is_lock_middle) {
			Company::Get(old_owner)->infrastructure.water--;
			Company::Get(new_owner)->infrastructure.water++;
		}
		if (IsShipDepot(tile)) {
			Company::Get(old_owner)->infrastructure.water -= LOCK_DEPOT_TILE_FACTOR;
			Company::Get(new_owner)->infrastructure.water += LOCK_DEPOT_TILE_FACTOR;
		}

		SetTileOwner(tile, new_owner);
		return;
	}

	/* Remove depot */
	if (IsShipDepot(tile)) Command<CMD_LANDSCAPE_CLEAR>::Do({DoCommandFlag::Execute, DoCommandFlag::Bankrupt}, tile);

	/* Set owner of canals and locks ... and also canal under dock there was before.
	 * Check if the new owner after removing depot isn't OWNER_WATER. */
	if (IsTileOwner(tile, old_owner)) {
		if (GetWaterClass(tile) == WATER_CLASS_CANAL && !is_lock_middle) Company::Get(old_owner)->infrastructure.water--;
		SetTileOwner(tile, OWNER_NONE);
	}
}

static VehicleEnterTileStates VehicleEnter_Water(Vehicle *, TileIndex, int, int)
{
	return {};
}

static CommandCost TerraformTile_Water(TileIndex tile, DoCommandFlags flags, int, Slope)
{
	/* Canals can't be terraformed */
	if (IsWaterTile(tile) && IsCanal(tile)) return CommandCost(STR_ERROR_MUST_DEMOLISH_CANAL_FIRST);

	return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile);
}


extern const TileTypeProcs _tile_type_water_procs = {
	DrawTile_Water,           // draw_tile_proc
	GetSlopePixelZ_Water,     // get_slope_z_proc
	ClearTile_Water,          // clear_tile_proc
	nullptr,                     // add_accepted_cargo_proc
	GetTileDesc_Water,        // get_tile_desc_proc
	GetTileTrackStatus_Water, // get_tile_track_status_proc
	ClickTile_Water,          // click_tile_proc
	nullptr,                     // animate_tile_proc
	TileLoop_Water,           // tile_loop_proc
	ChangeTileOwner_Water,    // change_tile_owner_proc
	nullptr,                     // add_produced_cargo_proc
	VehicleEnter_Water,       // vehicle_enter_tile_proc
	GetFoundation_Water,      // get_foundation_proc
	TerraformTile_Water,      // terraform_tile_proc
};
