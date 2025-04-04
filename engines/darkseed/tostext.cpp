/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/file.h"
#include "darkseed/darkseed.h"
#include "darkseed/tostext.h"

namespace Darkseed {

bool TosText::load() {
	if (g_engine->isDosDemo()) {
		loadDemoTosEntries();
		return true;
	}
	Common::File tostextFile;
	if (!tostextFile.open("tostext.bin")) {
		return false;
	}
	_numEntries = tostextFile.readUint16LE() / 2;
	_textArray.resize(_numEntries);

	for (int i = 0; i < _numEntries; i++) {
		_textArray[i] = loadString(tostextFile, i);
	}
	return true;
}

void TosText::loadDemoTosEntries() {
	_numEntries = 973;
	_textArray.resize(_numEntries);
	_textArray[10] = "My head is killing me.";
	_textArray[11] = "My head feels like it is going to explode.";
	_textArray[12] = "It seems like I've had this headache since I moved here.";
	_textArray[13] = "I need an asprin or something.";

	_textArray[798] = "You can't touch the horizon!";

	_textArray[802] = "You see the unfortunate victims of the life leech.";

	_textArray[972] = "Holding ";
}

const Common::U32String &TosText::getText(uint16 textIndex) {
	assert(textIndex < _numEntries);
	return _textArray[textIndex];
}

Common::U32String TosText::loadString(Common::File &file, uint16 index) const {
	Common::U32String str;
	file.seek(index * 2, SEEK_SET);
	auto startOffset = file.readUint16LE();
	uint16 strLen = index == _numEntries - 1
						? (uint16)file.size() - startOffset
						: file.readUint16LE() - startOffset;
	file.seek(startOffset, SEEK_SET);
	if (g_engine->getLanguage() == Common::KO_KOR || g_engine->getLanguage() == Common::ZH_ANY) {
		// handle multi-byte languages
		for (int i = 0; i < strLen; i++) {
			uint8 byte = (char)file.readByte();
			if (byte & 0x80) {
				if (i < strLen - 1) {
					uint8 byte2 = (char)file.readByte();
					str += (int)byte << 8 | byte2;
					i++;
				}
			} else {
				str += byte;
			}
		}
	} else {
		for (int i = 0; i < strLen; i++) {
			str += (char)file.readByte();
		}
	}
	return str;
}

uint16 TosText::getNumEntries() const {
	return _numEntries;
}

} // namespace Darkseed
