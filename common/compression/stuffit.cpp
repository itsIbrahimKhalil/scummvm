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

// StuffIt parsing based on https://github.com/mietek/theunarchiver/wiki/StuffItFormat
// Compressions 13 and 14 based on libxad (http://sourceforge.net/projects/libxad/)

#include "common/compression/stuffit.h"

#include "common/archive.h"
#include "common/bitstream.h"
#include "common/debug.h"
#include "common/hash-str.h"
#include "common/hashmap.h"
#include "common/macresman.h"
#include "common/memstream.h"
#include "common/substream.h"
#include "common/crc.h"

namespace Common {

struct SIT14Data;

class StuffItArchive : public Common::MemcachingCaseInsensitiveArchive {
public:
	StuffItArchive();
	~StuffItArchive() override;

	bool open(const Common::Path &filename, bool flattenTree);
	bool open(Common::SeekableReadStream *stream, bool flattenTree);
	void close();
	bool isOpen() const { return _stream != nullptr; }

	// Common::Archive API implementation
	bool hasFile(const Common::Path &path) const override;
	int listMembers(Common::ArchiveMemberList &list) const override;
	const Common::ArchiveMemberPtr getMember(const Common::Path &path) const override;
	Common::SharedArchiveContents readContentsForPath(const Common::Path &name) const override;
	Common::SharedArchiveContents readContentsForPathAltStream(const Common::Path &translatedPath, Common::AltStreamType altStreamType) const override;
	Common::Path translatePath(const Common::Path &path) const override;
	char getPathSeparator() const override;

private:
	struct FileEntryFork {
		FileEntryFork();

		uint32 uncompressedSize;
		uint32 compressedSize;
		uint32 offset;
		uint16 crc;
		byte compression;
	};

	struct FileEntry {
		FileEntryFork dataFork;
		FileEntryFork resFork;
	};

	class StuffItArchiveMember : public Common::GenericArchiveMember {
	public:
		StuffItArchiveMember(const Common::Path &path, const Common::Archive &archive);

		bool isInMacArchive() const override;
	};

	Common::SeekableReadStream *_stream;

	typedef Common::HashMap<Common::Path, FileEntry, Common::Path::IgnoreCase_Hash, Common::Path::IgnoreCase_EqualTo> FileMap;
	FileMap _map;

	typedef Common::HashMap<Common::Path, Common::MacFinderInfoData, Common::Path::IgnoreCase_Hash, Common::Path::IgnoreCase_EqualTo> MetadataMap;
	MetadataMap _metadataMap;

	bool _flattenTree;

	// Decompression Functions
	bool decompress13(Common::SeekableReadStream *src, byte *dst, uint32 uncompressedSize) const;
	void decompress14(Common::SeekableReadStream *src, byte *dst, uint32 uncompressedSize) const;

	// Decompression Helpers
	void update14(uint16 first, uint16 last, byte *code, uint16 *freq) const;
	void readTree14(Common::BitStream8LSB *bits, SIT14Data *dat, uint16 codesize, uint16 *result) const;

	Common::SharedArchiveContents readContentsForPathFork(const Common::Path &translatedPath, bool isResFork) const;
};

StuffItArchive::StuffItArchive() : Common::MemcachingCaseInsensitiveArchive(), _flattenTree(false) {
	_stream = nullptr;
}

StuffItArchive::~StuffItArchive() {
	close();
}

// Some known values of StuffIt FourCC's
// 11H Mac in particular uses ST46, while EMI Mac uses ST65
static const uint32 s_magicNumbers[] = {
	MKTAG('S', 'I', 'T', '!'), MKTAG('S', 'T', '6', '5'), MKTAG('S', 'T', '5', '0'),
	MKTAG('S', 'T', '6', '0'), MKTAG('S', 'T', 'i', 'n'), MKTAG('S', 'T', 'i', '2'),
	MKTAG('S', 'T', 'i', '3'), MKTAG('S', 'T', 'i', '4'), MKTAG('S', 'T', '4', '6')
};

bool StuffItArchive::open(const Common::Path &filename, bool flattenTree) {
	Common::SeekableReadStream *stream = SearchMan.createReadStreamForMember(filename);
	return open(stream, flattenTree);
}

bool StuffItArchive::open(Common::SeekableReadStream *stream, bool flattenTree) {
	close();

	_stream = stream;
	_flattenTree = flattenTree;

	if (!_stream)
		return false;

	uint32 tag = _stream->readUint32BE();

	// Check all the possible FourCC's
	bool found = false;
	for (int i = 0; i < ARRAYSIZE(s_magicNumbers); i++) {
		if (tag == s_magicNumbers[i]) {
			found = true;
			break;
		}
	}

	// Didn't find one, let's bail out
	if (!found) {
		close();
		return false;
	}

	/* uint16 fileCount = */ _stream->readUint16BE();
	uint32 archiveSize = _stream->readUint32BE();

	// Some sort of second magic number
	if (_stream->readUint32BE() != MKTAG('r', 'L', 'a', 'u')) {
		close();
		return false;
	}

	/* byte version = */ _stream->readByte(); // meaning not clear

	_stream->skip(7); // unknown

	Common::CRC16 crc;

	Common::String dirPrefix;

	while (_stream->pos() < _stream->size() && !_stream->eos() && _stream->pos() < archiveSize) {
		const uint kMaxFileLength = 31;

		byte header[112];
		_stream->read(header, sizeof(header));
		Common::MemoryReadStream headStream(header, sizeof(header));
		byte resForkCompression = headStream.readByte();
		byte dataForkCompression = headStream.readByte();

		byte fileNameLength = headStream.readByte();
		Common::String name;

		if (fileNameLength > kMaxFileLength)
			error("File name length too long in stuffit archive: %d at 0x%x", fileNameLength, (int) (_stream->pos() - 3));


		for (byte i = 0; i < fileNameLength; i++)
			name += (char)headStream.readByte();

		// Skip remaining bytes
		headStream.skip(63 - fileNameLength);

		MacFinderInfo finfo;

		headStream.read(finfo.type, 4);
		headStream.read(finfo.creator, 4);
		finfo.flags = headStream.readUint16BE();
		/* uint32 creationDate = */ headStream.readUint32BE();
		/* uint32 modificationDate = */ headStream.readUint32BE();
		uint32 resForkUncompressedSize = headStream.readUint32BE();
		uint32 dataForkUncompressedSize = headStream.readUint32BE();
		uint32 resForkCompressedSize = headStream.readUint32BE();
		uint32 dataForkCompressedSize = headStream.readUint32BE();
		uint16 resForkCRC = headStream.readUint16BE();
		uint16 dataForkCRC = headStream.readUint16BE();
		headStream.skip(6); // unknown
		uint16 headerCRC = headStream.readUint16BE();

		uint16 actualHeaderCRC = crc.crcFast(header, sizeof(header) - 2);

		if (actualHeaderCRC != headerCRC)
			error ("StuffItArchive::open(): Header CRC mismatch: %04x vs %04x", actualHeaderCRC, headerCRC);

		byte dirCheckMethod = (dataForkCompression & 0x6f);	// Strip 0x80 (encrypted) and 0x10 (folder contents encrypted) flags
		if (dirCheckMethod == 32) {
			// Start of folder
			if (!flattenTree)
				dirPrefix = dirPrefix + name + ":";
			continue;
		}

		if (dirCheckMethod == 33) {
			// End of folder
			if (!flattenTree && dirPrefix.size() > 0) {
				size_t secondLastDelimiter = Common::String::npos;
				if (dirPrefix.size() > 1)
					secondLastDelimiter = dirPrefix.rfind(':', dirPrefix.size() - 2);

				if (secondLastDelimiter == Common::String::npos) {
					// Only one level deep
					dirPrefix.clear();
				} else {
					// Multiple levels deep
					dirPrefix = dirPrefix.substr(0, secondLastDelimiter + 1);
				}
			}
			continue;
		}

		if (!flattenTree)
			name = dirPrefix + name;

		Common::Path path(name, ':');

		_metadataMap[path] = finfo.toData();

		if (dataForkUncompressedSize != 0) {
			// We have a data fork

			FileEntryFork &entryFork = _map[path].dataFork;
			entryFork.compression = dataForkCompression;
			entryFork.uncompressedSize = dataForkUncompressedSize;
			entryFork.compressedSize = dataForkCompressedSize;
			entryFork.offset = _stream->pos() + resForkCompressedSize;
			entryFork.crc = dataForkCRC;

			debug(0, "StuffIt file '%s' data fork, Compression = %d", name.c_str(), entryFork.compression);
		}

		if (resForkUncompressedSize != 0) {
			// We have a resource fork

			FileEntryFork &entryFork = _map[path].resFork;
			entryFork.compression = resForkCompression;
			entryFork.uncompressedSize = resForkUncompressedSize;
			entryFork.compressedSize = resForkCompressedSize;
			entryFork.offset = _stream->pos();
			entryFork.crc = resForkCRC;

			debug(0, "StuffIt file '%s' res fork, Compression = %d", name.c_str(), entryFork.compression);
		}

		// Go to the next entry
		_stream->skip(dataForkCompressedSize + resForkCompressedSize);
	}

	return true;
}

void StuffItArchive::close() {
	delete _stream;
	_stream = nullptr;
	_map.clear();
}

bool StuffItArchive::hasFile(const Common::Path &path) const {
	return _map.contains(path);
}

int StuffItArchive::listMembers(Common::ArchiveMemberList &list) const {
	for (const auto &file : _map)
		list.push_back(getMember(file._key));

	return _map.size();
}

const Common::ArchiveMemberPtr StuffItArchive::getMember(const Common::Path &path) const {
	return Common::ArchiveMemberPtr(new StuffItArchiveMember(path, *this));
}

Common::SharedArchiveContents StuffItArchive::readContentsForPath(const Common::Path &path) const {
	return readContentsForPathFork(path, false);
}

Common::SharedArchiveContents StuffItArchive::readContentsForPathAltStream(const Common::Path &translatedPath, Common::AltStreamType altStreamType) const {
	if (altStreamType == Common::AltStreamType::MacFinderInfo) {
		if (_metadataMap.contains(translatedPath)) {
			const Common::MacFinderInfoData &metadata = _metadataMap[translatedPath];
			byte *copy = new byte[sizeof(Common::MacFinderInfoData)];
			memcpy(copy, reinterpret_cast<const byte *>(&metadata), sizeof(Common::MacFinderInfoData));
			return Common::SharedArchiveContents(copy, sizeof(Common::MacFinderInfoData));
		}
		return Common::SharedArchiveContents();
	}

	if (altStreamType == Common::AltStreamType::MacResourceFork)
		return readContentsForPathFork(translatedPath, true);

	return Common::SharedArchiveContents();
}

Common::SharedArchiveContents StuffItArchive::readContentsForPathFork(const Common::Path &path, bool isResFork) const {
	FileMap::const_iterator entryIt = _map.find(path);

	if (entryIt == _map.end())
		return Common::SharedArchiveContents();

	const FileEntry &entry = entryIt->_value;
	const FileEntryFork &entryFork = isResFork ? entry.resFork : entry.dataFork;

	if (entryFork.uncompressedSize == 0) {
		if (isResFork)
			return Common::SharedArchiveContents();
		else
			return Common::SharedArchiveContents(nullptr, 0);	// Treat no data fork as an empty stream
	}

	if (entryFork.compression & 0xF0)
		error("Unhandled StuffIt encryption");

	Common::SeekableSubReadStream subStream(_stream, entryFork.offset, entryFork.offset + entryFork.compressedSize);

	byte *uncompressedBlock = new byte[entryFork.uncompressedSize];

	// We currently only support type 14 compression
	switch (entryFork.compression) {
	case 0: // Uncompressed
		subStream.read(uncompressedBlock, entryFork.uncompressedSize);
		break;
	case 13: // TableHuff
		if (!decompress13(&subStream, uncompressedBlock, entryFork.uncompressedSize))
			error("SIT-13 decompression failed");
		break;
	case 14: // Installer
		decompress14(&subStream, uncompressedBlock, entryFork.uncompressedSize);
		break;
	default:
		error("Unhandled StuffIt compression %d", entryFork.compression);
		return Common::SharedArchiveContents();
	}

	uint16 actualCRC = Common::CRC16().crcFast(uncompressedBlock, entryFork.uncompressedSize);

	if (actualCRC != entryFork.crc) {
		error("StuffItArchive::readContentsForPath(): CRC mismatch: %04x vs %04x for file %s %s fork", actualCRC, entryFork.crc, path.toString().c_str(), (isResFork ? "res" : "data"));
	}

	return Common::SharedArchiveContents(uncompressedBlock, entryFork.uncompressedSize);
}

Common::Path StuffItArchive::translatePath(const Common::Path &path) const {
	return _flattenTree ? path.getLastComponent() : path;
}

char StuffItArchive::getPathSeparator() const {
	return ':';
}

void StuffItArchive::update14(uint16 first, uint16 last, byte *code, uint16 *freq) const {
	uint16 i, j;

	while (last - first > 1) {
		i = first;
		j = last;

		do {
			while (++i < last && code[first] > code[i])
				;

			while (--j > first && code[first] < code[j])
				;

			if (j > i) {
				SWAP(code[i], code[j]);
				SWAP(freq[i], freq[j]);
			}
		} while (j > i);

		if (first != j) {
			SWAP(code[first], code[j]);
			SWAP(freq[first], freq[j]);

			i = j + 1;

			if (last - i <= j - first) {
				update14(i, last, code, freq);
				last = j;
			} else {
				update14(first, j, code, freq);
				first = i;
			}
		} else {
			++first;
		}
	}
}

struct SIT14Data {
	byte code[308];
	byte codecopy[308];
	uint16 freq[308];
	uint32 buff[308];

	byte var1[52];
	uint16 var2[52];
	uint16 var3[75 * 2];

	byte var4[76];
	uint32 var5[75];
	byte var6[1024];
	uint16 var7[308 * 2];
	byte var8[0x4000];

	byte window[0x40000];
};

// Realign to a byte boundary
#define ALIGN_BITS(b) \
	if (b->pos() & 7) \
		b->skip(8 - (b->pos() & 7))

void StuffItArchive::readTree14(Common::BitStream8LSB *bits, SIT14Data *dat, uint16 codesize, uint16 *result) const {
	uint32 i, l, n;
	uint32 k = bits->getBit();
	uint32 j = bits->getBits<2>() + 2;
	uint32 o = bits->getBits<3>() + 1;
	uint32 size = 1 << j;
	uint32 m = size - 1;
	k = k ? (m - 1) : 0xFFFFFFFF;

	if (bits->getBits<2>() & 1) { // skip 1 bit!
		// requirements for this call: dat->buff[32], dat->code[32], dat->freq[32*2]
		readTree14(bits, dat, size, dat->freq);

		for (i = 0; i < codesize; ) {
			l = 0;

			do {
				l = dat->freq[l + bits->getBit()];
				n = size << 1;
			} while (n > l);

			l -= n;

			if (k != l) {
				if (l == m) {
					l = 0;

					do {
						l = dat->freq[l + bits->getBit()];
						n = size <<  1;
					} while (n > l);

					l += 3 - n;

					while (l--) {
						dat->code[i] = dat->code[i - 1];
						++i;
					}
				} else {
					dat->code[i++] = l + o;
				}
			} else {
				dat->code[i++] = 0;
			}
		}
	} else {
		for (i = 0; i < codesize; ) {
			l = bits->getBits(j);

			if (k != l) {
				if (l == m) {
					l = bits->getBits(j) + 3;

					while (l--) {
						dat->code[i] = dat->code[i - 1];
						++i;
					}
				} else {
					dat->code[i++] = l + o;
				}
			} else {
				dat->code[i++] = 0;
			}
		}
	}

	for (i = 0; i < codesize; ++i) {
		dat->codecopy[i] = dat->code[i];
		dat->freq[i] = i;
	}

	update14(0, codesize, dat->codecopy, dat->freq);

	for (i = 0; i < codesize && !dat->codecopy[i]; ++i)
		; // find first nonempty

	for (j = 0; i < codesize; ++i, ++j) {
		if (i)
			j <<= (dat->codecopy[i] - dat->codecopy[i - 1]);

		k = dat->codecopy[i];
		m = 0;

		for (l = j; k--; l >>= 1)
			m = (m << 1) | (l & 1);

		dat->buff[dat->freq[i]] = m;
	}

	for (i = 0; i < (uint32)codesize * 2; ++i)
		result[i] = 0;

	j = 2;

	for (i = 0; i < codesize; ++i) {
		l = 0;
		m = dat->buff[i];

		for (k = 0; k < dat->code[i]; ++k) {
			l += (m & 1);

			if (dat->code[i] - 1 <= (int32)k) {
				result[l] = codesize * 2 + i;
			} else {
				if (!result[l]) {
					result[l] = j;
					j += 2;
				}

				l = result[l];
			}

			m >>= 1;
		}
	}

	ALIGN_BITS(bits);
}

struct SIT13Buffer {
	uint16 data;
	int8  bits;

	SIT13Buffer() : data(0), bits(0) {}
};

struct SIT13Store {
	int16  freq;
	uint16 d1;
	uint16 d2;

	SIT13Store() : freq(0), d1(0), d2(0) {}
};

struct SIT13Data {
	uint16              MaxBits;
	struct SIT13Store  Buffer4[0xE08];
	struct SIT13Buffer Buffer1[0x1000];
	struct SIT13Buffer Buffer2[0x1000];
	struct SIT13Buffer Buffer3[0x1000];
	struct SIT13Buffer Buffer3b[0x1000];
	struct SIT13Buffer Buffer5[0x141];
	uint8              TextBuf[658];
	uint8              Window[0x10000];

	SIT13Data() : MaxBits(0) {
		memset(TextBuf, 0, sizeof(TextBuf));
		memset(Window, 0, sizeof(Window));
	}
};

static const uint8 SIT13Bits[16] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
static const uint16 SIT13Info[37] = {
	0x5D8, 0x058, 0x040, 0x0C0, 0x000, 0x078, 0x02B, 0x014,
	0x00C, 0x01C, 0x01B, 0x00B, 0x010, 0x020, 0x038, 0x018,
	0x0D8, 0xBD8, 0x180, 0x680, 0x380, 0xF80, 0x780, 0x480,
	0x080, 0x280, 0x3D8, 0xFD8, 0x7D8, 0x9D8, 0x1D8, 0x004,
	0x001, 0x002, 0x007, 0x003, 0x008
};
static const uint16 SIT13InfoBits[37] = {
	11,  8,  8,  8,  8,  7,  6,  5,  5,  5,  5,  6,  5,  6,  7,  7,
	9, 12, 10, 11, 11, 12, 12, 11, 11, 11, 12, 12, 12, 12, 12,  5,
	2,  2,  3,  4,  5
};
static const uint16 SIT13StaticPos[5] = {0, 330, 661, 991, 1323};
static const uint8 SIT13StaticBits[5] = {11, 13, 14, 11, 11};
static const uint8 SIT13Static[1655] = {
	0xB8,0x98,0x78,0x77,0x75,0x97,0x76,0x87,0x77,0x77,0x77,0x78,0x67,0x87,0x68,0x67,0x3B,0x77,0x78,0x67,
	0x77,0x77,0x77,0x59,0x76,0x87,0x77,0x77,0x77,0x77,0x77,0x77,0x76,0x87,0x67,0x87,0x77,0x77,0x75,0x88,
	0x59,0x75,0x79,0x77,0x78,0x68,0x77,0x67,0x73,0xB6,0x65,0xB6,0x76,0x97,0x67,0x47,0x9A,0x2A,0x4A,0x87,
	0x77,0x78,0x67,0x86,0x78,0x77,0x77,0x77,0x68,0x77,0x77,0x77,0x68,0x77,0x77,0x77,0x77,0x77,0x77,0x77,
	0x68,0x77,0x77,0x77,0x67,0x87,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x68,0x77,0x77,0x68,0x77,0x77,0x77,
	0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x68,0x77,0x77,0x77,0x77,0x77,0x67,0x87,
	0x68,0x77,0x77,0x77,0x68,0x77,0x68,0x63,0x86,0x7A,0x87,0x77,0x77,0x87,0x76,0x87,0x77,0x77,0x77,0x77,
	0x77,0x77,0x77,0x77,0x77,0x76,0x86,0x77,0x86,0x86,0x86,0x86,0x87,0x76,0x86,0x87,0x67,0x74,0xA7,0x86,
	0x36,0x88,0x78,0x76,0x87,0x76,0x96,0x87,0x77,0x84,0xA6,0x86,0x87,0x76,0x92,0xB5,0x94,0xA6,0x96,0x85,
	0x78,0x75,0x96,0x86,0x86,0x75,0xA7,0x67,0x87,0x85,0x87,0x85,0x95,0x77,0x77,0x85,0xA3,0xA7,0x93,0x87,
	0x86,0x94,0x85,0xA8,0x67,0x85,0xA5,0x95,0x86,0x68,0x67,0x77,0x96,0x78,0x75,0x86,0x77,0xA5,0x67,0x87,
	0x85,0xA6,0x75,0x96,0x85,0x87,0x95,0x95,0x87,0x86,0x94,0xA5,0x86,0x85,0x87,0x86,0x86,0x86,0x86,0x77,
	0x67,0x76,0x66,0x9A,0x75,0xA5,0x94,0x97,0x76,0x96,0x76,0x95,0x86,0x77,0x86,0x87,0x75,0xA5,0x96,0x85,
	0x86,0x96,0x86,0x86,0x85,0x96,0x86,0x76,0x95,0x86,0x95,0x95,0x95,0x87,0x76,0x87,0x76,0x96,0x85,0x78,
	0x75,0xA6,0x85,0x86,0x95,0x86,0x95,0x86,0x45,0x69,0x78,0x77,0x87,0x67,0x69,0x58,0x79,0x68,0x78,0x87,
	0x78,0x66,0x88,0x68,0x68,0x77,0x76,0x87,0x68,0x68,0x69,0x58,0x5A,0x4B,0x76,0x88,0x69,0x67,0xA7,0x70,
	0x9F,0x90,0xA4,0x84,0x77,0x77,0x77,0x89,0x17,0x77,0x7B,0xA7,0x86,0x87,0x77,0x68,0x68,0x69,0x67,0x78,
	0x77,0x78,0x76,0x87,0x77,0x76,0x73,0xB6,0x87,0x96,0x66,0x87,0x76,0x85,0x87,0x78,0x77,0x77,0x86,0x77,
	0x86,0x78,0x66,0x76,0x77,0x87,0x86,0x78,0x76,0x76,0x86,0xA5,0x67,0x97,0x77,0x87,0x87,0x76,0x66,0x59,
	0x67,0x59,0x77,0x6A,0x65,0x86,0x78,0x94,0x77,0x88,0x77,0x78,0x86,0x86,0x76,0x88,0x76,0x87,0x67,0x87,
	0x77,0x77,0x76,0x87,0x86,0x77,0x77,0x77,0x86,0x86,0x76,0x96,0x77,0x77,0x76,0x78,0x86,0x86,0x86,0x95,
	0x86,0x96,0x85,0x95,0x86,0x87,0x75,0x88,0x77,0x87,0x57,0x78,0x76,0x86,0x76,0x96,0x86,0x87,0x76,0x87,
	0x86,0x76,0x77,0x86,0x78,0x78,0x57,0x87,0x86,0x76,0x85,0xA5,0x87,0x76,0x86,0x86,0x85,0x86,0x53,0x98,
	0x78,0x78,0x77,0x87,0x79,0x67,0x79,0x85,0x87,0x69,0x67,0x68,0x78,0x69,0x68,0x69,0x58,0x87,0x66,0x97,
	0x68,0x68,0x76,0x85,0x78,0x87,0x67,0x97,0x67,0x74,0xA2,0x28,0x77,0x78,0x77,0x77,0x78,0x68,0x67,0x78,
	0x77,0x78,0x68,0x68,0x77,0x59,0x67,0x5A,0x68,0x68,0x68,0x68,0x68,0x68,0x67,0x77,0x78,0x68,0x68,0x78,
	0x59,0x58,0x76,0x77,0x68,0x78,0x68,0x59,0x69,0x58,0x68,0x68,0x67,0x78,0x77,0x78,0x69,0x58,0x68,0x57,
	0x78,0x67,0x78,0x76,0x88,0x58,0x67,0x7A,0x46,0x88,0x77,0x78,0x68,0x68,0x66,0x78,0x78,0x68,0x68,0x59,
	0x68,0x69,0x68,0x59,0x67,0x78,0x59,0x58,0x69,0x59,0x67,0x68,0x67,0x69,0x69,0x57,0x79,0x68,0x59,0x59,
	0x59,0x68,0x68,0x68,0x58,0x78,0x67,0x59,0x68,0x78,0x59,0x58,0x78,0x58,0x76,0x78,0x68,0x68,0x68,0x69,
	0x59,0x67,0x68,0x69,0x59,0x59,0x58,0x69,0x59,0x59,0x58,0x5A,0x58,0x68,0x68,0x59,0x58,0x68,0x66,0x47,
	0x88,0x77,0x87,0x77,0x87,0x76,0x87,0x87,0x87,0x77,0x77,0x87,0x67,0x96,0x78,0x76,0x87,0x68,0x77,0x77,
	0x76,0x86,0x96,0x86,0x88,0x77,0x85,0x86,0x8B,0x76,0x0A,0xF9,0x07,0x38,0x57,0x67,0x77,0x78,0x77,0x91,
	0x77,0xD7,0x77,0x7A,0x67,0x3C,0x68,0x68,0x77,0x68,0x78,0x59,0x77,0x68,0x77,0x68,0x76,0x77,0x69,0x68,
	0x68,0x68,0x68,0x67,0x68,0x68,0x77,0x87,0x77,0x67,0x78,0x68,0x67,0x58,0x78,0x68,0x77,0x68,0x78,0x67,
	0x68,0x68,0x67,0x78,0x77,0x77,0x87,0x77,0x76,0x67,0x86,0x85,0x87,0x86,0x97,0x58,0x67,0x79,0x57,0x77,
	0x87,0x77,0x87,0x77,0x76,0x59,0x78,0x77,0x77,0x68,0x77,0x77,0x76,0x78,0x77,0x77,0x77,0x76,0x87,0x77,
	0x77,0x68,0x77,0x77,0x77,0x67,0x78,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x68,0x77,0x76,0x68,0x87,0x77,
	0x77,0x77,0x77,0x68,0x77,0x68,0x77,0x77,0x77,0x77,0x77,0x77,0x76,0x78,0x77,0x77,0x76,0x87,0x77,0x77,
	0x67,0x78,0x77,0x77,0x76,0x78,0x67,0x68,0x68,0x29,0x77,0x88,0x78,0x78,0x77,0x68,0x77,0x77,0x77,0x77,
	0x77,0x77,0x77,0x77,0x4A,0x77,0x4A,0x74,0x77,0x77,0x68,0xA4,0x7A,0x47,0x76,0x86,0x78,0x76,0x7A,0x4A,
	0x83,0xB2,0x87,0x77,0x87,0x76,0x96,0x86,0x96,0x76,0x78,0x87,0x77,0x85,0x87,0x85,0x96,0x65,0xB5,0x95,
	0x96,0x77,0x77,0x86,0x76,0x86,0x86,0x87,0x86,0x86,0x76,0x96,0x96,0x57,0x77,0x85,0x97,0x85,0x86,0xA5,
	0x86,0x85,0x87,0x77,0x68,0x78,0x77,0x95,0x86,0x75,0x87,0x76,0x86,0x79,0x68,0x84,0x96,0x76,0xB3,0x87,
	0x77,0x68,0x86,0xA5,0x77,0x56,0xB6,0x68,0x85,0x93,0xB6,0x95,0x95,0x85,0x95,0xA5,0x95,0x95,0x69,0x85,
	0x95,0x85,0x86,0x86,0x97,0x84,0x85,0xB6,0x84,0xA5,0x95,0xA4,0x95,0x95,0x95,0x68,0x95,0x66,0xA6,0x95,
	0x95,0x95,0x86,0x93,0xB5,0x86,0x77,0x94,0x96,0x95,0x96,0x85,0x68,0x94,0x87,0x95,0x86,0x86,0x93,0xB4,
	0xA3,0xB3,0xA6,0x86,0x85,0x85,0x96,0x76,0x86,0x64,0x69,0x78,0x68,0x78,0x78,0x77,0x67,0x79,0x68,0x79,
	0x59,0x56,0x87,0x98,0x68,0x78,0x76,0x88,0x68,0x68,0x67,0x76,0x87,0x68,0x78,0x76,0x78,0x77,0x78,0xA6,
	0x80,0xAF,0x81,0x38,0x47,0x67,0x77,0x78,0x77,0x89,0x07,0x79,0xB7,0x87,0x86,0x86,0x87,0x86,0x87,0x76,
	0x78,0x77,0x87,0x66,0x96,0x86,0x86,0x74,0xA6,0x87,0x86,0x77,0x86,0x77,0x76,0x77,0x77,0x87,0x77,0x77,
	0x77,0x77,0x87,0x65,0x78,0x77,0x78,0x75,0x88,0x85,0x76,0x87,0x95,0x77,0x86,0x87,0x86,0x96,0x85,0x76,
	0x69,0x67,0x59,0x77,0x6A,0x65,0x86,0x78,0x94,0x77,0x88,0x77,0x78,0x85,0x96,0x65,0x98,0x77,0x87,0x67,
	0x86,0x77,0x87,0x66,0x87,0x86,0x86,0x86,0x77,0x86,0x86,0x76,0x87,0x86,0x77,0x76,0x87,0x77,0x86,0x86,
	0x86,0x87,0x76,0x95,0x86,0x86,0x87,0x65,0x97,0x86,0x87,0x76,0x86,0x86,0x87,0x75,0x88,0x76,0x87,0x76,
	0x87,0x76,0x77,0x77,0x86,0x78,0x76,0x76,0x96,0x78,0x76,0x77,0x86,0x77,0x77,0x76,0x96,0x75,0x95,0x56,
	0x87,0x87,0x87,0x78,0x88,0x67,0x87,0x87,0x58,0x87,0x77,0x87,0x77,0x76,0x87,0x96,0x59,0x88,0x37,0x89,
	0x69,0x69,0x84,0x96,0x67,0x77,0x57,0x4B,0x58,0xB7,0x80,0x8E,0x0D,0x78,0x87,0x77,0x87,0x68,0x79,0x49,
	0x76,0x78,0x77,0x5A,0x67,0x69,0x68,0x68,0x68,0x4A,0x68,0x69,0x67,0x69,0x59,0x58,0x68,0x67,0x69,0x77,
	0x77,0x69,0x68,0x68,0x66,0x68,0x87,0x68,0x77,0x5A,0x68,0x67,0x68,0x68,0x67,0x78,0x78,0x67,0x6A,0x59,
	0x67,0x57,0x95,0x78,0x77,0x86,0x88,0x57,0x77,0x68,0x67,0x79,0x76,0x76,0x98,0x68,0x75,0x68,0x88,0x58,
	0x87,0x5A,0x57,0x79,0x67,0x59,0x78,0x49,0x58,0x77,0x79,0x49,0x68,0x59,0x77,0x68,0x78,0x48,0x79,0x67,
	0x68,0x59,0x68,0x68,0x59,0x75,0x6A,0x68,0x76,0x4C,0x67,0x77,0x78,0x59,0x69,0x56,0x96,0x68,0x68,0x68,
	0x77,0x69,0x67,0x68,0x67,0x78,0x69,0x68,0x58,0x59,0x68,0x68,0x69,0x49,0x77,0x59,0x67,0x69,0x67,0x68,
	0x65,0x48,0x77,0x87,0x86,0x96,0x88,0x75,0x87,0x96,0x87,0x95,0x87,0x77,0x68,0x86,0x77,0x77,0x96,0x68,
	0x86,0x77,0x85,0x5A,0x81,0xD5,0x95,0x68,0x99,0x74,0x98,0x77,0x09,0xF9,0x0A,0x5A,0x66,0x58,0x77,0x87,
	0x91,0x77,0x77,0xE9,0x77,0x77,0x77,0x76,0x87,0x75,0x97,0x77,0x77,0x77,0x78,0x68,0x68,0x68,0x67,0x3B,
	0x59,0x77,0x77,0x57,0x79,0x57,0x86,0x87,0x67,0x97,0x77,0x57,0x79,0x77,0x77,0x75,0x95,0x77,0x79,0x75,
	0x97,0x57,0x77,0x79,0x58,0x69,0x77,0x77,0x77,0x77,0x77,0x75,0x86,0x77,0x87,0x58,0x95,0x78,0x65,0x8A,
	0x39,0x58,0x87,0x96,0x87,0x77,0x77,0x77,0x86,0x87,0x76,0x78,0x77,0x77,0x77,0x68,0x77,0x77,0x77,0x77,
	0x77,0x68,0x77,0x68,0x77,0x67,0x86,0x77,0x78,0x77,0x77,0x77,0x77,0x77,0x68,0x77,0x77,0x77,0x77,0x68,
	0x77,0x68,0x77,0x67,0x78,0x77,0x77,0x68,0x68,0x76,0x87,0x68,0x77,0x77,0x77,0x68,0x77,0x77,0x77,0x77,
	0x77,0x77,0x77,0x68,0x77,0x77,0x77,0x68,0x68,0x68,0x76,0x38,0x97,0x67,0x79,0x77,0x77,0x77,0x77,0x77,
	0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x78,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x68,
	0x72,0xC5,0x86,0x86,0x98,0x77,0x86,0x78,0x1C,0x85,0x2E,0x77,0x77,0x77,0x87,0x86,0x76,0x86,0x86,0xA0,
	0xBD,0x49,0x97,0x66,0x48,0x88,0x48,0x68,0x86,0x78,0x77,0x77,0x78,0x66,0xA6,0x87,0x83,0x85,0x88,0x78,
	0x66,0xA7,0x56,0x87,0x6A,0x46,0x89,0x76,0xA7,0x76,0x87,0x74,0xA2,0x86,0x77,0x79,0x66,0xB6,0x48,0x67,
	0x8A,0x36,0x88,0x77,0xA5,0xA5,0xB1,0xE9,0x39,0x78,0x78,0x75,0x87,0x77,0x77,0x77,0x68,0x58,0x79,0x69,
	0x4A,0x59,0x29,0x6A,0x3C,0x3B,0x46,0x78,0x75,0x89,0x76,0x89,0x4A,0x56,0x88,0x3B,0x66,0x88,0x68,0x87,
	0x57,0x97,0x38,0x87,0x56,0xB7,0x84,0x88,0x67,0x57,0x95,0xA8,0x59,0x77,0x68,0x4A,0x49,0x69,0x57,0x6A,
	0x59,0x58,0x67,0x87,0x5A,0x75,0x78,0x69,0x56,0x97,0x77,0x73,0x08,0x78,0x78,0x77,0x87,0x78,0x77,0x78,
	0x77,0x77,0x87,0x78,0x68,0x77,0x77,0x87,0x78,0x76,0x86,0x97,0x58,0x77,0x78,0x58,0x78,0x77,0x68,0x78,
	0x75,0x95,0xB7,0x70,0x8F,0x80,0xA6,0x87,0x65,0x66,0x78,0x7A,0x17,0x77,0x70,
};

static void SIT13_Func1(struct SIT13Data *s, struct SIT13Buffer *buf, uint32 info, uint16 bits, uint16 num)
{
	uint32 i, j;

	if(bits <= 12)
	{
		for(i = 0; i < (1<<12); i += (1<<bits))
		{
			buf[info+i].data = num;
			buf[info+i].bits = bits;
		}
	}
	else
	{
		j = bits-12;

		if(buf[info & 0xFFF].bits != 0x1F)
		{
			buf[info & 0xFFF].bits = 0x1F;
			buf[info & 0xFFF].data = s->MaxBits++;
		}
		bits = buf[info & 0xFFF].data;
		info >>= 12;

		while(j--)
		{
			uint16 *a;

			a = info & 1 ? &s->Buffer4[bits].d2 : &s->Buffer4[bits].d1;
			if(!*a)
				*a = s->MaxBits++;
			bits = *a;
			info >>= 1;
		}
		s->Buffer4[bits].freq = num;
	}
}

static void SIT13_SortTree(struct SIT13Data *s, struct SIT13Buffer *buf, struct SIT13Buffer *buf2)
{
	uint16 td;
	int8 tb;

	struct SIT13Buffer *a, *b;

	while(buf2-1 > buf)
	{
		a = buf;
		b = buf2;

		for(;;)
		{
			while(++a < buf2)
			{
				tb = a->bits - buf->bits;
				if(tb > 0 || (!tb && (a->data >= buf->data)))
					break;
			}
			while(--b > buf)
			{
				tb = b->bits - buf->bits;
				if(tb < 0 || (!tb && (b->data <= buf->data)))
					break;
			}
			if(b < a)
				break;
			else
			{
				tb = a->bits;
				td = a->data;
				a->bits = b->bits;
				a->data = b->data;
				b->bits = tb;
				b->data = td;
			}
		}
		if(b == buf)
			++buf;
		else
		{
			tb = buf->bits;
			td = buf->data;
			buf->bits = b->bits;
			buf->data = b->data;
			b->bits = tb;
			b->data = td;
			if(buf2-b-1 > b-buf)
			{
				SIT13_SortTree(s, buf, b);
				buf = b+1;
			}
			else
			{
				SIT13_SortTree(s, b+1, buf2);
				buf2 = b;
			}
		}
	}
}

static void SIT13_Func2(struct SIT13Data *s, struct SIT13Buffer *buf, uint16 bits, struct SIT13Buffer *buf2)
{
	int32 i, j, k, l, m, n;

	SIT13_SortTree(s, buf2, buf2 + bits);

	l = k = j = 0;
	for(i = 0; i < bits; ++i)
	{
		l += k;
		m = buf2[i].bits;
		if(m != j)
		{
			if((j = m) == -1)
				k = 0;
			else
				k = 1 << (32-j);
		}
		if(j > 0)
		{
			for(n = m = 0; n < 8*4; n += 4)
				m += SIT13Bits[(l>>n)&0xF]<<(7*4-n);
			SIT13_Func1(s, buf, m, j, buf2[i].data);
		}
	}
}

static void SIT13_CreateStaticTree(struct SIT13Data *s, struct SIT13Buffer *buf, uint16 bits, uint8 *bitsbuf)
{
	uint32 i;

	for(i = 0; i < bits; ++i)
	{
		s->Buffer5[i].data = i;
		s->Buffer5[i].bits = bitsbuf[i];
	}
	SIT13_Func2(s, buf, bits, s->Buffer5);
}

static void SIT13InitInfo(struct SIT13Data *s, uint8 id)
{
	int32 i;
	uint8 k, l = 0, *a;
	const uint8 *b;

	a = s->TextBuf;
	b = (const uint8 *) SIT13Static+SIT13StaticPos[id-1];
	id &= 1;

	for(i = 658; i; --i)
	{
		k = id ? *b >> 4 : *(b++) & 0xF; id ^=1;

		if(!k)
		{
			l -= id ? *b >> 4 : *(b++) & 0xF; id ^= 1;
		}
		else
		{
			if(k == 15)
			{
				l += id ? *b >> 4 : *(b++) & 0xF; id ^= 1;
			}
			else
				l += k-7;
		}
		*(a++) = l;
	}
}

static bool SIT13_Extract(struct SIT13Data *s, Common::BitStream8LSB *bits, Common::MemoryWriteStream& out)
{
	uint32 wpos = 0, j, k, l, size;
	struct SIT13Buffer *buf = s->Buffer3;

	while(!bits->eos())
	{
		k = bits->peekBits<12>();
		if((j = buf[k].bits) <= 12)
		{
			l = buf[k].data;
			if (j == 0)
				return false;
			bits->getBits(j);
		}
		else
		{
			bits->getBits<12>();

			j = buf[k].data;
			while(s->Buffer4[j].freq == -1)
				j = bits->getBit() ? s->Buffer4[j].d2 : s->Buffer4[j].d1;
			l = s->Buffer4[j].freq;
		}
		if(l < 0x100)
		{
			s->Window[wpos++] = l;
			out.writeByte(l);
			wpos &= 0xFFFF;
			buf = s->Buffer3;
		}
		else
		{
			buf = s->Buffer3b;
			if(l < 0x13E)
				size = l - 0x100 + 3;
			else
			{
				if(l == 0x13E)
					size = bits->getBits<10>();
				else
				{
					if(l == 0x140)
						return true;
					size = bits->getBits<15>();
				}
				size += 65;
			}
			j = bits->peekBits<12>();
			k = s->Buffer2[j].bits;
			if(k <= 12)
			{
				l = s->Buffer2[j].data;
				bits->getBits(k);
			}
			else
			{
				bits->getBits<12>();
				j = s->Buffer2[j].data;
				while(s->Buffer4[j].freq == -1)
					j = bits->getBit() ? s->Buffer4[j].d2 : s->Buffer4[j].d1;
				l = s->Buffer4[j].freq;
			}
			k = 0;
			if(l--)
				k = (1 << l) | bits->getBits(l);
			l = wpos+0x10000-(k+1);
			while(size--)
			{
				l &= 0xFFFF;
				byte b = s->Window[l++];
				out.writeByte(b);
				s->Window[wpos++] = b;
				wpos &= 0xFFFF;
			}
		} /* l >= 0x100 */
	}

	return false;
}

static void SIT13_CreateTree(struct SIT13Data *s, Common::BitStream8LSB *bits, struct SIT13Buffer *buf, uint16 num)
{
	struct SIT13Buffer *b;
	uint32 i;
	uint16 data;
	int8 bi = 0;

	for(i = 0; i < num; ++i)
	{
		b = &s->Buffer1[bits->peekBits<12>()];
		data = b->data;
		bits->getBits(b->bits);

		switch(data-0x1F)
		{
		case 0: bi = -1; break;
		case 1: ++bi; break;
		case 2: --bi; break;
		case 3:
			if(bits->getBit())
				s->Buffer5[i++].bits = bi;
			break;
		case 4:
			data = bits->getBits<3>()+2;
			while(data--)
				s->Buffer5[i++].bits = bi;
			break;
		case 5:
			data = bits->getBits<6>()+10;
			while(data--)
				s->Buffer5[i++].bits = bi;
			break;
		default: bi = data+1; break;
		}
		s->Buffer5[i].bits = bi;
	}
	for(i = 0; i < num; ++i)
		s->Buffer5[i].data = i;
	SIT13_Func2(s, buf, num, s->Buffer5);
}

bool StuffItArchive::decompress13(Common::SeekableReadStream *src, byte *dst, uint32 uncompressedSize) const {
	Common::MemoryWriteStream out(dst, uncompressedSize);

	Common::BitStream8LSB bits(src);

	uint32 i, j;

	SIT13Data *s = new SIT13Data();

	s->MaxBits = 1;
	for(i = 0; i < 37; ++i)
		SIT13_Func1(s, s->Buffer1, SIT13Info[i], SIT13InfoBits[i], i);
	for(i = 1; i < 0x704; ++i)
	{
		/* s->Buffer4[i].d1 = s->Buffer4[i].d2 = 0; */
		s->Buffer4[i].freq = -1;
	}

	j = bits.getBits<8>();
	i = j>>4;
	if(i > 5)
		return false;
	if(i)
	{
		SIT13InitInfo(s, i--);
		SIT13_CreateStaticTree(s, s->Buffer3, 0x141, s->TextBuf);
		SIT13_CreateStaticTree(s, s->Buffer3b, 0x141, s->TextBuf+0x141);
		SIT13_CreateStaticTree(s, s->Buffer2, SIT13StaticBits[i], s->TextBuf+0x282);
	}
	else
	{
		SIT13_CreateTree(s, &bits, s->Buffer3, 0x141);
		if(j&8)
			memcpy(s->Buffer3b, s->Buffer3, 0x1000*sizeof(struct SIT13Buffer));
		else
			SIT13_CreateTree(s, &bits, s->Buffer3b, 0x141);
		j = (j&7)+10;
		SIT13_CreateTree(s, &bits, s->Buffer2, j);
	}
	return SIT13_Extract(s, &bits, out);
}

#define OUTPUT_VAL(x) \
	out.writeByte(x); \
	dat->window[j++] = x; \
	j &= 0x3FFFF

void StuffItArchive::decompress14(Common::SeekableReadStream *src, byte *dst, uint32 uncompressedSize) const {
	Common::MemoryWriteStream out(dst, uncompressedSize);

	Common::BitStream8LSB *bits = new Common::BitStream8LSB(src);

	uint32 i, j, k, l, m, n;

	SIT14Data *dat = new SIT14Data();

	// initialization
	for (i = k = 0; i < 52; ++i) {
		dat->var2[i] = k;
		k += (1 << (dat->var1[i] = ((i >= 4) ? ((i - 4) >> 2) : 0)));
	}

	for (i = 0; i < 4; ++i)
		dat->var8[i] = i;

	for (m = 1, l = 4; i < 0x4000; m <<= 1) // i is 4
		for (n = l + 4; l < n; ++l)
			for (j = 0; j < m; ++j)
				dat->var8[i++] = l;

	for (i = 0, k = 1; i < 75; ++i) {
		dat->var5[i] = k;
		k += (1 << (dat->var4[i] = (i >= 3 ? ((i - 3) >> 2) : 0)));
	}

	for (i = 0; i < 4; ++i)
		dat->var6[i] = i - 1;

	for (m = 1, l = 3; i < 0x400; m <<= 1) // i is 4
		for (n = l + 4; l < n; ++l)
			for (j = 0; j < m; ++j)
				dat->var6[i++] = l;

	m = bits->getBits<16>(); // number of blocks
	j = 0; // window position

	while (m-- && !bits->eos()) {
		bits->getBits<16>(); // skip crunched block size
		bits->getBits<16>();
		n = bits->getBits<16>(); // number of uncrunched bytes
		n |= bits->getBits<16>() << 16;
		readTree14(bits, dat, 308, dat->var7);
		readTree14(bits, dat, 75, dat->var3);

		while (n && !bits->eos()) {
			for (i = 0; i < 616;)
				i = dat->var7[i + bits->getBit()];

			i -= 616;

			if (i < 0x100) {
				OUTPUT_VAL(i);
				--n;
			} else {
				i -= 0x100;
				k = dat->var2[i] + 4;
				i = dat->var1[i];

				if (i)
					k += bits->getBits(i);

				for (i = 0; i < 150;)
					i = dat->var3[i + bits->getBit()];

				i -= 150;
				l = dat->var5[i];
				i = dat->var4[i];

				if (i)
					l += bits->getBits(i);

				n -= k;
				l = j + 0x40000 - l;

				while (k--) {
					l &= 0x3FFFF;
					OUTPUT_VAL(dat->window[l]);
					l++;
				}
			}
		}

		ALIGN_BITS(bits);
	}

	delete dat;
	delete bits;
}

#undef OUTPUT_VAL
#undef ALIGN_BITS


StuffItArchive::FileEntryFork::FileEntryFork() : uncompressedSize(0), compressedSize(0), offset(0), crc(0), compression(0) {
}

StuffItArchive::StuffItArchiveMember::StuffItArchiveMember(const Common::Path &path, const Common::Archive &archive)
	: Common::GenericArchiveMember(path, archive) {
}

bool StuffItArchive::StuffItArchiveMember::isInMacArchive() const {
	return true;
}

Common::Archive *createStuffItArchive(const Common::Path &fileName, bool flattenTree) {
	StuffItArchive *archive = new StuffItArchive();

	if (!archive->open(fileName, flattenTree)) {
		delete archive;
		return nullptr;
	}

	return archive;
}

Common::Archive *createStuffItArchive(Common::SeekableReadStream *stream, bool flattenTree) {
	StuffItArchive *archive = new StuffItArchive();

	if (!archive->open(stream, flattenTree)) {
		delete archive;
		return nullptr;
	}

	return archive;
}

} // End of namespace Common
