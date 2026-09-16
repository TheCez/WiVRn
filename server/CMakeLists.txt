set(U_GIT_TAG_CPP "${CMAKE_BINARY_DIR}/u_git_tag.cpp")

set(XRT_BUILD_DRIVER_ARDUINO            OFF)
set(XRT_BUILD_DRIVER_BLUBUR_S1          OFF)
set(XRT_BUILD_DRIVER_DAYDREAM           OFF)
set(XRT_BUILD_DRIVER_EUROC              OFF)
set(XRT_BUILD_DRIVER_HDK                OFF)
set(XRT_BUILD_DRIVER_HYDRA              OFF)
set(XRT_BUILD_DRIVER_NS                 OFF)
set(XRT_BUILD_DRIVER_OHMD               OFF)
set(XRT_BUILD_DRIVER_OPENGLOVES         OFF)
set(XRT_BUILD_DRIVER_PSMV               OFF)
set(XRT_BUILD_DRIVER_PSSENSE            OFF)
set(XRT_BUILD_DRIVER_PSVR               OFF)
set(XRT_BUILD_DRIVER_QWERTY             OFF)
set(XRT_BUILD_DRIVER_REMOTE             OFF)
set(XRT_BUILD_DRIVER_RIFT               OFF)
set(XRT_BUILD_DRIVER_RIFT_S             OFF)
set(XRT_BUILD_DRIVER_ROKID              OFF)
set(XRT_BUILD_DRIVER_SIMULATED          OFF)
set(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE ${WIVRN_FEATURE_STEAMVR_LIGHTHOUSE})
set(XRT_BUILD_DRIVER_SOLARXR            ${WIVRN_FEATURE_SOLARXR})
set(XRT_BUILD_DRIVER_TWRAP              OFF)
set(XRT_BUILD_DRIVER_VF                 OFF)
set(XRT_BUILD_DRIVER_VIVE               OFF)
set(XRT_BUILD_DRIVER_WMR                OFF)
set(XRT_BUILD_DRIVER_XREAL_AIR          OFF)
set(XRT_BUILD_SAMPLES                   OFF)

set(XRT_FEATURE_COMPOSITOR_NULL                   OFF)
set(XRT_FEATURE_DEBUG_GUI                         ${WIVRN_FEATURE_DEBUG_GUI})
set(XRT_FEATURE_SERVICE                           ON)
set(XRT_FEATURE_SERVICE_SYSTEMD                   OFF)
set(XRT_FEATURE_SLAM                              OFF)
set(XRT_FEATURE_STEAMVR_PLUGIN                    OFF)
set(XRT_FEATURE_WINDOW_PEEK                       OFF)
set(XRT_FEATURE_OPENXR_ACTIVE_ACTION_SET_PRIORITY ON)
set(XRT_FEATURE_RENDERDOC                         ${WIVRN_FEATURE_RENDERDOC})

set(XRT_FEATURE_OPENXR_BODY_TRACKING_FB          ON)
set(XRT_FEATURE_OPENXR_FACE_TRACKING_ANDROID     ON)
set(XRT_FEATURE_OPENXR_FACE_TRACKING2_FB         ON)
set(XRT_FEATURE_OPENXR_FACIAL_TRACKING_HTC       ON)
set(XRT_FEATURE_OPENXR_HAND_TRACKING_EXT         ON)
set(XRT_FEATURE_OPENXR_INTERACTION_EXT_HAND      ON)
set(XRT_FEATURE_OPENXR_INTERACTION_EXT_PALM_POSE ON)
set(XRT_FEATURE_OPENXR_INTERACTION_BYTEDANCE     ON)
set(XRT_FEATURE_OPENXR_INTERACTION_VIVE_FOCUS3   ON)
set(XRT_FEATURE_OPENXR_INTERACTION_TOUCH_PRO     ON)
set(XRT_FEATURE_OPENXR_INTERACTION_TOUCH_PLUS    ON)
# VRChat's Android build only suggests bindings for Steam Frame's profile
# (never falls back to Oculus Touch), so without this its controllers are unbound.
set(XRT_FEATURE_OPENXR_INTERACTION_VALVE_FRAME_CONTROLLER ON)
set(XRT_FEATURE_OPENXR_LAYER_COLOR_SCALE_BIAS    ON)
set(XRT_FEATURE_OPENXR_LAYER_DEPTH              OFF)
set(XRT_FEATURE_OPENXR_VARJO_QUAD_VIEWS         OFF)

set(XRT_HAVE_BLUETOOTH               OFF)
set(XRT_HAVE_GST                     OFF)

set(XRT_HAVE_JPEG                    OFF)
if (NOT WIVRN_FEATURE_DEBUG_GUI)
	set(XRT_HAVE_SDL2                OFF)
endif()
set(XRT_HAVE_OPENCV                  OFF)
set(XRT_HAVE_DBUS                    OFF)
set(XRT_HAVE_LIBUDEV                 OFF)
set(XRT_HAVE_ONNXRUNTIME             OFF)

set(XRT_HAVE_SYSTEMD                 OFF)
set(XRT_INSTALL_SYSTEMD_UNIT_FILES   OFF)

set(BUILD_DOC                        OFF)
set(BUILD_TESTING                    OFF)

set(XRT_HAVE_SYSTEM_CJSON            OFF)

# Drive Monado's tracing from the same switch as WiVRn's: WIVRN_TRACE_MONADO builds Monado
# with percetto so its u_trace markers join the WIVRN_TRACING session (system backend).
# Monado does not fetch percetto; it must be discoverable (pkg-config / installed, or
# -DPercetto_ROOT_DIR=...). If it is missing, Monado's option_with_deps fails the configure
# with "XRT_HAVE_PERCETTO specified but not available". Linking percetto against WiVRn's own
# Perfetto SDK copy (to drop this requirement and the second runtime) is a planned follow-up.
if(WIVRN_TRACE_MONADO)
	message(STATUS "WIVRN_TRACE_MONADO=ON: building Monado with percetto tracing (percetto must be available)")
	set(XRT_FEATURE_TRACING ON)
	set(XRT_HAVE_PERCETTO   ON)
endif()

set(XRT_IPC_MSG_SOCK_FILENAME     "wivrn/comp_ipc" CACHE STRING "Service socket filename")
set(XRT_IPC_SERVICE_PID_FILENAME  "wivrn.pid"      CACHE STRING "Service pidfile filename")
set(XRT_OXR_RUNTIME_SUFFIX        "wivrn"          CACHE STRING "OpenXR client library suffix")

FetchContent_MakeAvailable(monado)

# Rootless custom Vulkan driver loading (adrenotools/Turnip) -- Android only.
if (ANDROID)
	FetchContent_MakeAvailable(adrenotools)
endif()

# VK_LAYER_KHRONOS_synchronization2 -- Android only (see top-level
# CMakeLists.txt's own comment). BUILD_TESTS off for all three (we only
# need the layer's own shared library, not its or its dependencies' test
# suites); WSI options are irrelevant on Android already (Vulkan-ExtensionLayer's
# own CMakeLists.txt excludes them there).
if (ANDROID)
	set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(vulkan_extensionlayer)
endif()

if (WIVRN_BUILD_SERVER)
	# Shared between both target types below.
	set(_wivrn_server_sources
			accept_connection.cpp
			hostname.cpp
			ipc_server_cb.cpp
			target_instance_wivrn.cpp
			wivrn_ipc.cpp

			audio/audio_setup.cpp

			compositor/compositor.cpp
			compositor/layer_squasher.cpp
			compositor/foveation.cpp
			compositor/pacer.cpp

			encoder/encoder_settings.cpp
			encoder/idr_handler.cpp
			encoder/video_encoder.cpp
			encoder/video_encoder_raw.cpp

			driver/app_pacer.cpp
			driver/clock_offset.cpp
			driver/configuration.cpp
			driver/hand_joints_list.cpp
			driver/pose_list.cpp
			driver/tracking_control.cpp
			driver/view_list.cpp
			driver/wivrn_hmd.cpp
			driver/wivrn_controller.cpp
			driver/wivrn_eye_tracker.cpp
			driver/wivrn_gamepad.cpp
			driver/wivrn_android_face_tracker.cpp
			driver/wivrn_body_tracker.cpp
			driver/wivrn_fb_face2_tracker.cpp
			driver/wivrn_htc_face_tracker.cpp
			driver/wivrn_generic_tracker.cpp
			driver/wivrn_session.cpp
			driver/wivrn_connection.cpp
			driver/xrt_cast.cpp

			utils/wivrn_vk_bundle.cpp
			utils/wivrn_trace.cpp
			utils/gpu_timestamp_pool.cpp
			utils/frame_timing_stats.cpp
		)

	# WIVRN_ANDROID_JNI builds a shared library instead of an executable, for
	# System.loadLibrary() from the Service wrapper (android/wivrn_server_jni.cpp).
	if(ANDROID AND WIVRN_ANDROID_JNI)
		add_library(wivrn-server SHARED ${_wivrn_server_sources})
	else()
		add_executable(wivrn-server ${_wivrn_server_sources})
	endif()

	# main.cpp and its desktop-only support files (D-Bus dashboard IPC,
	# avahi, sleep_inhibitor, start_application/systemd_unit) have no Android
	# equivalent. wivrn_uinput.cpp opens /dev/uinput directly, which an
	# unprivileged Android app can't do, so it gets a no-op implementation instead.
	if(ANDROID)
		target_sources(wivrn-server PRIVATE
				driver/wivrn_uinput_android.cpp
				android/wivrn_ipc_socket_monado_stub.cpp
				utils/vulkan_loader.cpp
			)
		if(WIVRN_ANDROID_JNI)
			target_sources(wivrn-server PRIVATE android/wivrn_server_jni.cpp)
		else()
			target_sources(wivrn-server PRIVATE android/wivrn_server_android_stub_main.cpp)
		endif()
		# NDK libc++ only has std::jthread/stop_token behind this flag
		# (checked against NDK r28b/Clang 19). zoned_time/enumerate_view
		# aren't available at all -- see driver/configuration.cpp and
		# utils/enumerate_polyfill.h.
		target_compile_options(wivrn-server PRIVATE -fexperimental-library)
		# Without this, vulkan_core.h doesn't declare the Android platform
		# Vulkan extensions/structs at all; the AHardwareBuffer import
		# extension then silently never gets requested at device creation,
		# so the driver never populates its function pointer -- a null-pointer
		# call at the callsite, not a link error, which is why this was missed.
		target_compile_definitions(wivrn-server PRIVATE VK_USE_PLATFORM_ANDROID_KHR)

		# Required to unlock vk::raii::Context's PFN_vkGetInstanceProcAddr
		# constructor -- the default only exposes vulkan-hpp's own internal
		# loader, which can't be redirected to a custom driver (adrenotools/Turnip,
		# see utils/vulkan_loader.cpp).
		target_compile_definitions(wivrn-server PRIVATE VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0)
		target_link_libraries(wivrn-server PRIVATE adrenotools)
	else()
		target_sources(wivrn-server PRIVATE
				active_runtime.cpp
				avahi_publisher.cpp
				main.cpp
				sleep_inhibitor.cpp
				start_application.cpp
				start_systemd_unit.cpp
				driver/wivrn_uinput.cpp
			)
	endif()
	target_compile_features(wivrn-server PRIVATE cxx_std_20)
	target_compile_definitions(wivrn-server PRIVATE VULKAN_HPP_NO_CONSTRUCTORS)

	target_include_directories(wivrn-server SYSTEM PRIVATE ${monado_SOURCE_DIR}/src/xrt/compositor/)
	target_include_directories(wivrn-server PRIVATE .)

	target_link_libraries(wivrn-server PRIVATE CLI11::CLI11 xrt-external-renderdoc)

	if (WIVRN_FEATURE_STEAMVR_LIGHTHOUSE)
		target_include_directories(wivrn-server SYSTEM PRIVATE ${monado_SOURCE_DIR}/src/xrt/drivers/steamvr_lh/)
		target_link_libraries(wivrn-server PRIVATE drv_steamvr_lh)
	endif()

	if (WIVRN_FEATURE_SOLARXR)
		target_include_directories(wivrn-server SYSTEM PRIVATE ${monado_SOURCE_DIR}/src/xrt/drivers/solarxr/)
		target_link_libraries(wivrn-server PRIVATE drv_solarxr)
	endif()

	if(WIVRN_USE_NVENC)
		target_sources(
			wivrn-server
			PRIVATE encoder/video_encoder_nvenc.cpp
				encoder/video_encoder_nvenc_shared_state.cpp
			)
	endif()

	if(WIVRN_USE_VAAPI)
		target_sources(
			wivrn-server
			PRIVATE encoder/ffmpeg/video_encoder_ffmpeg.cpp
				encoder/ffmpeg/video_encoder_va.cpp
				encoder/ffmpeg/ffmpeg_helper.cpp
			)
		target_link_libraries(wivrn-server PRIVATE PkgConfig::LIBAV PkgConfig::LIBDRM)
	endif()

	if(WIVRN_USE_VULKAN_ENCODE)
		target_sources(wivrn-server PRIVATE
			encoder/video_encoder_vulkan.cpp
			encoder/video_encoder_vulkan_h264.cpp
			encoder/video_encoder_vulkan_h265.cpp
			)
	endif()

	if(WIVRN_USE_X264)
		target_sources(wivrn-server PRIVATE encoder/video_encoder_x264.cpp)
		target_link_libraries(wivrn-server PRIVATE PkgConfig::X264)
	endif()

	if(WIVRN_USE_MEDIACODEC)
		target_sources(wivrn-server PRIVATE encoder/video_encoder_mediacodec.cpp)
		find_library(MEDIANDK_LIBRARY NAMES mediandk REQUIRED)
		target_link_libraries(wivrn-server PRIVATE ${MEDIANDK_LIBRARY})
	endif()

	if(WIVRN_USE_PERFETTO)
		find_path(PERFETTO_SDK_DIR
			NAMES perfetto.h
			PATHS /usr/share/perfetto/sdk
			DOC "Perfetto amalgamated SDK directory")
		if(NOT PERFETTO_SDK_DIR)
			message(FATAL_ERROR "WIVRN_USE_PERFETTO=ON but Perfetto SDK (perfetto.h) not found. Install perfetto-sdk.")
		endif()
		add_library(wivrn-perfetto-sdk STATIC ${PERFETTO_SDK_DIR}/perfetto.cc)
		target_include_directories(wivrn-perfetto-sdk PUBLIC ${PERFETTO_SDK_DIR})
		target_compile_options(wivrn-perfetto-sdk PRIVATE -w)
		target_link_libraries(wivrn-perfetto-sdk PUBLIC pthread dl)
		target_link_libraries(wivrn-server PRIVATE wivrn-perfetto-sdk)
		target_compile_definitions(wivrn-server PRIVATE WIVRN_USE_PERFETTO)
		if(WIVRN_TRACE_MONADO)
			target_compile_definitions(wivrn-server PRIVATE WIVRN_TRACE_MONADO)
		endif()
	endif()

	if(WIVRN_USE_PIPEWIRE)
		target_sources(
			wivrn-server
			PRIVATE audio/audio_pipewire.cpp
			)
		target_link_libraries(wivrn-server PRIVATE PkgConfig::libpipewire)
	endif()

	# D-Bus dashboard IPC (GDBus codegen + glib + libnotify): desktop-only,
	# only used by main.cpp, which itself is excluded on Android above.
	if(NOT ANDROID)
	# Generate dbus interface code as a separate target to avoid warnings in generated code
	add_custom_command(OUTPUT wivrn_server_dbus.c wivrn_server_dbus.h
			   COMMAND ${GDBUS_CODEGEN} ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/io.github.wivrn.Server.xml --interface-prefix io.github.wivrn --generate-c-code wivrn_server_dbus --c-namespace Wivrn
			   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/io.github.wivrn.Server.xml)
	add_custom_target(wivrn-server-dbus-codegen ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wivrn_server_dbus.c ${CMAKE_CURRENT_BINARY_DIR}/wivrn_server_dbus.h)
	add_library(wivrn-server-dbus STATIC
		${CMAKE_CURRENT_BINARY_DIR}/wivrn_server_dbus.c
		${CMAKE_CURRENT_BINARY_DIR}/wivrn_server_dbus.h
		)
	target_include_directories(wivrn-server-dbus PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
	add_dependencies(wivrn-server-dbus wivrn-server-dbus-codegen)
	target_link_libraries(wivrn-server-dbus PUBLIC PkgConfig::glib2)
	target_link_libraries(wivrn-server PUBLIC wivrn-server-dbus)
	target_link_libraries(wivrn-server PUBLIC PkgConfig::libnotify)

	add_custom_command(OUTPUT systemd_manager.c systemd_manager.h
			   COMMAND ${GDBUS_CODEGEN} ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/org.freedesktop.systemd1.Manager.xml --interface-prefix org.freedesktop.systemd1 --generate-c-code systemd_manager --c-namespace systemd
			   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/org.freedesktop.systemd1.Manager.xml )
	add_custom_command(OUTPUT systemd_unit.c systemd_unit.h
		COMMAND ${GDBUS_CODEGEN} ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/org.freedesktop.systemd1.Unit.xml --interface-prefix org.freedesktop.systemd1 --generate-c-code systemd_unit --c-namespace unit
			   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/org.freedesktop.systemd1.Manager.xml )
	target_sources(wivrn-server-dbus PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/systemd_manager.c ${CMAKE_CURRENT_BINARY_DIR}/systemd_manager.h
		${CMAKE_CURRENT_BINARY_DIR}/systemd_unit.c ${CMAKE_CURRENT_BINARY_DIR}/systemd_unit.h)
	endif()

	target_link_libraries(
		wivrn-server
		PRIVATE
			aux_os
			aux_render
			aux_util
			aux_vk
			base
			comp_multi
			comp_util
			ipc_server
			target_helpers
			xrt-external-openxr
			xrt-interfaces

			Eigen3::Eigen
			wivrn-common
			wivrn-external
			nlohmann_json::nlohmann_json
		)

	if(NOT ANDROID)
		# wivrn-common-server (Steam/Flatpak/XDG desktop app discovery and
		# launch): only used by main.cpp and the get_application_list
		# handler in driver/wivrn_session.cpp, both desktop-only paths --
		# see common/CMakeLists.txt.
		target_link_libraries(wivrn-server PRIVATE wivrn-common-server PkgConfig::AVAHI)
	endif()

	include(CompileGLSL)
	FetchContent_GetProperties(monado SOURCE_DIR _monado_src)
	wivrn_compile_glsl(wivrn-server
		TARGET_ENV vulkan1.3
		SHADERS
			"${_monado_src}/src/xrt/auxiliary/render/shaders/layer.comp"
			"${CMAKE_CURRENT_SOURCE_DIR}/compositor/shaders/foveation.comp"
		)

	target_compile_definitions(wivrn-server PRIVATE JSON_DIAGNOSTICS=1)

	configure_file(dist/wivrn.service.in wivrn.service)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wivrn.service
		DESTINATION lib/systemd/user/)

	install(FILES dist/firewalld-wivrn.xml
		RENAME wivrn.xml
		DESTINATION lib/firewalld/services)

	install(TARGETS wivrn-server)
endif()

if (WIVRN_BUILD_SERVER_LIBRARY)
	set_target_properties(${RUNTIME_TARGET} PROPERTIES EXCLUDE_FROM_ALL FALSE)
	set_target_properties(monado PROPERTIES EXCLUDE_FROM_ALL FALSE)

	set(_extra_args)
	if (WIVRN_OPENXR_MANIFEST_TYPE STREQUAL "absolute")
		set(_extra_args ABSOLUTE_RUNTIME_PATH)
	elseif (WIVRN_OPENXR_MANIFEST_TYPE STREQUAL "relative")
		set(_extra_args RUNTIME_DIR_RELATIVE_TO_MANIFEST ../../../${CMAKE_INSTALL_LIBDIR}/wivrn)
	endif()

	set_target_properties(monado PROPERTIES OUTPUT_NAME "monado_wivrn")
	# Workaround as generate_openxr_runtime_manifest_at_install uses target name as library path
	add_library(monado_wivrn ALIAS monado)

	if (WIVRN_OPENXR_MANIFEST_ABI)
		if (CMAKE_C_COMPILER_ARCHITECTURE_ID MATCHES "i.86")
			set(_filename openxr_wivrn.i686.json)
		else()
			set(_filename openxr_wivrn.${CMAKE_C_COMPILER_ARCHITECTURE_ID}.json)
		endif()
	else()
		set(_filename openxr_wivrn.json)
	endif()

	generate_openxr_runtime_manifest_at_install(
		${_extra_args}
		RUNTIME_TARGET ${RUNTIME_TARGET}
		DESTINATION share/openxr/1
		RELATIVE_RUNTIME_DIR ${CMAKE_INSTALL_LIBDIR}/wivrn
		LIBMONADO monado_wivrn
		OUT_FILENAME ${_filename}
		MANIFEST_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/dist/openxr_manifest.in.json"
		)

	install(TARGETS ${RUNTIME_TARGET} monado
		LIBRARY
		DESTINATION ${CMAKE_INSTALL_LIBDIR}/wivrn
		COMPONENT Runtime)
endif()
