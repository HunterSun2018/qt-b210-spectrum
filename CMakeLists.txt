cmake_minimum_required(VERSION 3.21)

project(QT6B210Spectrum VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
find_package(Boost REQUIRED)

pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)
pkg_check_modules(UHD REQUIRED IMPORTED_TARGET uhd)

qt_standard_project_setup()

qt_add_executable(qt6_b210_spectrum
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/SdrWorker.cpp
    src/SdrWorker.h
    src/FftProcessor.cpp
    src/FftProcessor.h
    src/SpectrumWidget.cpp
    src/SpectrumWidget.h
    src/WaterfallWidget.cpp
    src/WaterfallWidget.h
    src/IqFftwProcessor.c
    src/IqFftwProcessor.h
    src/SignalProcessor.cpp
    src/SignalProcessor.h
)

target_include_directories(qt6_b210_spectrum  PRIVATE src)

target_link_libraries(qt6_b210_spectrum PRIVATE
        Boost::boost
        Qt6::Widgets
        PkgConfig::ALSA
        PkgConfig::FFTW3
        PkgConfig::UHD
        fftw3f
)
