/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file tile_cmd.h Generic 'commands' that can be performed on all tiles. */

#ifndef TILE_CMD_H
#define TILE_CMD_H

#include "core/geometry_type.hpp"
#include "command_type.h"
#include "vehicle_type.h"
#include "cargo_type.h"
#include "track_type.h"
#include "tile_map.h"
#include "timer/timer_game_calendar.h"

enum class VehicleEnterTileState : uint8_t {
	EnteredStation, ///< The vehicle entered a station
	EnteredWormhole, ///< The vehicle either entered a bridge, tunnel or depot tile (this includes the last tile of the bridge/tunnel)
	CannotEnter, ///< The vehicle cannot enter the tile
};

using VehicleEnterTileStates = EnumBitSet<VehicleEnterTileState, uint8_t>;

/** Tile information, used while rendering the tile */
struct TileInfo : Coord3D<int> {
	Slope tileh;    ///< Slope of the tile
	TileIndex tile; ///< Tile index
};

/** Tile description for the 'land area information' tool */
struct TileDesc {
	StringID str{}; ///< Description of the tile
	uint64_t dparam = 0; ///< Parameter of the \a str string
	std::array<Owner, 4> owner{}; ///< Name of the owner(s)
	std::array<StringID, 4> owner_type{}; ///< Type of each owner
	TimerGameCalendar::Date build_date = CalendarTime::INVALID_DATE; ///< Date of construction of tile contents
	StringID station_class{}; ///< Class of station
	StringID station_name{}; ///< Type of station within the class
	StringID airport_class{}; ///< Name of the airport class
	StringID airport_name{}; ///< Name of the airport
	StringID airport_tile_name{}; ///< Name of the airport tile
	std::optional<std::string> grf = std::nullopt; ///< newGRF used for the tile contents
	StringID railtype{}; ///< Type of rail on the tile.
	uint16_t rail_speed = 0; ///< Speed limit of rail (bridges and track)
	StringID roadtype{}; ///< Type of road on the tile.
	uint16_t road_speed = 0; ///< Speed limit of road (bridges and track)
	StringID tramtype{}; ///< Type of tram on the tile.
	uint16_t tram_speed = 0; ///< Speed limit of tram (bridges and track)
	std::optional<bool> town_can_upgrade = std::nullopt; ///< Whether the town can upgrade this house during town growth.
};

/**
 * Tile callback function signature for drawing a tile and its contents to the screen
 * @param ti Information about the tile to draw
 */
typedef void DrawTileProc(TileInfo *ti);

/**
 * Tile callback function signature for obtaining the world \c Z coordinate of a given
 * point of a tile.
 *
 * @param tile The queries tile for the Z coordinate.
 * @param x World X coordinate in tile "units".
 * @param y World Y coordinate in tile "units".
 * @param ground_vehicle Whether to get the Z coordinate of the ground vehicle, or the ground.
 * @return World Z coordinate at tile ground (vehicle) level, including slopes and foundations.
 * @see GetSlopePixelZ
 */
typedef int GetSlopeZProc(TileIndex tile, uint x, uint y, bool ground_vehicle);
typedef CommandCost ClearTileProc(TileIndex tile, DoCommandFlags flags);

/**
 * Tile callback function signature for obtaining cargo acceptance of a tile
 * @param tile            Tile queried for its accepted cargo
 * @param acceptance      Storage destination of the cargo acceptance in 1/8
 * @param always_accepted Bitmask of always accepted cargo types
 */
typedef void AddAcceptedCargoProc(TileIndex tile, CargoArray &acceptance, CargoTypes &always_accepted);

/**
 * Tile callback function signature for obtaining a tile description
 * @param tile Tile being queried
 * @param td   Storage pointer for returned tile description
 */
typedef void GetTileDescProc(TileIndex tile, TileDesc &td);

/**
 * Tile callback function signature for getting the possible tracks
 * that can be taken on a given tile by a given transport.
 *
 * The return value contains the existing trackdirs and signal states.
 *
 * see track_func.h for usage of TrackStatus.
 *
 * @param tile     the tile to get the track status from
 * @param mode     the mode of transportation
 * @param sub_mode used to differentiate between different kinds within the mode
 * @return the track status information
 */
typedef TrackStatus GetTileTrackStatusProc(TileIndex tile, TransportType mode, uint sub_mode, DiagDirection side);

/**
 * Tile callback function signature for obtaining the produced cargo of a tile.
 * @param tile      Tile being queried
 * @param produced  Destination array for produced cargo
 */
typedef void AddProducedCargoProc(TileIndex tile, CargoArray &produced);
typedef bool ClickTileProc(TileIndex tile);
typedef void AnimateTileProc(TileIndex tile);
typedef void TileLoopProc(TileIndex tile);
typedef void ChangeTileOwnerProc(TileIndex tile, Owner old_owner, Owner new_owner);

typedef VehicleEnterTileStates VehicleEnterTileProc(Vehicle *v, TileIndex tile, int x, int y);
typedef Foundation GetFoundationProc(TileIndex tile, Slope tileh);

/**
 * Tile callback function signature of the terraforming callback.
 *
 * The function is called when a tile is affected by a terraforming operation.
 * It has to check if terraforming of the tile is allowed and return extra terraform-cost that depend on the tiletype.
 * With DoCommandFlag::Execute in \a flags it has to perform tiletype-specific actions (like clearing land etc., but not the terraforming itself).
 *
 * @note The terraforming has not yet taken place. So GetTileZ() and GetTileSlope() refer to the landscape before the terraforming operation.
 *
 * @param tile      The involved tile.
 * @param flags     Command flags passed to the terraform command (DoCommandFlag::Execute, DoCommandFlag::QueryCost, etc.).
 * @param z_new     TileZ after terraforming.
 * @param tileh_new Slope after terraforming.
 * @return Error code or extra cost for terraforming (like clearing land, building foundations, etc., but not the terraforming itself.)
 */
typedef CommandCost TerraformTileProc(TileIndex tile, DoCommandFlags flags, int z_new, Slope tileh_new);

/**
 * Set of callback functions for performing tile operations of a given tile type.
 * @see TileType
 */
struct TileTypeProcs {
	DrawTileProc *draw_tile_proc;                  ///< Called to render the tile and its contents to the screen
	GetSlopeZProc *get_slope_z_proc;
	ClearTileProc *clear_tile_proc;
	AddAcceptedCargoProc *add_accepted_cargo_proc; ///< Adds accepted cargo of the tile to cargo array supplied as parameter
	GetTileDescProc *get_tile_desc_proc;           ///< Get a description of a tile (for the 'land area information' tool)
	GetTileTrackStatusProc *get_tile_track_status_proc; ///< Get available tracks and status of a tile
	ClickTileProc *click_tile_proc;                ///< Called when tile is clicked
	AnimateTileProc *animate_tile_proc;
	TileLoopProc *tile_loop_proc;
	ChangeTileOwnerProc *change_tile_owner_proc;
	AddProducedCargoProc *add_produced_cargo_proc; ///< Adds produced cargo of the tile to cargo array supplied as parameter
	VehicleEnterTileProc *vehicle_enter_tile_proc; ///< Called when a vehicle enters a tile
	GetFoundationProc *get_foundation_proc;
	TerraformTileProc *terraform_tile_proc;        ///< Called when a terraforming operation is about to take place
};

extern const TileTypeProcs * const _tile_type_procs[16];

TrackStatus GetTileTrackStatus(TileIndex tile, TransportType mode, uint sub_mode, DiagDirection side = INVALID_DIAGDIR);
VehicleEnterTileStates VehicleEnterTile(Vehicle *v, TileIndex tile, int x, int y);
void ChangeTileOwner(TileIndex tile, Owner old_owner, Owner new_owner);
void GetTileDesc(TileIndex tile, TileDesc &td);

inline void AddAcceptedCargo(TileIndex tile, CargoArray &acceptance, CargoTypes *always_accepted)
{
	AddAcceptedCargoProc *proc = _tile_type_procs[GetTileType(tile)]->add_accepted_cargo_proc;
	if (proc == nullptr) return;
	CargoTypes dummy = 0; // use dummy bitmask so there don't need to be several 'always_accepted != nullptr' checks
	proc(tile, acceptance, always_accepted == nullptr ? dummy : *always_accepted);
}

inline void AddProducedCargo(TileIndex tile, CargoArray &produced)
{
	AddProducedCargoProc *proc = _tile_type_procs[GetTileType(tile)]->add_produced_cargo_proc;
	if (proc == nullptr) return;
	proc(tile, produced);
}

/**
 * Test if a tile may be animated.
 * @param tile Tile to test.
 * @returns True iff the type of the tile has a handler for tile animation.
 */
inline bool MayAnimateTile(TileIndex tile)
{
	return _tile_type_procs[GetTileType(tile)]->animate_tile_proc != nullptr;
}

inline void AnimateTile(TileIndex tile)
{
	AnimateTileProc *proc = _tile_type_procs[GetTileType(tile)]->animate_tile_proc;
	assert(proc != nullptr);
	proc(tile);
}

inline bool ClickTile(TileIndex tile)
{
	ClickTileProc *proc = _tile_type_procs[GetTileType(tile)]->click_tile_proc;
	if (proc == nullptr) return false;
	return proc(tile);
}

#endif /* TILE_CMD_H */
