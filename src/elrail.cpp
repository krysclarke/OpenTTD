/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file elrail.cpp
 * This file deals with displaying wires and pylons for electric railways.
 * <h2>Basics</h2>
 *
 * <h3>Tile Types</h3>
 *
 * We have two different types of tiles in the drawing code:
 * Normal Railway Tiles (NRTs) which can have more than one track on it, and
 * Special Railways tiles (SRTs) which have only one track (like crossings, depots
 * stations, etc).
 *
 * <h3>Location Categories</h3>
 *
 * All tiles are categorized into three location groups (TLG):
 * Group 0: Tiles with both an even X coordinate and an even Y coordinate
 * Group 1: Tiles with an even X and an odd Y coordinate
 * Group 2: Tiles with an odd X and an even Y coordinate
 * Group 3: Tiles with both an odd X and Y coordinate.
 *
 * <h3>Pylon Points</h3>
 * <h4>Control Points</h4>
 * A Pylon Control Point (PCP) is a position where a wire (or rather two)
 * is mounted onto a pylon.
 * Each NRT does contain 4 PCPs which are bitmapped to a byte
 * variable and are represented by the DiagDirection enum
 *
 * Each track ends on two PCPs and thus requires one pylon on each end. However,
 * there is one exception: Straight-and-level tracks only have one pylon every
 * other tile.
 *
 * Now on each edge there are two PCPs: One from each adjacent tile. Both PCPs
 * are merged using an OR operation (i. e. if one tile needs a PCP at the position
 * in question, both tiles get it).
 *
 * <h4>Position Points</h4>
 * A Pylon Position Point (PPP) is a position where a pylon is located on the
 * ground.  Each PCP owns 8 in (45 degree steps) PPPs that are located around
 * it. PPPs are represented using the Direction enum. Each track bit has PPPs
 * that are impossible (because the pylon would be situated on the track) and
 * some that are preferred (because the pylon would be rectangular to the track).
 *
 * @image html elrail_tile.png
 * @image html elrail_track.png
 *
 */

#include "stdafx.h"
#include "station_map.h"
#include "viewport_func.h"
#include "train.h"
#include "rail_gui.h"
#include "tunnelbridge_map.h"
#include "tunnelbridge.h"
#include "elrail_func.h"
#include "company_base.h"
#include "newgrf_railtype.h"

#include "table/elrail_data.h"

#include "safeguards.h"

/**
 * Get the tile location group of a tile.
 * @param t The tile to get the tile location group of.
 * @return The tile location group.
 */
static inline TileLocationGroup GetTileLocationGroup(TileIndex t)
{
	return static_cast<TileLocationGroup>((GB(TileX(t), 0, 1) << 1) + GB(TileY(t), 0, 1));
}

/**
 * Finds which Electrified Rail Bits are present on a given tile.
 * @param t tile to check
 * @param override pointer to PCP override, can be nullptr
 * @return trackbits of tile if it is electrified
 */
static TrackBits GetRailTrackBitsUniversal(TileIndex t, DiagDirections *override)
{
	switch (GetTileType(t)) {
		case MP_RAILWAY:
			if (!HasRailCatenary(GetRailType(t))) return TRACK_BIT_NONE;
			switch (GetRailTileType(t)) {
				case RAIL_TILE_NORMAL: case RAIL_TILE_SIGNALS:
					return GetTrackBits(t);
				default:
					return TRACK_BIT_NONE;
			}
			break;

		case MP_TUNNELBRIDGE:
			if (GetTunnelBridgeTransportType(t) != TRANSPORT_RAIL) return TRACK_BIT_NONE;
			if (!HasRailCatenary(GetRailType(t))) return TRACK_BIT_NONE;
			if (override != nullptr && (IsTunnel(t) || GetTunnelBridgeLength(t, GetOtherBridgeEnd(t)) > 0)) {
				*override = GetTunnelBridgeDirection(t);
			}
			return DiagDirToDiagTrackBits(GetTunnelBridgeDirection(t));

		case MP_ROAD:
			if (!IsLevelCrossing(t)) return TRACK_BIT_NONE;
			if (!HasRailCatenary(GetRailType(t))) return TRACK_BIT_NONE;
			return GetCrossingRailBits(t);

		case MP_STATION:
			if (!HasStationRail(t)) return TRACK_BIT_NONE;
			if (!HasRailCatenary(GetRailType(t))) return TRACK_BIT_NONE;
			return TrackToTrackBits(GetRailStationTrack(t));

		default:
			return TRACK_BIT_NONE;
	}
}

/**
 * Masks out track bits when neighbouring tiles are unelectrified.
 */
static TrackBits MaskWireBits(TileIndex t, TrackBits tracks)
{
	/* Single track bits are never masked out. */
	if (HasAtMostOneBit(tracks)) [[likely]] return tracks;

	if (!IsPlainRailTile(t)) return tracks;

	TrackdirBits neighbour_tdb = TRACKDIR_BIT_NONE;
	for (DiagDirection d = DIAGDIR_BEGIN; d < DIAGDIR_END; d++) {
		/* If the neighbour tile is either not electrified or has no tracks that can be reached
		 * from this tile, mark all trackdirs that can be reached from the neighbour tile
		 * as needing no catenary. We make an exception for blocked station tiles with a matching
		 * axis that still display wires to preserve visual continuity. */
		TileIndex next_tile = TileAddByDiagDir(t, d);
		RailType rt = GetTileRailType(next_tile);
		if (rt == INVALID_RAILTYPE || !HasRailCatenary(rt) ||
				((TrackStatusToTrackBits(GetTileTrackStatus(next_tile, TRANSPORT_RAIL, 0)) & DiagdirReachesTracks(d)) == TRACK_BIT_NONE &&
				(!HasStationTileRail(next_tile) || GetRailStationAxis(next_tile) != DiagDirToAxis(d) || !CanStationTileHaveWires(next_tile)))) {
			neighbour_tdb |= DiagdirReachesTrackdirs(ReverseDiagDir(d));
		}
	}

	/* If the tracks from either a diagonal crossing or don't overlap, both
	 * trackdirs have to be marked to mask the corresponding track bit. Else
	 * one marked trackdir is enough the mask the track bit. */
	TrackBits mask;
	if (tracks == TRACK_BIT_CROSS || !TracksOverlap(tracks)) {
		/* If the tracks form either a diagonal crossing or don't overlap, both
		 * trackdirs have to be marked to mask the corresponding track bit. */
		mask = ~(TrackBits)((neighbour_tdb & (neighbour_tdb >> 8)) & TRACK_BIT_MASK);
		/* If that results in no masked tracks and it is not a diagonal crossing,
		 * require only one marked trackdir to mask. */
		if (tracks != TRACK_BIT_CROSS && (mask & TRACK_BIT_MASK) == TRACK_BIT_MASK) mask = ~TrackdirBitsToTrackBits(neighbour_tdb);
	} else {
		/* Require only one marked trackdir to mask the track. */
		mask = ~TrackdirBitsToTrackBits(neighbour_tdb);
		/* If that results in an empty set, require both trackdirs for diagonal track. */
		if ((tracks & mask) == TRACK_BIT_NONE) {
			if ((neighbour_tdb & TRACKDIR_BIT_X_NE) == 0 || (neighbour_tdb & TRACKDIR_BIT_X_SW) == 0) mask |= TRACK_BIT_X;
			if ((neighbour_tdb & TRACKDIR_BIT_Y_NW) == 0 || (neighbour_tdb & TRACKDIR_BIT_Y_SE) == 0) mask |= TRACK_BIT_Y;
			/* If that still is not enough, require both trackdirs for any track. */
			if ((tracks & mask) == TRACK_BIT_NONE) mask = ~(TrackBits)((neighbour_tdb & (neighbour_tdb >> 8)) & TRACK_BIT_MASK);
		}
	}

	/* Mask the tracks only if at least one track bit would remain. */
	return (tracks & mask) != TRACK_BIT_NONE ? tracks & mask : tracks;
}

/**
 * Get the base wire sprite to use.
 */
static inline SpriteID GetWireBase(TileIndex tile, TileContext context = TCX_NORMAL)
{
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(tile));
	SpriteID wires = GetCustomRailSprite(rti, tile, RTSG_WIRES, context);
	return wires == 0 ? SPR_WIRE_BASE : wires;
}

/**
 * Get the base pylon sprite to use.
 */
static inline SpriteID GetPylonBase(TileIndex tile, TileContext context = TCX_NORMAL)
{
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(tile));
	SpriteID pylons = GetCustomRailSprite(rti, tile, RTSG_PYLONS, context);
	return pylons == 0 ? SPR_PYLON_BASE : pylons;
}

/**
 * Corrects the tileh for certain tile types. Returns an effective tileh for the track on the tile.
 * @param tile The tile to analyse
 * @param *tileh the tileh
 */
static void AdjustTileh(TileIndex tile, Slope *tileh)
{
	if (IsTileType(tile, MP_TUNNELBRIDGE)) {
		if (IsTunnel(tile)) {
			*tileh = SLOPE_STEEP; // XXX - Hack to make tunnel entrances to always have a pylon
		} else if (*tileh != SLOPE_FLAT) {
			*tileh = SLOPE_FLAT;
		} else {
			*tileh = InclinedSlope(GetTunnelBridgeDirection(tile));
		}
	}
}

/**
 * Returns the Z position of a Pylon Control Point.
 *
 * @param tile The tile the pylon should stand on.
 * @param pcp_pos The PCP of the tile.
 * @return The Z position of the PCP.
 */
static int GetPCPElevation(TileIndex tile, DiagDirection pcp_pos)
{
	/* The elevation of the "pylon"-sprite should be the elevation at the PCP.
	 * PCPs are always on a tile edge.
	 *
	 * This position can be outside of the tile, i.e. ?_pcp_offset == TILE_SIZE > TILE_SIZE - 1.
	 * So we have to move it inside the tile, because if the neighboured tile has a foundation,
	 * that does not smoothly connect to the current tile, we will get a wrong elevation from GetSlopePixelZ().
	 *
	 * When we move the position inside the tile, we will get a wrong elevation if we have a slope.
	 * To catch all cases we round the Z position to the next (TILE_HEIGHT / 2).
	 * This will return the correct elevation for slopes and will also detect non-continuous elevation on edges.
	 *
	 * Also note that the result of GetSlopePixelZ() is very special on bridge-ramps.
	 */

	int z = GetSlopePixelZ(TileX(tile) * TILE_SIZE + std::min<int8_t>(_x_pcp_offsets[pcp_pos], TILE_SIZE - 1),
	                       TileY(tile) * TILE_SIZE + std::min<int8_t>(_y_pcp_offsets[pcp_pos], TILE_SIZE - 1), true);
	/* Round the Z to the nearest half tile height. */
	static const uint HALF_TILE_HEIGHT = TILE_HEIGHT / 2;
	return (z + HALF_TILE_HEIGHT / 2) / HALF_TILE_HEIGHT * HALF_TILE_HEIGHT;
}

/**
 * Draws wires on a tunnel tile
 *
 * DrawTile_TunnelBridge() calls this function to draw the wires as SpriteCombine with the tunnel roof.
 *
 * @param ti The Tileinfo to draw the tile for
 */
void DrawRailCatenaryOnTunnel(const TileInfo *ti)
{
	DiagDirection dir = GetTunnelBridgeDirection(ti->tile);

	SpriteID wire_base = GetWireBase(ti->tile);

	const SortableSpriteStruct &sss = _rail_catenary_sprite_data_tunnel[dir];
	AddSortableSpriteToDraw(wire_base + sss.image_offset, PAL_NONE, ti->x, ti->y, GetTilePixelZ(ti->tile), sss, IsTransparencySet(TO_CATENARY));
}

/**
 * Draws wires and, if required, pylons on a given tile
 * @param ti The Tileinfo to draw the tile for
 */
static void DrawRailCatenaryRailway(const TileInfo *ti)
{
	/* Pylons are placed on a tile edge, so we need to take into account
	 * the track configuration of 2 adjacent tiles. trackconfig[0] stores the
	 * current tile (home tile) while [1] holds the neighbour */
	TrackBits track_config[TS_END];
	TrackBits wire_config[TS_END];
	bool is_flat[TS_END];
	/* Note that ti->tileh has already been adjusted for Foundations */
	Slope tileh[TS_END] = { ti->tileh, SLOPE_FLAT };

	/* Half tile slopes coincide only with horizontal/vertical track.
	 * Faking a flat slope results in the correct sprites on positions. */
	Corner halftile_corner = CORNER_INVALID;
	if (IsHalftileSlope(tileh[TS_HOME])) {
		halftile_corner = GetHalftileSlopeCorner(tileh[TS_HOME]);
		tileh[TS_HOME] = SLOPE_FLAT;
	}

	TileLocationGroup tlg = GetTileLocationGroup(ti->tile);
	DiagDirections pcp_status{};
	DiagDirections override_pcp{};
	std::array<Directions, DIAGDIR_END> ppp_preferred{};
	std::array<Directions, DIAGDIR_END> ppp_allowed{};

	/* Find which rail bits are present, and select the override points.
	 * We don't draw a pylon:
	 * 1) INSIDE a tunnel (we wouldn't see it anyway)
	 * 2) on the "far" end of a bridge head (the one that connects to bridge middle),
	 *    because that one is drawn on the bridge. Exception is for length 0 bridges
	 *    which have no middle tiles */
	track_config[TS_HOME] = GetRailTrackBitsUniversal(ti->tile, &override_pcp);
	wire_config[TS_HOME] = MaskWireBits(ti->tile, track_config[TS_HOME]);
	/* If a track bit is present that is not in the main direction, the track is level */
	is_flat[TS_HOME] = ((track_config[TS_HOME] & (TRACK_BIT_HORZ | TRACK_BIT_VERT)) != 0);

	AdjustTileh(ti->tile, &tileh[TS_HOME]);

	SpriteID pylon_normal = GetPylonBase(ti->tile);
	SpriteID pylon_halftile = (halftile_corner != CORNER_INVALID) ? GetPylonBase(ti->tile, TCX_UPPER_HALFTILE) : pylon_normal;

	for (DiagDirection i = DIAGDIR_BEGIN; i < DIAGDIR_END; i++) {
		static const uint edge_corners[] = {
			1 << CORNER_N | 1 << CORNER_E, // DIAGDIR_NE
			1 << CORNER_S | 1 << CORNER_E, // DIAGDIR_SE
			1 << CORNER_S | 1 << CORNER_W, // DIAGDIR_SW
			1 << CORNER_N | 1 << CORNER_W, // DIAGDIR_NW
		};
		SpriteID pylon_base = (halftile_corner != CORNER_INVALID && HasBit(edge_corners[i], halftile_corner)) ? pylon_halftile : pylon_normal;
		TileIndex neighbour = ti->tile + TileOffsByDiagDir(i);
		int elevation = GetPCPElevation(ti->tile, i);

		/* Here's one of the main headaches. GetTileSlope does not correct for possibly
		 * existing foundataions, so we do have to do that manually later on.*/
		tileh[TS_NEIGHBOUR] = GetTileSlope(neighbour);
		track_config[TS_NEIGHBOUR] = GetRailTrackBitsUniversal(neighbour, nullptr);
		wire_config[TS_NEIGHBOUR] = MaskWireBits(neighbour, track_config[TS_NEIGHBOUR]);
		if (IsTunnelTile(neighbour) && i != GetTunnelBridgeDirection(neighbour)) wire_config[TS_NEIGHBOUR] = track_config[TS_NEIGHBOUR] = TRACK_BIT_NONE;

		/* Ignore station tiles that allow neither wires nor pylons. */
		if (IsRailStationTile(neighbour) && !CanStationTileHavePylons(neighbour) && !CanStationTileHaveWires(neighbour)) wire_config[TS_NEIGHBOUR] = track_config[TS_NEIGHBOUR] = TRACK_BIT_NONE;

		/* If the neighboured tile does not smoothly connect to the current tile (because of a foundation),
		 * we have to draw all pillars on the current tile. */
		if (elevation != GetPCPElevation(neighbour, ReverseDiagDir(i))) wire_config[TS_NEIGHBOUR] = track_config[TS_NEIGHBOUR] = TRACK_BIT_NONE;

		is_flat[TS_NEIGHBOUR] = ((track_config[TS_NEIGHBOUR] & (TRACK_BIT_HORZ | TRACK_BIT_VERT)) != 0);

		ppp_preferred[i] = DIRECTIONS_ALL; // We start with preferring everything (end-of-line in any direction)
		ppp_allowed[i] = _allowed_ppp_on_pcp[i];

		/* We cycle through all the existing tracks at a PCP and see what
		 * PPPs we want to have, or may not have at all */
		for (uint k = 0; k < NUM_TRACKS_AT_PCP; k++) {
			/* Next to us, we have a bridge head, don't worry about that one, if it shows away from us */
			if (_track_source_tile[i][k] == TS_NEIGHBOUR &&
			    IsBridgeTile(neighbour) &&
			    GetTunnelBridgeDirection(neighbour) == ReverseDiagDir(i)) {
				continue;
			}

			/* We check whether the track in question (k) is present in the tile
			 * (TrackSourceTile) */
			DiagDirection pcp_pos = i;
			if (HasBit(wire_config[_track_source_tile[i][k]], _tracks_at_pcp[i][k])) {
				/* track found, if track is in the neighbour tile, adjust the number
				 * of the PCP for preferred/allowed determination*/
				pcp_pos = (_track_source_tile[i][k] == TS_HOME) ? i : ReverseDiagDir(i);
				pcp_status.Set(i); // This PCP is in use
				ppp_preferred[i] &= _preferred_ppp_of_track_at_pcp[_tracks_at_pcp[i][k]][pcp_pos];
			}

			if (HasBit(track_config[_track_source_tile[i][k]], _tracks_at_pcp[i][k])) {
				ppp_allowed[i].Reset(_disallowed_ppp_of_track_at_pcp[_tracks_at_pcp[i][k]][pcp_pos]);
			}
		}

		/* Deactivate all PPPs if PCP is not used */
		if (!pcp_status.Test(i)) {
			ppp_preferred[i].Reset();
			ppp_allowed[i].Reset();
		}

		Foundation foundation = FOUNDATION_NONE;

		/* Station and road crossings are always "flat", so adjust the tileh accordingly */
		if (IsTileType(neighbour, MP_STATION) || IsTileType(neighbour, MP_ROAD)) tileh[TS_NEIGHBOUR] = SLOPE_FLAT;

		/* Read the foundations if they are present, and adjust the tileh */
		if (track_config[TS_NEIGHBOUR] != TRACK_BIT_NONE && IsTileType(neighbour, MP_RAILWAY) && HasRailCatenary(GetRailType(neighbour))) foundation = GetRailFoundation(tileh[TS_NEIGHBOUR], track_config[TS_NEIGHBOUR]);
		if (IsBridgeTile(neighbour)) {
			foundation = GetBridgeFoundation(tileh[TS_NEIGHBOUR], DiagDirToAxis(GetTunnelBridgeDirection(neighbour)));
		}

		ApplyFoundationToSlope(foundation, tileh[TS_NEIGHBOUR]);

		/* Half tile slopes coincide only with horizontal/vertical track.
		 * Faking a flat slope results in the correct sprites on positions. */
		if (IsHalftileSlope(tileh[TS_NEIGHBOUR])) tileh[TS_NEIGHBOUR] = SLOPE_FLAT;

		AdjustTileh(neighbour, &tileh[TS_NEIGHBOUR]);

		/* If we have a straight (and level) track, we want a pylon only every 2 tiles
		 * Delete the PCP if this is the case.
		 * Level means that the slope is the same, or the track is flat */
		if (tileh[TS_HOME] == tileh[TS_NEIGHBOUR] || (is_flat[TS_HOME] && is_flat[TS_NEIGHBOUR])) {
			for (uint k = 0; k < NUM_IGNORE_GROUPS; k++) {
				if (ppp_preferred[i] == _ignored_pcp[k][tlg][i]) pcp_status.Reset(i);
			}
		}

		/* Now decide where we draw our pylons. First try the preferred PPPs, but they may not exist.
		 * In that case, we try the any of the allowed ones. if they don't exist either, don't draw
		 * anything. Note that the preferred PPPs still contain the end-of-line markers.
		 * Remove those (simply by ANDing with allowed, since these markers are never allowed) */
		if (ppp_allowed[i].Any(ppp_preferred[i])) ppp_allowed[i] &= ppp_preferred[i];

		if (IsBridgeAbove(ti->tile)) {
			Track bridgetrack = GetBridgeAxis(ti->tile) == AXIS_X ? TRACK_X : TRACK_Y;
			int height = GetBridgeHeight(GetNorthernBridgeEnd(ti->tile));

			if ((height <= GetTileMaxZ(ti->tile) + 1) &&
					(i == _pcp_positions[bridgetrack][0] || i == _pcp_positions[bridgetrack][1])) {
				override_pcp.Set(i);
			}
		}

		if (ppp_allowed[i].Any() && pcp_status.Test(i) && !override_pcp.Test(i) &&
				(!IsRailStationTile(ti->tile) || CanStationTileHavePylons(ti->tile))) {

			const auto &ppp_orders = _ppp_order[i][GetTileLocationGroup(ti->tile)];
			for (Direction k = DIR_BEGIN; k < DIR_END; k++) {
				Direction temp = ppp_orders[k];

				if (ppp_allowed[i].Test(temp)) {
					uint x = ti->x + _x_pcp_offsets[i] + _x_ppp_offsets[temp];
					uint y = ti->y + _y_pcp_offsets[i] + _y_ppp_offsets[temp];

					/* Don't build the pylon if it would be outside the tile */
					if (!_owned_ppp_on_pcp[i].Test(temp)) {
						/* We have a neighbour that will draw it, bail out */
						if (track_config[TS_NEIGHBOUR] != TRACK_BIT_NONE) break;
						continue; // No neighbour, go looking for a better position
					}

					AddSortableSpriteToDraw(pylon_base + _pylon_sprites[temp], PAL_NONE, x, y, elevation,
						{{-1, -1, 0}, {1, 1, BB_HEIGHT_UNDER_BRIDGE}, {1, 1, 0}}, IsTransparencySet(TO_CATENARY));

					break; // We already have drawn a pylon, bail out
				}
			}
		}
	}

	/* The wire above the tunnel is drawn together with the tunnel-roof (see DrawRailCatenaryOnTunnel()) */
	if (IsTunnelTile(ti->tile)) return;

	/* Don't draw a wire under a low bridge */
	if (IsBridgeAbove(ti->tile) && !IsTransparencySet(TO_BRIDGES)) {
		int height = GetBridgeHeight(GetNorthernBridgeEnd(ti->tile));

		if (height <= GetTileMaxZ(ti->tile) + 1) return;
	}

	/* Don't draw a wire if the station tile does not want any */
	if (IsRailStationTile(ti->tile) && !CanStationTileHaveWires(ti->tile)) return;

	SpriteID wire_normal = GetWireBase(ti->tile);
	SpriteID wire_halftile = (halftile_corner != CORNER_INVALID) ? GetWireBase(ti->tile, TCX_UPPER_HALFTILE) : wire_normal;
	Track halftile_track;
	switch (halftile_corner) {
		case CORNER_W: halftile_track = TRACK_LEFT; break;
		case CORNER_S: halftile_track = TRACK_LOWER; break;
		case CORNER_E: halftile_track = TRACK_RIGHT; break;
		case CORNER_N: halftile_track = TRACK_UPPER; break;
		default:       halftile_track = INVALID_TRACK; break;
	}

	/* Drawing of pylons is finished, now draw the wires */
	for (Track t : SetTrackBitIterator(wire_config[TS_HOME])) {
		SpriteID wire_base = (t == halftile_track) ? wire_halftile : wire_normal;
		uint8_t pcp_config = pcp_status.Test(_pcp_positions[t][0]) +
			(pcp_status.Test(_pcp_positions[t][1]) << 1);
		int tileh_selector = !(tileh[TS_HOME] % 3) * tileh[TS_HOME] / 3; // tileh for the slopes, 0 otherwise

		assert(pcp_config != 0); // We have a pylon on neither end of the wire, that doesn't work (since we have no sprites for that)
		assert(!IsSteepSlope(tileh[TS_HOME]));
		const SortableSpriteStruct &sss = _rail_catenary_sprite_data[_rail_wires[tileh_selector][t][pcp_config]];

		/*
		 * The "wire"-sprite position is inside the tile, i.e. 0 <= sss->?_offset < TILE_SIZE.
		 * Therefore it is safe to use GetSlopePixelZ() for the elevation.
		 * Also note that the result of GetSlopePixelZ() is very special for bridge-ramps, so we round the result up or
		 * down to the nearest full height change.
		 */
		int z = (GetSlopePixelZ(ti->x + sss.origin.x, ti->y + sss.origin.y, true) + 4) / 8 * 8;
		AddSortableSpriteToDraw(wire_base + sss.image_offset, PAL_NONE, ti->x, ti->y, z, sss, IsTransparencySet(TO_CATENARY));
	}
}

/**
 * Draws wires on a tunnel tile
 *
 * DrawTile_TunnelBridge() calls this function to draw the wires on the bridge.
 *
 * @param ti The Tileinfo to draw the tile for
 */
void DrawRailCatenaryOnBridge(const TileInfo *ti)
{
	TileIndex end = GetSouthernBridgeEnd(ti->tile);
	TileIndex start = GetOtherBridgeEnd(end);

	uint length = GetTunnelBridgeLength(start, end);
	uint num = GetTunnelBridgeLength(ti->tile, start) + 1;

	Axis axis = GetBridgeAxis(ti->tile);
	TileLocationGroup tlg = GetTileLocationGroup(ti->tile);

	RailCatenarySprite offset = (RailCatenarySprite)(axis == AXIS_X ? 0 : WIRE_Y_FLAT_BOTH - WIRE_X_FLAT_BOTH);

	const SortableSpriteStruct *sss;
	if ((length % 2) && num == length) {
		/* Draw the "short" wire on the southern end of the bridge
		 * only needed if the length of the bridge is odd */
		sss = &_rail_catenary_sprite_data[WIRE_X_FLAT_BOTH + offset];
	} else {
		/* Draw "long" wires on all other tiles of the bridge (one pylon every two tiles) */
		sss = &_rail_catenary_sprite_data[WIRE_X_FLAT_SW + (num % 2) + offset];
	}

	uint height = GetBridgePixelHeight(end);

	SpriteID wire_base = GetWireBase(end, TCX_ON_BRIDGE);

	AddSortableSpriteToDraw(wire_base + sss->image_offset, PAL_NONE, ti->x, ti->y, height, *sss, IsTransparencySet(TO_CATENARY));

	SpriteID pylon_base = GetPylonBase(end, TCX_ON_BRIDGE);

	static constexpr SpriteBounds pylon_bounds{{-1, -1, 0}, {1, 1, BB_HEIGHT_UNDER_BRIDGE}, {1, 1, 0}};

	/* Finished with wires, draw pylons
	 * every other tile needs a pylon on the northern end */
	if (num % 2) {
		DiagDirection pcp_pos = (axis == AXIS_X ? DIAGDIR_NE : DIAGDIR_NW);
		Direction ppp_pos = (axis == AXIS_X ? DIR_NW : DIR_NE);
		if (HasBit(tlg, (axis == AXIS_X ? 0 : 1))) ppp_pos = ReverseDir(ppp_pos);
		uint x = ti->x + _x_pcp_offsets[pcp_pos] + _x_ppp_offsets[ppp_pos];
		uint y = ti->y + _y_pcp_offsets[pcp_pos] + _y_ppp_offsets[ppp_pos];
		AddSortableSpriteToDraw(pylon_base + _pylon_sprites[ppp_pos], PAL_NONE, x, y, height, pylon_bounds, IsTransparencySet(TO_CATENARY));
	}

	/* need a pylon on the southern end of the bridge */
	if (GetTunnelBridgeLength(ti->tile, start) + 1 == length) {
		DiagDirection pcp_pos = (axis == AXIS_X ? DIAGDIR_SW : DIAGDIR_SE);
		Direction ppp_pos = (axis == AXIS_X ? DIR_NW : DIR_NE);
		if (HasBit(tlg, (axis == AXIS_X ? 0 : 1))) ppp_pos = ReverseDir(ppp_pos);
		uint x = ti->x + _x_pcp_offsets[pcp_pos] + _x_ppp_offsets[ppp_pos];
		uint y = ti->y + _y_pcp_offsets[pcp_pos] + _y_ppp_offsets[ppp_pos];
		AddSortableSpriteToDraw(pylon_base + _pylon_sprites[ppp_pos], PAL_NONE, x, y, height, pylon_bounds, IsTransparencySet(TO_CATENARY));
	}
}

/**
 * Draws overhead wires and pylons for electric railways.
 * @param ti The TileInfo struct of the tile being drawn
 * @see DrawRailCatenaryRailway
 */
void DrawRailCatenary(const TileInfo *ti)
{
	switch (GetTileType(ti->tile)) {
		case MP_RAILWAY:
			if (IsRailDepot(ti->tile)) {
				const SortableSpriteStruct &sss = _rail_catenary_sprite_data_depot[GetRailDepotDirection(ti->tile)];

				SpriteID wire_base = GetWireBase(ti->tile);

				/* This wire is not visible with the default depot sprites */
				AddSortableSpriteToDraw(wire_base + sss.image_offset, PAL_NONE, ti->x, ti->y, GetTileMaxPixelZ(ti->tile), sss, IsTransparencySet(TO_CATENARY));
				return;
			}
			break;

		case MP_TUNNELBRIDGE:
		case MP_ROAD:
		case MP_STATION:
			break;

		default: return;
	}
	DrawRailCatenaryRailway(ti);
}

void SettingsDisableElrail(int32_t new_value)
{
	bool disable = (new_value != 0);
	UpdateDisableElrailSettingState(disable, true);
}

void UpdateDisableElrailSettingState(bool disable, bool update_vehicles)
{
	/* pick appropriate railtype for elrail engines depending on setting */
	const RailType new_railtype = disable ? RAILTYPE_RAIL : RAILTYPE_ELECTRIC;

	/* walk through all train engines */
	for (Engine *e : Engine::IterateType(VEH_TRAIN)) {
		RailVehicleInfo *rv_info = &e->u.rail;
		/* update railtype of engines intended to use elrail */
		if (rv_info->intended_railtype == RAILTYPE_ELECTRIC) {
			rv_info->railtype = new_railtype;
		}
	}

	/* when disabling elrails, make sure that all existing trains can run on
	 *  normal rail too */
	if (disable) {
		for (Train *t : Train::Iterate()) {
			if (t->railtype == RAILTYPE_ELECTRIC) {
				/* this railroad vehicle is now compatible only with elrail,
				 *  so add there also normal rail compatibility */
				t->compatible_railtypes.Set(RAILTYPE_RAIL);
				t->railtype = RAILTYPE_RAIL;
				t->flags.Set(VehicleRailFlag::AllowedOnNormalRail);
			}
		}
	}

	/* Fix the total power and acceleration for trains */
	if (update_vehicles) {
		for (Train *t : Train::Iterate()) {
			/* power and acceleration is cached only for front engines */
			if (t->IsFrontEngine()) {
				t->ConsistChanged(CCF_TRACK);
			}
		}
	}

	for (Company *c : Company::Iterate()) c->avail_railtypes = GetCompanyRailTypes(c->index);

	/* This resets the _last_built_railtype, which will be invalid for electric
	 * rails. It may have unintended consequences if that function is ever
	 * extended, though. */
	ReinitGuiAfterToggleElrail(disable);
}
