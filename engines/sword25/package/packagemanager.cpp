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

/*
 * This code is based on Broken Sword 2.5 engine
 *
 * Copyright (c) Malte Thiesen, Daniel Queteschiner and Michael Elsdoerfer
 *
 * Licensed under GNU GPL v2
 *
 */

#include "common/archive.h"
#include "common/config-manager.h"
#include "common/savefile.h"
#include "common/str-array.h"
#include "common/system.h"
#include "common/compression/unzip.h"
#include "sword25/sword25.h"	// for kDebugScript
#include "sword25/kernel/filesystemutil.h"
#include "sword25/package/packagemanager.h"

namespace Sword25 {

const char PATH_SEPARATOR = '/';

static Common::String normalizePath(const Common::String &path, const Common::String &currentDirectory) {
	Common::String wholePath = (path.size() >= 1 && path[0] == PATH_SEPARATOR) ? path : currentDirectory + PATH_SEPARATOR + path;

	if (wholePath.size() == 0) {
		// The path list has no elements, therefore the root directory is returned
		return Common::String(PATH_SEPARATOR);
	}

	return Common::normalizePath(wholePath, PATH_SEPARATOR);
}

PackageManager::PackageManager(Kernel *pKernel) : Service(pKernel),
	_currentDirectory(PATH_SEPARATOR),
	_rootFolder(ConfMan.getPath("path")),
	_useEnglishSpeech(ConfMan.getBool("english_speech")),
	_extractedFiles(false) {
	if (!registerScriptBindings())
		error("Script bindings could not be registered.");
	else
		debugC(kDebugScript, "Script bindings registered.");
}

PackageManager::~PackageManager() {
	// Free the package list
	Common::List<ArchiveEntry *>::iterator i;
	for (i = _archiveList.begin(); i != _archiveList.end(); ++i)
		delete *i;

}

Common::String PackageManager::ensureSpeechLang(const Common::String &fileName) {
	if (!_useEnglishSpeech || fileName.size() < 9 || !fileName.hasPrefix("/speech/"))
		return fileName;

	// Always keep German speech as a fallback in case the English speech pack is not present.
	// However this means we cannot play with German text and English voice.
	if (fileName.hasPrefix("/speech/de"))
		return fileName;

	Common::String newFileName = "/speech/en";
	uint fileIdx = 9;
	while (fileIdx < fileName.size() && fileName[fileIdx] != '/')
		++fileIdx;
	if (fileIdx < fileName.size())
		newFileName += fileName.c_str() + fileIdx;

	return newFileName;
}

/**
 * Scans through the archive list for a specified file
 */
Common::ArchiveMemberPtr PackageManager::getArchiveMember(const Common::String &fileName) {
	Common::String fileName2 = ensureSpeechLang(fileName);
	// Loop through checking each archive
	Common::List<ArchiveEntry *>::iterator i;
	for (i = _archiveList.begin(); i != _archiveList.end(); ++i) {
		if (!fileName2.hasPrefix((*i)->_mountPath)) {
			// The mount path is in different subtree. Skipping
			continue;
		}

		// Look into the archive for the desired file
		Common::Archive *archiveFolder = (*i)->archive;

		// Construct relative path
		Common::Path resPath(&fileName2.c_str()[(*i)->_mountPath.size()]);

		if (archiveFolder->hasFile(resPath)) {
			return archiveFolder->getMember(resPath);
		}
	}

	return Common::ArchiveMemberPtr();
}

bool PackageManager::loadPackage(const Common::Path &fileName, const Common::String &mountPosition) {
	debug(3, "loadPackage(%s, %s)", fileName.toString(Common::Path::kNativeSeparator).c_str(), mountPosition.c_str());

	Common::Archive *zipFile = Common::makeZipArchive(fileName);
	if (zipFile == NULL) {
		error("Unable to mount file \"%s\" to \"%s\"", fileName.toString(Common::Path::kNativeSeparator).c_str(), mountPosition.c_str());
		return false;
	} else {
		debugC(kDebugResource, "Package '%s' mounted as '%s'.", fileName.toString(Common::Path::kNativeSeparator).c_str(), mountPosition.c_str());
		Common::ArchiveMemberList files;
		zipFile->listMembers(files);
		debug(3, "Capacity %d", files.size());

		for (Common::ArchiveMemberList::iterator it = files.begin(); it != files.end(); ++it)
			debug(3, "%s", (*it)->getName().c_str());

		_archiveList.push_front(new ArchiveEntry(zipFile, mountPosition));

		return true;
	}
}

bool PackageManager::loadDirectoryAsPackage(const Common::Path &directoryName, const Common::String &mountPosition) {
	Common::FSNode directory(directoryName);
	Common::Archive *folderArchive = new Common::FSDirectory(directory, 6, false, false, true);
	if (!directory.exists() || (folderArchive == NULL)) {
		error("Unable to mount directory \"%s\" to \"%s\".", directoryName.toString(Common::Path::kNativeSeparator).c_str(), mountPosition.c_str());
		return false;
	} else {
		debugC(kDebugResource, "Directory '%s' mounted as '%s'.", directoryName.toString(Common::Path::kNativeSeparator).c_str(), mountPosition.c_str());

		Common::ArchiveMemberList files;
		folderArchive->listMembers(files);
		debug(0, "Capacity %d", files.size());

		_archiveList.push_front(new ArchiveEntry(folderArchive, mountPosition));

		return true;
	}
}

byte *PackageManager::getFile(const Common::String &fileName, uint *fileSizePtr) {
	const Common::String B25S_EXTENSION(".b25s");
	Common::SeekableReadStream *in;

	if (fileName.hasSuffix(B25S_EXTENSION)) {
		// Savegame loading logic
		Common::SaveFileManager *sfm = g_system->getSavefileManager();
		Common::InSaveFile *file = sfm->openForLoading(
			FileSystemUtil::getPathFilename(fileName));
		if (!file) {
			error("Could not load savegame \"%s\".", fileName.c_str());
			return 0;
		}

		if (fileSizePtr)
			*fileSizePtr = file->size();

		byte *buffer = new byte[file->size()];
		file->read(buffer, file->size());

		delete file;
		return buffer;
	}

	Common::ArchiveMemberPtr fileNode = getArchiveMember(normalizePath(fileName, _currentDirectory));
	if (!fileNode)
		return 0;
	if (!(in = fileNode->createReadStream()))
		return 0;

	// If the filesize is desired, then output the size
	if (fileSizePtr)
		*fileSizePtr = in->size();

	// Read the file
	byte *buffer = new byte[in->size()];
	int bytesRead = in->read(buffer, in->size());
	delete in;

	// Modify the buffer to enable internal debugger if needed
	if (debugChannelSet(-1, kDebugInternalDebugger) && fileName.equals("/system/internal_config.lua")) {
		char *found = strstr((char *)buffer, "ENGINE_RELEASE_TYPE = 'pub'");
		if (found != nullptr) {
			memcpy(found + 23, "dev", 3);
		}
	}

	// Modify the buffer to properly set the death screen as background 
	// by changing its z value
    if (fileName.equals("rooms/tod/scripts/default.lua")) {
        char *found = strstr((char *)buffer, "self:AddOccluder('/rooms/tod/gfx/rip.png', { X = 0, Y = 80 }, 10)");
        if (found != nullptr) {
            memcpy(found + 62, " 8", 2);
        }
    }

	if (!bytesRead) {
		delete[] buffer;
		return NULL;
	}

	return buffer;
}

Common::SeekableReadStream *PackageManager::getStream(const Common::String &fileName) {
	Common::SeekableReadStream *in;
	Common::ArchiveMemberPtr fileNode = getArchiveMember(normalizePath(fileName, _currentDirectory));
	if (!fileNode)
		return 0;
	if (!(in = fileNode->createReadStream()))
		return 0;

	return in;
}

bool PackageManager::changeDirectory(const Common::String &directory) {
	// Get the path elements for the file
	_currentDirectory = normalizePath(directory, _currentDirectory);
	return true;
}

Common::String PackageManager::getAbsolutePath(const Common::String &fileName) {
	return normalizePath(ensureSpeechLang(fileName), _currentDirectory);
}

bool PackageManager::fileExists(const Common::String &fileName) {
	// FIXME: The current Zip implementation doesn't support getting a folder entry, which is needed for detecting
	// the English voice pack
	Common::String fileName2 = ensureSpeechLang(fileName);
	if (fileName2 == "/speech/en") {
		// To get around this, change to detecting one of the files in the folder
		bool exists = getArchiveMember(normalizePath(fileName2 + "/APO0001.ogg", _currentDirectory));
		if (!exists && _useEnglishSpeech) {
			_useEnglishSpeech = false;
			warning("English speech not found");
		}
		return exists;
	}

	Common::ArchiveMemberPtr fileNode = getArchiveMember(normalizePath(fileName2, _currentDirectory));
	return fileNode;
}

int PackageManager::doSearch(Common::ArchiveMemberList &list, const Common::String &filter, const Common::String &path, uint typeFilter) {
	Common::String normalizedFilter = normalizePath(ensureSpeechLang(filter), _currentDirectory);
	int num = 0;

	if (path.size() > 0)
		warning("STUB: PackageManager::doSearch(<%s>, <%s>, %d)", filter.c_str(), path.c_str(), typeFilter);

	debug(9, "PackageManager::doSearch(..., \"%s\", \"%s\", %d)", filter.c_str(), path.c_str(), typeFilter);

	// Loop through checking each archive
	Common::List<ArchiveEntry *>::iterator i;
	for (i = _archiveList.begin(); i != _archiveList.end(); ++i) {
		Common::ArchiveMemberList memberList;

		if (!normalizedFilter.hasPrefix((*i)->_mountPath)) {
			// The mount path is in different subtree. Skipping
			continue;
		}

		// Construct relative path
		Common::Path resFilter(&normalizedFilter.c_str()[(*i)->_mountPath.size()]);

		if ((*i)->archive->listMatchingMembers(memberList, resFilter) == 0)
			continue;

		// Create a list of the matching names
		for (Common::ArchiveMemberList::iterator it = memberList.begin(); it != memberList.end(); ++it) {
			Common::Path name = (*it)->getPathInArchive();
			bool isDirectory = (*it)->isDirectory();
			bool matchType = (((typeFilter & PackageManager::FT_DIRECTORY) && isDirectory) ||
							  ((typeFilter & PackageManager::FT_FILE) && !isDirectory));

			if (matchType) {

				// Do not add duplicate files
				bool found = false;
				for (Common::ArchiveMemberList::iterator it1 = list.begin(); it1 != list.end(); ++it1) {
					if ((*it1)->getPathInArchive() == name) {
						found = true;
						break;
					}
				}

				if (!found) {
					list.push_back(Common::ArchiveMemberList::value_type(new Common::GenericArchiveMember(name, *(*i)->archive)));
					debug(9, "> %s", name.toString().c_str());
				}
				num++;
			}
		}
	}

	return num;
}

} // End of namespace Sword25
