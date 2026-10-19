cmake_minimum_required(VERSION 3.20)
project(plasma-notification-mirror LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui DBus)
find_package(LayerShellQt REQUIRED)
find_package(KF6Service REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_library(notification-support STATIC
    src/notification.cpp
    src/config.cpp
    src/popuplayout.cpp
    src/notificationactions.cpp
)
target_include_directories(notification-support PUBLIC src)
target_link_libraries(notification-support PUBLIC Qt6::Core Qt6::Gui Qt6::DBus KF6::Service PkgConfig::LIBSYSTEMD)

add_executable(plasma-notification-mirror src/main.cpp)

target_link_libraries(plasma-notification-mirror
    PRIVATE
        notification-support
        LayerShellQt::Interface
        PkgConfig::LIBSYSTEMD
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(notification-tests tests/notification_test.cpp)
    set_target_properties(notification-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(notification-tests PRIVATE notification-support Qt6::Test)
    add_executable(config-tests tests/config_test.cpp)
    set_target_properties(config-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(config-tests PRIVATE notification-support Qt6::Test)
    add_test(NAME config-tests COMMAND config-tests)
    add_executable(layout-tests tests/layout_test.cpp)
    set_target_properties(layout-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(layout-tests PRIVATE notification-support Qt6::Test)
    add_test(NAME layout-tests COMMAND layout-tests)
    add_test(NAME notification-tests
        COMMAND dbus-run-session -- $<TARGET_FILE:notification-tests>)
    set_tests_properties(notification-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

include(GNUInstallDirs)

install(TARGETS plasma-notification-mirror
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES config/config.example.ini
    DESTINATION ${CMAKE_INSTALL_DATADIR}/plasma-notification-mirror
)

install(FILES systemd/plasma-notification-mirror.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user
)
