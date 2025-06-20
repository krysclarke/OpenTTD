/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file music.cpp The songs that OpenTTD knows. */

#include "stdafx.h"
#include "string_func.h"
#include "base_media_func.h"
#include "base_media_music.h"
#include "random_access_file_type.h"
#include "core/string_consumer.hpp"

#include "safeguards.h"


/**
 * Read the name of a music CAT file entry.
 * @param filename Name of CAT file to read from
 * @param entrynum Index of entry whose name to read
 * @return Name of CAT file entry if it could be read.
 */
std::optional<std::string> GetMusicCatEntryName(const std::string &filename, size_t entrynum)
{
	if (!FioCheckFileExists(filename, BASESET_DIR)) return std::nullopt;

	RandomAccessFile file(filename, BASESET_DIR);
	uint32_t ofs = file.ReadDword();
	size_t entry_count = ofs / 8;
	if (entrynum >= entry_count) return std::nullopt;

	file.SeekTo(entrynum * 8, SEEK_SET);
	file.SeekTo(file.ReadDword(), SEEK_SET);
	uint8_t namelen = file.ReadByte();

	std::string name(namelen, '\0');
	file.ReadBlock(name.data(), namelen);
	return StrMakeValid(name);
}

/**
 * Read the full data of a music CAT file entry.
 * @param filename Name of CAT file to read from.
 * @param entrynum Index of entry to read
 * @return Data of CAT file entry.
 */
std::optional<std::vector<uint8_t>> GetMusicCatEntryData(const std::string &filename, size_t entrynum)
{
	if (!FioCheckFileExists(filename, BASESET_DIR)) return std::nullopt;

	RandomAccessFile file(filename, BASESET_DIR);
	uint32_t ofs = file.ReadDword();
	size_t entry_count = ofs / 8;
	if (entrynum >= entry_count) return std::nullopt;

	file.SeekTo(entrynum * 8, SEEK_SET);
	size_t entrypos = file.ReadDword();
	size_t entrylen = file.ReadDword();
	file.SeekTo(entrypos, SEEK_SET);
	file.SkipBytes(file.ReadByte());

	std::vector<uint8_t> data(entrylen);
	file.ReadBlock(data.data(), entrylen);
	return data;
}

/** Names corresponding to the music set's files */
static const std::string_view _music_file_names[] = {
	"theme",
	"old_0", "old_1", "old_2", "old_3", "old_4", "old_5", "old_6", "old_7", "old_8", "old_9",
	"new_0", "new_1", "new_2", "new_3", "new_4", "new_5", "new_6", "new_7", "new_8", "new_9",
	"ezy_0", "ezy_1", "ezy_2", "ezy_3", "ezy_4", "ezy_5", "ezy_6", "ezy_7", "ezy_8", "ezy_9",
};
/** Make sure we aren't messing things up. */
static_assert(lengthof(_music_file_names) == NUM_SONGS_AVAILABLE);

template <>
/* static */ std::span<const std::string_view> BaseSet<MusicSet>::GetFilenames()
{
	return _music_file_names;
}

template <>
/* static */ std::string_view BaseMedia<MusicSet>::GetExtension()
{
	return ".obm"; // OpenTTD Base Music
}

template <>
/* static */ bool BaseMedia<MusicSet>::DetermineBestSet()
{
	if (BaseMedia<MusicSet>::used_set != nullptr) return true;

	const MusicSet *best = nullptr;
	for (const auto &c : BaseMedia<MusicSet>::available_sets) {
		if (c->GetNumMissing() != 0) continue;

		if (best == nullptr ||
				(best->fallback && !c->fallback) ||
				best->valid_files < c->valid_files ||
				(best->valid_files == c->valid_files &&
					(best->shortname == c->shortname && best->version < c->version))) {
			best = c.get();
		}
	}

	BaseMedia<MusicSet>::used_set = best;
	return BaseMedia<MusicSet>::used_set != nullptr;
}

template class BaseMedia<MusicSet>;

bool MusicSet::FillSetDetails(const IniFile &ini, const std::string &path, const std::string &full_filename)
{
	bool ret = this->BaseSet<MusicSet>::FillSetDetails(ini, path, full_filename);
	if (ret) {
		this->num_available = 0;
		const IniGroup *names = ini.GetGroup("names");
		const IniGroup *catindex = ini.GetGroup("catindex");
		const IniGroup *timingtrim = ini.GetGroup("timingtrim");
		uint tracknr = 1;
		for (uint i = 0; i < lengthof(this->songinfo); i++) {
			const std::string &filename = this->files[i].filename;
			if (filename.empty() || this->files[i].check_result == MD5File::CR_NO_FILE) {
				continue;
			}

			this->songinfo[i].filename = filename; // non-owned pointer

			const IniItem *item = catindex != nullptr ? catindex->GetItem(_music_file_names[i]) : nullptr;
			if (item != nullptr && item->value.has_value() && !item->value->empty()) {
				/* Song has a CAT file index, assume it's MPS MIDI format */
				this->songinfo[i].filetype = MTT_MPSMIDI;
				auto value = ParseInteger(*item->value);
				if (!value.has_value()) {
					Debug(grf, 0, "Invalid base music set song index: {}/{}", filename, *item->value);
					continue;
				}
				this->songinfo[i].cat_index = *value;
				auto songname = GetMusicCatEntryName(filename, this->songinfo[i].cat_index);
				if (!songname.has_value()) {
					Debug(grf, 0, "Base music set song missing from CAT file: {}/{}", filename, this->songinfo[i].cat_index);
					continue;
				}
				this->songinfo[i].songname = *songname;
			} else {
				this->songinfo[i].filetype = MTT_STANDARDMIDI;
			}

			std::string_view trimmed_filename{filename};
			/* As we possibly add a path to the filename and we compare
			 * on the filename with the path as in the .obm, we need to
			 * keep stripping path elements until we find a match. */
			while (!trimmed_filename.empty()) {
				/* Remove possible double path separator characters from
				 * the beginning, so we don't start reading e.g. root. */
				while (trimmed_filename.starts_with(PATHSEPCHAR)) trimmed_filename.remove_prefix(1);

				item = names != nullptr ? names->GetItem(trimmed_filename) : nullptr;
				if (item != nullptr && item->value.has_value() && !item->value->empty()) break;

				auto next = trimmed_filename.find(PATHSEPCHAR);
				if (next == std::string_view::npos) {
					trimmed_filename = {};
				} else {
					trimmed_filename.remove_prefix(next);
				}
			}

			if (this->songinfo[i].filetype == MTT_STANDARDMIDI) {
				if (item != nullptr && item->value.has_value() && !item->value->empty()) {
					this->songinfo[i].songname = item->value.value();
				} else {
					Debug(grf, 0, "Base music set song name missing: {}", filename);
					return false;
				}
			}
			this->num_available++;

			/* Number the theme song (if any) track 0, rest are normal */
			if (i == 0) {
				this->songinfo[i].tracknr = 0;
			} else {
				this->songinfo[i].tracknr = tracknr++;
			}

			item = !trimmed_filename.empty() && timingtrim != nullptr ? timingtrim->GetItem(trimmed_filename) : nullptr;
			if (item != nullptr && item->value.has_value() && !item->value->empty()) {
				StringConsumer consumer{*item->value};
				auto start = consumer.TryReadIntegerBase<uint>(10);
				auto valid = consumer.ReadIf(":");
				auto end = consumer.TryReadIntegerBase<uint>(10);
				if (start.has_value() && valid && end.has_value() && !consumer.AnyBytesLeft()) {
					this->songinfo[i].override_start = *start;
					this->songinfo[i].override_end = *end;
				}
			}
		}
	}
	return ret;
}
