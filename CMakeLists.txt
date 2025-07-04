#
# Copyright (C) 2004-2025 ZNC, see the NOTICE file for details.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

cmake_minimum_required(VERSION 3.13)
project(ZNC VERSION 1.11.0 LANGUAGES CXX)
set(ZNC_VERSION 1.11.x)
set(append_git_version true)
set(alpha_version "") # e.g. "-rc1"
set(VERSION_EXTRA "" CACHE STRING
	"Additional string appended to version, e.g. to mark distribution")

set(PROJECT_VERSION "${ZNC_VERSION}")

# https://cmake.org/pipermail/cmake/2010-September/039388.html
set(_all_targets "" CACHE INTERNAL "")
function(znc_add_library name)
	add_library("${name}" ${ARGN})
	set(_all_targets "${_all_targets};${name}" CACHE INTERNAL "")
endfunction()
function(znc_add_executable name)
	add_executable("${name}" ${ARGN})
	set(_all_targets "${_all_targets};${name}" CACHE INTERNAL "")
endfunction()
function(znc_add_custom_target name)
	add_custom_target("${name}" ${ARGN})
	set(_all_targets "${_all_targets};${name}" CACHE INTERNAL "")
endfunction()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(TestCXX17)
if(NOT CYGWIN)
	# We don't want to use -std=gnu++17 instead of -std=c++17, but among other
	# things, -std=c++17 on cygwin defines __STRICT_ANSI__ which makes cygwin
	# not to compile: undefined references to strerror_r, to fdopen, to
	# strcasecmp, etc (their declarations in system headers are between ifdef)
	set(CMAKE_CXX_EXTENSIONS false)
endif()

include(CMakeFindFrameworks_fixed)
include(use_homebrew)
include(GNUInstallDirs)
include(CheckCXXSymbolExists)
include(copy_csocket)
include(CMakePushCheckState)

set(CMAKE_THREAD_PREFER_PTHREAD true)
set(THREADS_PREFER_PTHREAD_FLAG true)
find_package(Threads REQUIRED)
if(NOT CMAKE_USE_PTHREADS_INIT)
	message(FATAL_ERROR "This compiler/OS doesn't seem to support pthreads.")
endif()

include(TestLargeFiles)
test_large_files(HAVE_LARGE_FILES_UNUSED_VAR)
find_package(PkgConfig)

macro(tristate_option opt help)
	set(WANT_${opt} AUTO CACHE STRING ${help})
	set_property(CACHE WANT_${opt} PROPERTY STRINGS AUTO YES NO)
	if(WANT_${opt} STREQUAL "AUTO")
		set(TRISTATE_${opt}_REQUIRED)
	else()
		set(TRISTATE_${opt}_REQUIRED REQUIRED)
	endif()
endmacro()

set(ZNC_CMAKE_FIND_DEPS "")
set(zncpubdeps)

tristate_option(OPENSSL "Support SSL")
if(WANT_OPENSSL)
	find_package(OpenSSL ${TRISTATE_OPENSSL_REQUIRED})

	if(OPENSSL_FOUND)
		# SSL_SESSION was made opaque in OpenSSL 1.1.0;
		# LibreSSL gained that function later too.
		# TODO: maybe remove this check at some point, and stop supporting old
		# libssl versions
		cmake_push_check_state(RESET)
			set(CMAKE_REQUIRED_LIBRARIES ${OPENSSL_LIBRARIES})
			set(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
			check_cxx_symbol_exists(SSL_SESSION_get0_cipher openssl/ssl.h
				HAVE_SSL_SESSION_get0_cipher)
		cmake_pop_check_state()
		set(ZNC_CMAKE_FIND_DEPS
			"${ZNC_CMAKE_FIND_DEPS}\nfind_dependency(OpenSSL)")
		list(APPEND zncpubdeps OpenSSL::SSL)
	endif()
endif()
set(HAVE_LIBSSL "${OPENSSL_FOUND}")

set(WANT_IPV6 true CACHE BOOL "Support IPv6")
set(HAVE_IPV6 "${WANT_IPV6}")

tristate_option(ZLIB "Compress HTTP traffic with Zlib")
if(WANT_ZLIB)
	find_package(ZLIB ${TRISTATE_ZLIB_REQUIRED})
endif()
set(HAVE_ZLIB "${ZLIB_FOUND}")

tristate_option(CYRUS "Support authentication with Cyrus")
if(WANT_CYRUS)
	pkg_check_modules(CYRUS IMPORTED_TARGET libsasl2)
	if(NOT CYRUS_FOUND)
		# libsasl2.pc is missing on 2.1.25 which is on ubuntu 14.04
		# next ubuntu version has 2.1.26 which has libsasl2.pc
		#
		# osx (as of El Capitan) doesn't have it either...
		cmake_push_check_state(RESET)
			set(CMAKE_REQUIRED_LIBRARIES -lsasl2)
			# sys/types.h here is workaround for sasl 2.1.26:
			# https://github.com/znc/znc/issues/1243
			# https://lists.andrew.cmu.edu/pipermail/cyrus-sasl/2012-December/002572.html
			# https://cgit.cyrus.foundation/cyrus-sasl/commit/include/sasl.h?id=2f740223fa1820dd71e6ab0e50d4964760789209
			check_cxx_symbol_exists(sasl_server_init "sys/types.h;sasl/sasl.h"
				CYRUS_HARDCODED)
		cmake_pop_check_state()
		if(CYRUS_HARDCODED)
			add_library(HardcodedCyrusDep INTERFACE)
			add_library(PkgConfig::CYRUS ALIAS HardcodedCyrusDep)
			target_link_libraries(HardcodedCyrusDep INTERFACE sasl2)
			set(CYRUS_FOUND true)
		endif()
	endif()
	if(TRISTATE_CYRUS_REQUIRED AND NOT CYRUS_FOUND)
		message(FATAL_ERROR "Can't find Cyrus SASL 2")
	endif()
endif()

tristate_option(ARGON "Store password hashes using Argon2id instead of SHA-256")
if(WANT_ARGON)
	pkg_check_modules(ARGON ${TRISTATE_ARGON_REQUIRED} IMPORTED_TARGET libargon2)
endif()
set(ZNC_HAVE_ARGON "${ARGON_FOUND}")

tristate_option(ICU "Support character encodings")
if(WANT_ICU)
	pkg_check_modules(ICU ${TRISTATE_ICU_REQUIRED} IMPORTED_TARGET icu-uc)
endif()
set(HAVE_ICU "${ICU_FOUND}")
if(ICU_FOUND)
	set(ZNC_CMAKE_FIND_DEPS "${ZNC_CMAKE_FIND_DEPS}\nfind_dependency_pc(ICU icu-uc)")
	list(APPEND zncpubdeps PkgConfig::ICU)
endif()

set(WANT_PERL false CACHE BOOL "Support Perl modules")
set(WANT_PYTHON false CACHE BOOL "Support Python modules")
set(WANT_PYTHON_VERSION "python3" CACHE STRING
	"Python version to use, e.g. python-3.5, this name is passed to pkg-config")
if(WANT_PYTHON AND NOT ICU_FOUND)
	message(FATAL_ERROR "Modpython requires ZNC to be compiled with charset "
	"support, but ICU library not found")
endif()
tristate_option(SWIG "Use SWIG to generate modperl and modpython")
set(search_swig false)
if(WANT_SWIG AND TRISTATE_SWIG_REQUIRED)
	set(search_swig true)
endif()
if(WANT_PERL AND NOT EXISTS
		"${PROJECT_SOURCE_DIR}/modules/modperl/generated.tar.gz")
	if(WANT_SWIG)
		set(search_swig true)
	else()
		message(FATAL_ERROR "Pregenerated modperl files are not available. "
			"SWIG is required. Alternatively, build ZNC from tarball.")
	endif()
endif()
if(WANT_PYTHON AND NOT EXISTS
		"${PROJECT_SOURCE_DIR}/modules/modpython/generated.tar.gz")
	if(WANT_SWIG)
		set(search_swig true)
	else()
		message(FATAL_ERROR "Pregenerated modpython files are not available. "
			"SWIG is required. Alternatively, build ZNC from tarball.")
	endif()
endif()
if(search_swig)
	find_package(SWIG 4.0.1)
	if(NOT SWIG_FOUND)
		message(FATAL_ERROR
			"Can't find SWIG, therefore Perl and Python aren't supported. "
			"Alternatively, build ZNC from tarball.")
	endif()
endif()

if(WANT_PERL)
	find_package(PerlLibs 5.10 REQUIRED)
endif()
if (WANT_PYTHON)
	set (_MIN_PYTHON_VERSION 3.4)
	find_package(Perl 5.10 REQUIRED)
	# VERSION_GREATER_EQUAL is available only since 3.7
	if (CMAKE_VERSION VERSION_LESS 3.12)
	else()
		# Even if FindPython3 is available (since CMake 3.12) we still use
		# pkg-config, because FindPython has a hardcoded list of python
		# versions, which may become outdated when new pythons are released,
		# but when cmake in the distro is old.
		#
		# Why FindPython3 is useful at all? Because sometimes there is no
		# python3.pc, but only python-3.5.pc and python-3.6.pc; which would
		# force user to provide the version explicitly via
		# WANT_PYTHON_VERSION. This is the case on Gentoo when NOT building
		# via emerge.
		if (WANT_PYTHON_VERSION STREQUAL "python3")
			find_package(Python3 COMPONENTS Development)
		else()
			# We used to pass value like "python-3.5" to the variable.
			if (WANT_PYTHON_VERSION MATCHES "^(python-)?(.*)$")
				find_package(Python3 COMPONENTS Development
					EXACT "${CMAKE_MATCH_2}")
			else()
				message(FATAL_ERROR "Invalid value of WANT_PYTHON_VERSION")
			endif()
		endif()
		if (Python3_FOUND AND Python3_VERSION VERSION_LESS
				${_MIN_PYTHON_VERSION})
			message(STATUS
				"Python too old, need at least ${_MIN_PYTHON_VERSION}")
			set(Python3_FOUND OFF)
		else()
			# Compatibility with pkg-config variables
			set(Python3_LDFLAGS "${Python3_LIBRARIES}")
		endif()
	endif()
	if (NOT Python3_FOUND AND WANT_PYTHON_VERSION MATCHES "^python")
		# Since python 3.8, -embed is required for embedding.
		pkg_check_modules(Python3
			"${WANT_PYTHON_VERSION}-embed >= ${_MIN_PYTHON_VERSION}")
		if (NOT Python3_FOUND)
			pkg_check_modules(Python3
				"${WANT_PYTHON_VERSION} >= ${_MIN_PYTHON_VERSION}")
		endif()
	endif()
	if (NOT Python3_FOUND)
		message(FATAL_ERROR "Python 3 is not found. Try disabling it.")
	endif()
endif()

set(WANT_TCL false CACHE BOOL "Support Tcl modules")
if(WANT_TCL)
	find_package(TCL QUIET)
	if(NOT TCL_FOUND)
		message(FATAL_ERROR "Can't find Tcl")
	endif()
endif()

tristate_option(I18N "Native language support (i18n)")
if(WANT_I18N)
	find_package(Boost ${TRISTATE_I18N_REQUIRED} COMPONENTS locale CONFIG)
	find_package(Gettext ${TRISTATE_I18N_REQUIRED})
endif()
if(Boost_LOCALE_FOUND AND GETTEXT_MSGFMT_EXECUTABLE)
	set(HAVE_I18N true)
else()
	set(HAVE_I18N false)
	message(STATUS "Boost.Locale or gettext (msgfmt) is not found, disabling i18n support")
endif()

if(HAVE_I18N AND GETTEXT_MSGMERGE_EXECUTABLE)
	find_program(XGETTEXT_EXECUTABLE xgettext)
	if(XGETTEXT_EXECUTABLE)
		add_custom_target(translation)
	endif()
endif()

# poll() is broken on Mac OS, it fails with POLLNVAL for pipe()s.
if(APPLE)
	set(CSOCK_USE_POLL false)
else()
	set(CSOCK_USE_POLL true)
endif()

find_package(cctz QUIET)
set(cctz_cc "")
if (cctz_FOUND)
	message(STATUS "Found cctz at ${cctz_DIR}")
else()
	message(STATUS "Will build cctz")
	set(cctz_cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/civil_time_detail.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_fixed.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_format.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_if.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_impl.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_info.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_libc.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_lookup.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/time_zone_posix.cc
		${PROJECT_SOURCE_DIR}/third_party/cctz/src/zone_info_source.cc
	)
	add_library(cctz INTERFACE)
	add_library(cctz::cctz ALIAS cctz)
	target_include_directories(cctz INTERFACE
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/cctz/include>)
	if (APPLE)
		find_library(CoreFoundation CoreFoundation REQUIRED)
		target_link_libraries(cctz INTERFACE ${CoreFoundation})
	endif()
endif()

check_cxx_symbol_exists(getopt_long "getopt.h" HAVE_GETOPT_LONG)
check_cxx_symbol_exists(lstat "sys/types.h;sys/stat.h;unistd.h" HAVE_LSTAT)
check_cxx_symbol_exists(getpassphrase "stdlib.h" HAVE_GETPASSPHRASE)
check_cxx_symbol_exists(tcsetattr "termios.h;unistd.h" HAVE_TCSETATTR)
check_cxx_symbol_exists(clock_gettime "time.h" HAVE_CLOCK_GETTIME)
check_cxx_symbol_exists(gethostname "unistd.h" ZNC_HAVE_GETHOSTNAME)
check_cxx_symbol_exists(uname "sys/utsname.h" ZNC_HAVE_UNAME)

# Note that old broken systems, such as OpenBSD, NetBSD, which don't support
# AI_ADDRCONFIG, also have thread-unsafe getaddrinfo(). Gladly, they fixed
# thread-safety before support of AI_ADDRCONFIG, so this can be abused to
# detect the thread-safe getaddrinfo().
#
# TODO: drop support of blocking DNS at some point. OpenBSD supports
# AI_ADDRCONFIG since Nov 2014, and their getaddrinfo() is thread-safe since
# Nov 2013. NetBSD's one is thread-safe since ages ago.
check_cxx_symbol_exists(AI_ADDRCONFIG "sys/types.h;sys/socket.h;netdb.h"
	HAVE_THREADED_DNS)

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CYGWIN)
	# These enable some debug options in g++'s STL, e.g. invalid use of
	# iterators, but they cause crashes on cygwin while loading modules
	set(_GLIBCXX_DEBUG true)
	set(_GLIBCXX_DEBUG_PEDANTIC true)
endif()

if(append_git_version)
	find_package(Git)
endif()


file(GLOB csocket_files LIST_DIRECTORIES FALSE
	"${PROJECT_SOURCE_DIR}/third_party/Csocket/Csocket.*")
if(csocket_files STREQUAL "")
	execute_process(COMMAND git status
		WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
		RESULT_VARIABLE git_status_var
		OUTPUT_QUIET
		ERROR_QUIET)
	if(git_status_var)
		message(FATAL_ERROR
			" It looks like git submodules are not initialized.\n"
			" Either this is not a git clone, or you don't have git installed.\n"
			" Fetch the tarball from the website: https://znc.in/releases/ ")
	else()
		message(FATAL_ERROR
			" It looks like git submodules are not initialized.\n"
			" Run: git submodule update --init --recursive")
	endif()
endif()

install(DIRECTORY webskins
	DESTINATION "${CMAKE_INSTALL_DATADIR}/znc")
install(DIRECTORY translations
	DESTINATION "${CMAKE_INSTALL_DATADIR}/znc")
install(DIRECTORY man/
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
	FILES_MATCHING PATTERN "znc*")

set(WANT_SYSTEMD false CACHE BOOL "Install znc.service to systemd")
if(WANT_SYSTEMD)
	configure_file("znc.service.in" "znc.service")
	set(SYSTEMD_DIR "" CACHE PATH "Path to systemd units")
	if(SYSTEMD_DIR STREQUAL "" AND PKG_CONFIG_EXECUTABLE)
		execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}"
			--variable=systemdsystemunitdir systemd
			OUTPUT_VARIABLE SYSTEMD_DIR)
	endif()
	if(SYSTEMD_DIR STREQUAL "")
		message(FATAL_ERROR "Systemd is enabled, "
			"but the unit dir can't be found.")
	endif()
	install(FILES "${PROJECT_BINARY_DIR}/znc.service"
		DESTINATION "${SYSTEMD_DIR}")
endif()

# On cygwin, if to link modules against znc.exe directly, modperl can't call
# ZNC's functions very well. They do get called, but global variables have
# different addresses. That address actually is in modperl/ZNC.dll if to look
# at /proc/123/maps
# Example of such global variable is one returned by CZNC::Get()
# Modpython seems to work though with STATIC on cygwin... (I didn't test it
# too much though)
#
# Non-cygwin should link modules to /usr/bin/znc directly to prevent this:
# error while loading shared libraries: libznc.so: cannot open shared object file: No such file or directory
# Without need to touch LD_LIBRARY_PATH
if(CYGWIN)
	set(znc_link "znclib")
	set(lib_type "SHARED")
	set(install_lib "znclib")
	set(znclib_pc "-L${CMAKE_INSTALL_FULL_LIBDIR} -lznc")
else()
	set(znc_link "znc")
	set(lib_type "STATIC")
	set(install_lib)
	set(znclib_pc)
endif()

configure_file("include/znc/zncconfig.h.cmake.in" "include/znc/zncconfig.h")
configure_file("test/integration/znctestconfig.h.cmake.in"
	"test/integration/znctestconfig.h")
add_subdirectory(include)
add_subdirectory(src)
add_subdirectory(modules)
add_subdirectory(test)
add_subdirectory(zz_msg)

add_custom_target(msg_after_all ALL
	COMMAND "${CMAKE_COMMAND}" -E echo
	COMMAND "${CMAKE_COMMAND}" -E echo " ZNC was successfully compiled."
	COMMAND "${CMAKE_COMMAND}" -E echo
	" Use 'make install' to install ZNC to '${CMAKE_INSTALL_PREFIX}'."
	COMMAND "${CMAKE_COMMAND}" -E echo
	VERBATIM)
add_dependencies(msg_after_all ${_all_targets})
#	@echo ""
#	@echo " ZNC was successfully compiled."
#	@echo " Use '$(MAKE) install' to install ZNC to '$(prefix)'."


configure_file("ZNCConfig.cmake.in" "ZNCConfig.cmake" @ONLY)
include(CMakePackageConfigHelpers)
write_basic_package_version_file("ZNCConfigVersion.cmake"
	COMPATIBILITY AnyNewerVersion)
install(FILES
	"${PROJECT_SOURCE_DIR}/cmake/CMakeFindDependencyMacroPC.cmake"
	"${PROJECT_SOURCE_DIR}/cmake/use_homebrew.cmake"
	"${PROJECT_BINARY_DIR}/ZNCConfig.cmake"
	"${PROJECT_BINARY_DIR}/ZNCConfigVersion.cmake"
	DESTINATION "${CMAKE_INSTALL_DATADIR}/znc/cmake")
configure_file("znc-buildmod.cmake.in" "znc-buildmod" @ONLY)
install(PROGRAMS "${PROJECT_BINARY_DIR}/znc-buildmod"
	DESTINATION "${CMAKE_INSTALL_BINDIR}")

configure_file("znc.pc.cmake.in" "znc.pc" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/znc.pc"
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

macro(summary_line text var)
	if(${var})
		list(APPEND summary_lines "${text} : yes")
	else()
		list(APPEND summary_lines "${text} : no")
	endif()
endmacro()
set(summary_lines
	"ZNC ${ZNC_VERSION}${VERSION_EXTRA}${alpha_version} configured"
	" "
	"Prefix    : ${CMAKE_INSTALL_PREFIX}")
summary_line("SSL      " "${OPENSSL_FOUND}")
summary_line("IPv6     " "${WANT_IPV6}")
summary_line("Async DNS" "${HAVE_THREADED_DNS}")
summary_line("Perl     " "${PERLLIBS_FOUND}")
summary_line("Python   " "${Python3_FOUND}")
summary_line("Tcl      " "${TCL_FOUND}")
summary_line("Cyrus    " "${CYRUS_FOUND}")
summary_line("Charset  " "${ICU_FOUND}")
summary_line("Zlib     " "${ZLIB_FOUND}")
summary_line("i18n     " "${HAVE_I18N}")
summary_line("Argon2   " "${ZNC_HAVE_ARGON}")

include(render_framed_multiline)
render_framed_multiline("${summary_lines}")

message("")
message("Now you can run 'make' to compile ZNC")
message("")
