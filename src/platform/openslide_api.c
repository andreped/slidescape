/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2022  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "platform.h"
#include "stringutils.h"

#define OPENSLIDE_API_IMPL
#include "openslide_api.h"

#ifdef _WIN32
#include <windows.h>
#include "win32_platform.h" // for win32_diagnostic()
#else
#include <dlfcn.h>
#endif

bool init_openslide() {
	i64 debug_start = get_clock();

#ifdef _WIN32
	// Look for DLLs in an openslide/ folder in the same location as the .exe
	char dll_path[4096];
	GetModuleFileNameA(NULL, dll_path, sizeof(dll_path));
	char* pos = (char*)one_past_last_slash(dll_path, sizeof(dll_path));
	i32 chars_left = sizeof(dll_path) - (pos - dll_path);
	strncpy(pos, "openslide", chars_left);
	SetDllDirectoryA(dll_path);

	HINSTANCE library_handle = LoadLibraryA("libopenslide-0.dll");
	if (!library_handle) {
		// If DLL not found in the openslide/ folder, look one folder up (in the same location as the .exe)
		*pos = '\0';
		SetDllDirectoryA(dll_path);
		library_handle = LoadLibraryA("libopenslide-0.dll");
	}
	SetDllDirectoryA(NULL);

#elif defined(__APPLE__)
	void* library_handle = dlopen("libopenslide.dylib", RTLD_LAZY);
	if (!library_handle) {
		// Check expected library path for MacPorts
		library_handle = dlopen("/opt/local/lib/libopenslide.dylib", RTLD_LAZY);
		if (!library_handle) {
			// Check expected library path for Homebrew
			library_handle = dlopen("/usr/local/opt/openslide/lib/libopenslide.dylib", RTLD_LAZY);
		}
	}
#else
	void* library_handle = dlopen("libopenslide.so", RTLD_LAZY);
	if (!library_handle) {
		library_handle = dlopen("/usr/local/lib/libopenslide.so", RTLD_LAZY);
	}
#endif

	if (library_handle) {

#ifdef _WIN32
#define GET_PROC(proc) if (!(openslide.proc = (void*) GetProcAddress(library_handle, #proc))) goto failed;
#else
#define GET_PROC(proc) if (!(openslide.proc = (void*) dlsym(library_handle, #proc))) goto failed;
#endif
		GET_PROC(openslide_detect_vendor);
		GET_PROC(openslide_open);
		GET_PROC(openslide_get_level_count);
		GET_PROC(openslide_get_level0_dimensions);
		GET_PROC(openslide_get_level_dimensions);
		GET_PROC(openslide_get_level_downsample);
		GET_PROC(openslide_get_best_level_for_downsample);
		GET_PROC(openslide_read_region);
		GET_PROC(openslide_close);
		GET_PROC(openslide_get_error);
		GET_PROC(openslide_get_property_names);
		GET_PROC(openslide_get_property_value);
		GET_PROC(openslide_get_associated_image_names);
		GET_PROC(openslide_get_associated_image_dimensions);
		GET_PROC(openslide_read_associated_image);
		GET_PROC(openslide_get_version);
#undef GET_PROC

		float seconds = get_seconds_elapsed(debug_start, get_clock());
		if (seconds > 0.1f) {
			console_print("OpenSlide initialized (loading took %g seconds)\n", seconds);
		} else {
			console_print("OpenSlide initialized\n");
		}
		is_openslide_available = true;

	} else failed: {
#ifdef _WIN32
		//win32_diagnostic("LoadLibraryA");
		console_print("OpenSlide not available: could not load libopenslide-0.dll\n");
#elif defined(__APPLE__)
		console_print("OpenSlide not available: could not load libopenslide.dylib (not installed?)\n");
#else
		console_print("OpenSlide not available: could not load libopenslide.so (not installed?)\n");
#endif
		is_openslide_available = false;
	}
	is_openslide_loading_done = true;
	return is_openslide_available;
}
